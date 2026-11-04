// Android port, Vulkan backend: device, memory pools, submissions, completion (see gfx_vk.h).
//   Memory pools: NVN gives the GPU the game's own memory (GPU address = CPU address). Each pool's pages are replaced by an AHardwareBuffer mapped at the same
//   address (its dma-buf mmapped MAP_FIXED, contents copied over), and the same AHardwareBuffer is imported as a VkBuffer: the game's pointer is GPU memory,
//   CPU-cached and coherent on Adreno (measured: probes/vkzc.c). A pool the allocator refuses keeps its anonymous pages and gets a GPU copy refreshed before
//   every submission (vk_pools_sync), correct but slow, reported once.
//   Submissions: a ring of slots, each with a command pool (upload + main command buffers), descriptor pools, scratch memory and objects freed once the GPU has
//   passed it. A submission signals a timeline semaphore with its fence; a completion thread waits for it and then applies what the CPU may see of its work
//   (event writes, counter reports), as nvn_mtl.m does in its completion handler.
#define _GNU_SOURCE
#include "gfx_vk.h"
#include "sync_trace.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "copy_queue.h"
#include "gpu_segments.h"
#include "vk_pace_budget.h"
#include "pool_write_ranges.h"
#include "mali_probe.h"
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VK_DEF(f) PFN_##f f;
VK_FNS(VK_DEF) VK_DEV_FNS(VK_DEF)
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
// Large uniform shadows may be filled while replay continues. Every GPU submit
// and scratch/mapping lifetime boundary drains these copies first.
static vk_copy_queue g_ucopy = VK_COPY_QUEUE_INIT;
static int g_ucopy_on;
static _Atomic int g_ucopy_shadow_pools;
uint64_t g_ucopy_jobs, g_ucopy_bytes;
_Atomic uint64_t g_ucopy_wait_ns;
static uint64_t ucopy_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void ucopy_begin(void) { pthread_setname_np(pthread_self(), "dm-ub-copy"); }
static void ucopy_wait(uint64_t sequence) {
    if (atomic_load_explicit(&g_ucopy.done, memory_order_acquire) >= sequence) return;
    uint64_t at = ucopy_now(); vk_copyq_wait(&g_ucopy, sequence);
    atomic_fetch_add_explicit(&g_ucopy_wait_ns, ucopy_now() - at, memory_order_relaxed);
}
static void ucopy_drain(void) { ucopy_wait(vk_copyq_head(&g_ucopy)); }
void vk_uniform_copy_config(int enabled) { if (!enabled) ucopy_drain(); g_ucopy_on = enabled; }
vkdev_t V;
void vk_compile_thread(const char *name) {
    pthread_setname_np(pthread_self(), name);
    // A worker spawned by the prime-pinned replay would inherit its narrow
    // affinity. Restore the app leader's allowed mask so idle cores can compile.
    cpu_set_t allowed;
    if (!sched_getaffinity(getpid(), sizeof allowed, &allowed)) sched_setaffinity(0, sizeof allowed, &allowed);
    setpriority(PRIO_PROCESS, 0, 10);
}
pthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;   // VkQueue access is externally synchronized: submissions and presents

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want && !(V.mem.memoryTypes[i].propertyFlags & avoid)) return i;
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static int is_on(const char *const *on, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(on[i], name)) return 1; return 0; }
// a feature struct as vk_init queries it (sType, pNext, then VkBool32s; the largest, descriptor indexing, has 20); featx: its VkBool32s, all off if it is not chained
typedef struct { VkBaseOutStructure h; VkBool32 b[24]; } featx_t;
static VkBool32 *featx(featx_t *x, uint32_t n, VkStructureType s) { static VkBool32 off[24]; for (uint32_t i = 0; i < n; i++) if (x[i].h.sType == s) return x[i].b; return off; }   // (off: only 0 is written to it)

static void completion_start(void);
static void slots_init(void);
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_validation_msg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u;   // each distinct message ID logged its first 3 times
    enum { NID = 512 }; static int32_t ids[NID]; static uint8_t cnt[NID]; static int nid; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); int k = 0; for (; k < nid; k++) if (ids[k] == d->messageIdNumber) break;
    if (k == nid && nid < NID) ids[nid++] = d->messageIdNumber;
    int show = k < NID && cnt[k] < 3; if (k < NID && cnt[k] < 255) cnt[k]++; pthread_mutex_unlock(&mu);
    if (show) VKLOG("[RED] validation %s: %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

void vk_init(void) {
    static int done; if (done) return; done = 1;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) die("[vk] libvulkan.so: %s", dlerror());
    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!p_vkGetInstanceProcAddr) die("[RED] [vk] driver has no vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)p_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    const char *iext[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmhost", 1, "nxnative", 1, VK_API_VERSION_1_3};
    // DM_VK_VALIDATE=1: Khronos's validation layer, which the APK carries only when built with it (mkapk.py --extra-so); its messages go to the log
    int validate = getenv("DM_VK_VALIDATE") && atoi(getenv("DM_VK_VALIDATE"));
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, validate ? 1u : 0u, layers, validate ? 3u : 2u, iext};
    VkResult ir = vkCreateInstance(&ici, NULL, &V.inst);
    if (ir && validate) { VKLOG("[RED] validation layer not available (%d): running without it", ir); validate = 0; ici.enabledLayerCount = 0; ici.enabledExtensionCount = 2; ir = vkCreateInstance(&ici, NULL, &V.inst); }
    VKCHK(ir);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT)p_vkGetInstanceProcAddr(V.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, vk_validation_msg, NULL};
        VkDebugUtilsMessengerEXT m; if (mk && !mk(V.inst, &mi, NULL, &m)) VKLOG("validation layer on");
    }
#define VK_LOADI(f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f);
    VK_FNS(VK_LOADI)
    uint32_t n = 1; VKCHK(vkEnumeratePhysicalDevices(V.inst, &n, &V.pd)); if (!n) die("[vk] no GPU");
    vkGetPhysicalDeviceProperties(V.pd, &V.props); vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mem);
    // the device names itself before any check below can stop: a Moto g 5G 2024's log (Adreno 619) ended at the timeline check with no GPU or driver named.
    // driver: as Qualcomm encodes it (VK_MAKE_VERSION: 512.530.0), with the raw value for other vendors
    { uint32_t d = V.props.driverVersion;
      VKLOG("GPU %s (vendor %#x, device %#x), Vulkan %u.%u.%u, driver %u.%u.%u (%#x)", V.props.deviceName, V.props.vendorID, V.props.deviceID, VK_API_VERSION_MAJOR(V.props.apiVersion),
            VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), d >> 22, d >> 12 & 0x3ff, d & 0xfff, d); }
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qp);
    V.qf = 0; while (V.qf < qn && (qp[V.qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) V.qf++;
    if (V.qf == qn) die("[vk] no graphics + compute queue");
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, ex);
    const char *want[] = {"VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_EXT_custom_border_color",
                          "VK_EXT_robustness2", "VK_EXT_depth_clip_control", "VK_EXT_provoking_vertex", "VK_EXT_index_type_uint8", "VK_EXT_transform_feedback",
                          "VK_GOOGLE_display_timing", "VK_EXT_depth_range_unrestricted"};
    const char *on[64]; uint32_t non = 0;
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) if (has_ext(ex, ne, want[i])) on[non++] = want[i]; else VKLOG("device has no %s", want[i]);
    V.has_depth_range_unrestricted = has_ext(ex, ne, "VK_EXT_depth_range_unrestricted");
    // A Vulkan 1.1 or 1.2 device (Legion Y700 2023, Adreno 730: its stock driver reports 1.1) has what the replay uses from 1.2 / 1.3 as extensions: they are enabled
    // when present, so the features chained below and the functions loaded by their KHR names are valid there. A 1.3 device needs none of them
    int v12 = V.props.apiVersion >= VK_API_VERSION_1_2, v13 = V.props.apiVersion >= VK_API_VERSION_1_3; uint32_t npromo = 0;
    static const char *promo12[] = {"VK_KHR_create_renderpass2", "VK_KHR_imageless_framebuffer", "VK_KHR_image_format_list", "VK_KHR_depth_stencil_resolve", "VK_KHR_timeline_semaphore",
        "VK_KHR_shader_float16_int8", "VK_KHR_8bit_storage", "VK_EXT_descriptor_indexing", "VK_EXT_scalar_block_layout", "VK_KHR_uniform_buffer_standard_layout",
        "VK_KHR_shader_float_controls", "VK_KHR_spirv_1_4", "VK_KHR_vulkan_memory_model", "VK_KHR_separate_depth_stencil_layouts", "VK_EXT_host_query_reset",
        "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_draw_indirect_count", "VK_KHR_buffer_device_address", "VK_EXT_shader_viewport_index_layer", "VK_EXT_sampler_filter_minmax",
        "VK_KHR_shader_subgroup_extended_types", "VK_KHR_shader_atomic_int64", "VK_KHR_driver_properties"};
    static const char *promo13[] = {"VK_EXT_image_robustness", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_shader_demote_to_helper_invocation",
        "VK_EXT_subgroup_size_control", "VK_KHR_copy_commands2", "VK_KHR_maintenance4", "VK_KHR_format_feature_flags2", "VK_EXT_extended_dynamic_state",
        "VK_KHR_zero_initialize_workgroup_memory", "VK_KHR_shader_terminate_invocation", "VK_EXT_inline_uniform_block", "VK_EXT_pipeline_creation_cache_control",
        "VK_EXT_texel_buffer_alignment", "VK_EXT_4444_formats", "VK_KHR_shader_integer_dot_product", "VK_EXT_private_data"};
    for (unsigned i = 0; !v12 && i < sizeof promo12 / sizeof *promo12 && non < 64; i++) if (has_ext(ex, ne, promo12[i])) { on[non++] = promo12[i]; npromo++; }
    for (unsigned i = 0; !v13 && i < sizeof promo13 / sizeof *promo13 && non < 64; i++) if (has_ext(ex, ne, promo13[i])) { on[non++] = promo13[i]; npromo++; }
    // features: what the replay uses when the device has it. VkPhysicalDeviceVulkan11/12/13Features are valid only on a 1.2 / 1.3 device (a 1.1 driver ignores
    // them and nothing would be enabled); below that, the same features come through Vulkan 1.1's own structs and those of the extensions enabled above (xf)
    static const struct { const char *ext; VkStructureType s; } XF[] = {   // ext NULL: Vulkan 1.1 core, VkPhysicalDeviceVulkan11Features's members
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES},
        {"VK_KHR_imageless_framebuffer", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES}, {"VK_KHR_timeline_semaphore", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES},
        {"VK_KHR_shader_float16_int8", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}, {"VK_KHR_8bit_storage", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES},
        {"VK_EXT_descriptor_indexing", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES}, {"VK_EXT_scalar_block_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES},
        {"VK_KHR_uniform_buffer_standard_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES}, {"VK_KHR_vulkan_memory_model", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES},
        {"VK_KHR_separate_depth_stencil_layouts", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES}, {"VK_EXT_host_query_reset", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES},
        {"VK_KHR_buffer_device_address", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}, {"VK_KHR_shader_subgroup_extended_types", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES},
        {"VK_KHR_shader_atomic_int64", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES},   // (mirror clamp, indirect count, minmax, viewport layer: the extension is the feature)
        {"VK_KHR_dynamic_rendering", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES}, {"VK_KHR_synchronization2", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES},
        {"VK_EXT_shader_demote_to_helper_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES}, {"VK_EXT_subgroup_size_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES},
        {"VK_KHR_maintenance4", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES}, {"VK_KHR_zero_initialize_workgroup_memory", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES},
        {"VK_KHR_shader_terminate_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES}, {"VK_EXT_inline_uniform_block", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES},
        {"VK_EXT_pipeline_creation_cache_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES}, {"VK_KHR_shader_integer_dot_product", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES},
        {"VK_EXT_private_data", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES}};   // (VK_EXT_image_robustness: robustImageAccess stays off, as on 1.3)
    uint32_t nxf = sizeof XF / sizeof *XF; featx_t xf[sizeof XF / sizeof *XF]; memset(xf, 0, sizeof xf); VkBaseOutStructure *xs = NULL;
    for (int i = (int)nxf - 1; i >= 0; i--) if (XF[i].ext ? is_on(on, non, XF[i].ext) : !v12) { xf[i].h.sType = XF[i].s; xf[i].h.pNext = xs; xs = &xf[i].h; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, xs};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? (void *)&f13 : xs};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v12 ? (void *)&f11 : xs};
    vkGetPhysicalDeviceFeatures2(V.pd, &ff);
    // without timeline semaphores (Vulkan 1.1 drivers: the Adreno 618 / 619 of 2021, 512.530) each submission signals a fence of a ring instead (slots_init).
    // DM_VK_X_FENCES=1 (start-up): the fences on a device that has them; with DM_VK_X_BC_CPU=1 below, a device of today stands in for those drivers
    { const char *e = getenv("DM_VK_X_FENCES");
      V.has_timeline = (v12 ? f12.timelineSemaphore : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)[0]) && !(e && atoi(e)); }
    if (!V.has_timeline) VKLOG("no timeline semaphores (Vulkan 1.2 or VK_KHR_timeline_semaphore): submissions signal fences");
    // dynamic rendering is 1.3 or its extension with the feature on; a loaded pointer does not tell, since the loader hands out a trampoline for core functions the driver lacks
    V.has_dynrender = v13 ? f13.dynamicRendering : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)[0];
    // imageless framebuffers need render pass 2 (gfx_vk_exec.c rp_get): core in 1.2, else its extension
    V.has_imageless = v12 ? f12.imagelessFramebuffer : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES)[0] && is_on(on, non, "VK_KHR_create_renderpass2");
    V.has_format_list = v12 || is_on(on, non, "VK_KHR_image_format_list");
    if (!V.has_imageless && !V.has_dynrender) die("[vk] the device has neither dynamic rendering nor imageless framebuffers with render pass 2: passes cannot be begun");
    if (!v13) VKLOG("Vulkan %u.%u device: %u extensions enabled for the features promoted to 1.2 / 1.3; dynamic rendering %s", VK_API_VERSION_MAJOR(V.props.apiVersion),
                    VK_API_VERSION_MINOR(V.props.apiVersion), npromo, V.has_dynrender ? "yes" : "no (render pass objects only)");
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT fr2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &fcb};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fdc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &fr2};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &fdc};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &fu8};
    VkPhysicalDeviceFeatures2 fx = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fxf};
    vkGetPhysicalDeviceFeatures2(V.pd, &fx);
    VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    const char *db_option = getenv("DM_VK_DESCRIPTOR_BUFFER");
    if (db_option && atoi(db_option) && has_ext(ex, ne, "VK_EXT_descriptor_buffer")) {
        VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fdb};
        vkGetPhysicalDeviceFeatures2(V.pd, &query);
        V.dbprops.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &V.dbprops};
        vkGetPhysicalDeviceProperties2(V.pd, &properties);
        int address = v12 ? f12.bufferDeviceAddress : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[0];
        V.descriptor_buffer = fdb.descriptorBuffer && address && non < 64 &&
            V.dbprops.combinedImageSamplerDescriptorSingleArray && V.dbprops.descriptorBufferOffsetAlignment &&
            V.dbprops.maxDescriptorBufferBindings >= 5 && V.dbprops.maxResourceDescriptorBufferBindings >= 5 &&
            V.dbprops.maxSamplerDescriptorBufferBindings >= 5 && V.dbprops.maxSamplerDescriptorBufferRange >= 65536 &&
            V.dbprops.maxResourceDescriptorBufferRange >= 65536;
        if (V.descriptor_buffer) on[non++] = "VK_EXT_descriptor_buffer";
    }
    if (db_option && atoi(db_option)) VKLOG("descriptor-buffer transport %s", V.descriptor_buffer ? "enabled (experimental)" : "unavailable: using descriptor sets");
    // transform feedback (NVN BeginTransformFeedback; gfx_vk_exec.c xfb_*): Xenoblade Chronicles 3 captures particle state with it
    V.has_xfb = fxf.transformFeedback && has_ext(ex, ne, "VK_EXT_transform_feedback"); fxf.geometryStreams = 0;
    fr2.robustBufferAccess2 = 0; fr2.robustImageAccess2 = 0;   // nullDescriptor only: unbound slots read 0 without a dummy
    V.has_custom_border = fcb.customBorderColors && has_ext(ex, ne, "VK_EXT_custom_border_color");
    V.has_robust2 = fr2.nullDescriptor && has_ext(ex, ne, "VK_EXT_robustness2");
    V.has_depth_clip_ctl = fdc.depthClipControl && has_ext(ex, ne, "VK_EXT_depth_clip_control");
    V.has_dtiming = has_ext(ex, ne, "VK_GOOGLE_display_timing");   // frame pacing on the display (gfx_vk_present.c pace_*)
    V.has_bc = ff.features.textureCompressionBC; V.has_astc = ff.features.textureCompressionASTC_LDR; V.has_gs = ff.features.geometryShader;
    { const char *e = getenv("DM_VK_X_BC_CPU"); if (e && atoi(e)) V.has_bc = 0; }   // (start-up) BC textures decoded on the CPU as on a device without them (gfx_vk_tex.c bc_decode)
    V.has_astc_decode = !V.has_bc && V.has_astc && has_ext(ex, ne, "VK_EXT_astc_decode_mode");
    if (V.has_astc_decode) on[non++] = "VK_EXT_astc_decode_mode";
    V.max_aniso = V.props.limits.maxSamplerAnisotropy;
    // enable: everything the shaders may use that the device has
    VkPhysicalDeviceFeatures en = ff.features; en.robustBufferAccess = 0; en.shaderFloat64 = 0;
    en.robustBufferAccess = vk_mali_robust(V.props.vendorID, ff.features.robustBufferAccess) ? VK_TRUE : VK_FALSE;   // Mali only (mali_probe.h)
    if (en.robustBufferAccess) VKLOG("robustBufferAccess on (Mali: out-of-range buffer reads fault without it)");
#ifndef DM_RELEASE
    else if (getenv("DM_VK_X_ROBUST")) VKLOG("DM_VK_X_ROBUST: robustBufferAccess off");
#endif
    V.vertex_stores = en.vertexPipelineStoresAndAtomics;
    f12.bufferDeviceAddressCaptureReplay = 0; f12.bufferDeviceAddressMultiDevice = 0;
    featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[1] = featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[2] = 0;   // (its extension's: the same two)
    f13.robustImageAccess = 0;
    fcb.pNext = NULL; fr2.pNext = V.has_custom_border ? (void *)&fcb : NULL; fdc.pNext = V.has_robust2 ? (void *)&fr2 : (V.has_custom_border ? (void *)&fcb : NULL);
    fu8.pNext = V.has_depth_clip_ctl ? (void *)&fdc : fdc.pNext;
    fxf.pNext = &fu8;
    VkBaseOutStructure *t = ff.pNext; while (t->pNext) t = t->pNext;   // the queried chain (core / promoted features), then the extensions'
    t->pNext = V.has_xfb ? (VkBaseOutStructure *)&fxf : (VkBaseOutStructure *)&fu8;
    fdb.descriptorBufferCaptureReplay = fdb.descriptorBufferImageLayoutIgnored = fdb.descriptorBufferPushDescriptors = 0;
    fdb.pNext = ff.pNext;
    VkPhysicalDeviceFeatures2 enf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, V.descriptor_buffer ? (void *)&fdb : ff.pNext, en};
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, V.qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enf, 0, 1, &qci, 0, NULL, non, on, NULL};
    VKCHK(vkCreateDevice(V.pd, &dci, NULL, &V.dev));
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)p_vkGetInstanceProcAddr(V.inst, "vkGetDeviceProcAddr");
#define VK_LOADD(f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f); if (!f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f "KHR"); if (!f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f);   // a promoted function by its extension's name (Vulkan 1.1 / 1.2 devices)
    VK_DEV_FNS(VK_LOADD)
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    vk_db_init();
    vk_encode_init();
    dm_sync_vulkan_install();
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    char path[1024]; const char *root = getenv("DM_ROOT"); void *data = NULL; size_t dn = 0;
    if (root && !getenv("DM_VK_NOPCACHE")) { snprintf(path, sizeof path, "%s/vk_pipeline_cache.bin", root); FILE *f = fopen(path, "rb");   // DM_VK_NOPCACHE=1: start empty
        if (f) { fseek(f, 0, SEEK_END); dn = (size_t)ftell(f); fseek(f, 0, SEEK_SET); data = malloc(dn); if (fread(data, 1, dn, f) != dn) dn = 0; fclose(f); } }
    pci.initialDataSize = dn; pci.pInitialData = data;
    if (vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache)) { pci.initialDataSize = 0; pci.pInitialData = NULL; VKCHK(vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache)); }
    free(data); free(ex);
    VKLOG("%s, Vulkan %u.%u, driver %#x; timeline semaphores %s, BC %s, ASTC %s, geometry shaders %s, nullDescriptor %s, custom border colours %s, depth clip control %s, transform feedback %s; pipeline cache %zu bytes",
          V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), V.props.driverVersion, V.has_timeline ? "yes" : "no (fences)",
          V.has_bc ? "yes" : "no (decoded on the CPU)", V.has_astc ? "yes" : "no", V.has_gs ? "yes" : "no", V.has_robust2 ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_depth_clip_ctl ? "yes" : "no", V.has_xfb ? "yes" : "no", dn);
    slots_init();
    completion_start();
}
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static size_t last;
    const char *root = getenv("DM_ROOT"); if (!root || !V.pcache) return;
    pthread_mutex_lock(&mu);
    size_t n = 0; if (vkGetPipelineCacheData(V.dev, V.pcache, &n, NULL) || !n || n == last) { pthread_mutex_unlock(&mu); return; }   // unchanged since the last save
    last = n; pthread_mutex_unlock(&mu);
    void *d = malloc(n); if (vkGetPipelineCacheData(V.dev, V.pcache, &n, d)) { free(d); return; }
    char path[1024], tmp[1100]; snprintf(path, sizeof path, "%s/vk_pipeline_cache.bin", root); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); rename(tmp, path); }
    free(d);
}

// ---------------- memory pools ----------------
#include "pool_ranges.h"
#include <limits.h>
static vkpool_t **g_pool; static int g_npool, g_pool_cap;
static vk_pool_range *g_pool_ranges;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t g_pool_gen;   // bumped under the write lock when a pool is added or removed: vk_pool_of's per-thread cache holds while it is unchanged
static uint64_t g_pool_adds, g_pool_dels;
// Exact buffer queries cache copied handles, never mutable registry pointers.
// Both address and requested length matter when registered pools overlap.
typedef struct { uint64_t addr, len, off, available; uint32_t gen; VkBuffer buf; } buf_lookup_t;
static __thread struct { buf_lookup_t entry[64]; uint64_t hits, misses, checks, failures; } t_buflookup;
static int g_buffer_cache_on;
// Grow only the pointer table and address index. Pool records stay in their original
// allocations: vk_pool_of returns them after unlocking, and its TLS cache keeps them.
// Removed slots are reused (with the same swap-delete semantics as the old array).
static void pool_reserve(void) {
    if (g_npool < g_pool_cap) return;
    if (g_pool_cap > INT_MAX / 2) die("[vk] memory pool registry capacity overflow");
    int cap = g_pool_cap ? g_pool_cap * 2 : 64;
    vkpool_t **slots = malloc((size_t)cap * sizeof *slots);
    vk_pool_range *ranges = malloc((size_t)cap * sizeof *ranges);
    vkpool_t *block = calloc((size_t)(cap - g_pool_cap), sizeof *block);
    if (!slots || !ranges || !block) {
        free(slots); free(ranges); free(block);
        die("[vk] out of host memory growing memory pool registry to %d", cap);
    }
    if (g_pool_cap) {
        memcpy(slots, g_pool, (size_t)g_pool_cap * sizeof *slots);
        memcpy(ranges, g_pool_ranges, (size_t)g_npool * sizeof *ranges);
    }
    for (int i = g_pool_cap; i < cap; i++) slots[i] = &block[i - g_pool_cap];
    free(g_pool); free(g_pool_ranges); g_pool = slots; g_pool_ranges = ranges; g_pool_cap = cap;
    VKLOG("memory pool registry: capacity %d, live %d, added %llu, removed %llu (Vulkan allocation limit %u)",
        cap, g_npool, (unsigned long long)g_pool_adds, (unsigned long long)g_pool_dels, V.props.limits.maxMemoryAllocationCount);
}
static void pool_log(const char *op, const vkpool_t *p) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_POOL_LOG"); on = e && atoi(e); }
    if (on) VKLOG("pool %s: serial %llu, range %#llx..%#llx, base %#llx, end %#llx, live %d, added %llu, removed %llu",
        op, (unsigned long long)p->serial, (unsigned long long)p->lo, (unsigned long long)p->hi,
        (unsigned long long)p->base, (unsigned long long)p->end, g_npool, (unsigned long long)g_pool_adds, (unsigned long long)g_pool_dels);
}
static int recv_fds(int sock, int *fds, int max) {
    char data[4096], ctl[CMSG_SPACE(sizeof(int) * 16)]; struct iovec iov = {data, sizeof data};
    struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) < 0) return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && n < max; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    return n;
}
static const VkBufferUsageFlags POOL_USAGE0 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
#define POOL_USAGE (POOL_USAGE0 | (V.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0))   // any pool range may be a transform feedback buffer
// Zero copy: the pages of [mem, mem+size) become an AHardwareBuffer's, imported as a VkBuffer. 0 on success
static int pool_zero_copy(vkpool_t *p, uint8_t *mem, uint64_t size) {
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (size > 0xffffffffull) return -1;
    AHardwareBuffer *ahb; if (AHardwareBuffer_allocate(&d, &ahb)) return -1;
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { AHardwareBuffer_release(ahb); return -1; }
    int fds[16], nfd = -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
    close(sv[0]); close(sv[1]);
    if (nfd < 1) { AHardwareBuffer_release(ahb); return -1; }
    // copy the pool's current contents into the buffer through a temporary mapping, then put the buffer's pages at the pool's address
    uint8_t *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    int ok = tmp != MAP_FAILED;
    if (ok) { memcpy(tmp, mem, size); munmap(tmp, size); }
    ok = ok && mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[0], 0) == mem;
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (!ok) { AHardwareBuffer_release(ahb); return -1; }
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    if (vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap) || vkCreateBuffer(V.dev, &bi, NULL, &p->buf)) { AHardwareBuffer_release(ahb); return -2; }
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf};   // the driver requires a dedicated allocation for an imported AHardwareBuffer
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
    if (vk_buffer_memory_allocate(&ai, &p->mem) || vkBindBufferMemory(V.dev, p->buf, p->mem, 0)) { vkDestroyBuffer(V.dev, p->buf, NULL); AHardwareBuffer_release(ahb); return -2; }
    p->ahb = ahb; p->zero_copy = 1;
    return 0;
}
// A zero-copy pool's dma-buf mapped again over [mem, mem+len) in one piece per chunk, read-only (nvn_dirty_defrag: the mappings its protection split never merge). 0 = done
int vk_pool_remap_ro(void *mem, size_t len) {
    ucopy_drain();
    int rc = 0; uint64_t done = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool && !rc; i++) {
        vkpool_t *p = g_pool[i]; if (p->base != (uint64_t)(uintptr_t)mem || p->end - p->base != len) continue;
        if (!p->zero_copy) { if (!p->shadow) { rc = -1; break; } done += p->hi - p->lo; continue; }   // a copied pool: nothing to map again (its protection stays as it is), and done, so defrag stops taking its pages busy
        int sv[2], fds[16], nfd = -1; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { rc = -1; break; }
        if (!AHardwareBuffer_sendHandleToUnixSocket(p->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
        close(sv[0]); close(sv[1]);
        void *at = (void *)(uintptr_t)p->lo; rc = nfd >= 1 && mmap(at, p->hi - p->lo, PROT_READ, MAP_SHARED | MAP_FIXED, fds[0], 0) == at ? 0 : -1;
        for (int k = 0; k < nfd; k++) close(fds[k]);
        if (!rc) done += p->hi - p->lo;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    return !rc && done == len ? 0 : -1;
}
static void pool_copy_back(vkpool_t *p) {   // the pages go back to anonymous memory with the buffer's contents (the game may reuse the memory)
    uint64_t size = p->hi - p->lo; uint8_t *mem = (uint8_t *)(uintptr_t)p->lo;
    uint8_t *save = malloc(size); if (save) memcpy(save, mem, size);
    mmap(mem, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (save) { memcpy(mem, save, size); free(save); }
}
// Pools over ZC_CHUNK are made of chunks of at most that size, each a pool entry with an AHardwareBuffer of its own: the Adreno 830's gralloc allocator service
// (vendor.qti.hardware.display.allocator-service, snapalloc) aborts on a BLOB of 812 MB ("ubsan: mul-overflow" in SnapConstraintManager::AlignmentToAlignedConstraints),
// and the whole Android UI restarts with it; 400 MB ones work (13 Sentinels: Aegis Rim's 0x32c00000-byte pool). A range across two chunks has no buffer (vk_pool_of)
#define ZC_CHUNK 0x10000000ull
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end);
// DM_VK_ZEROCOPY_MAX_MB=<n>: pools bigger than n MB are copied, not zero-copy, and kept whole (a copied pool takes no hardware buffer, so no chunks). The Legion Y700
// 2023's gralloc aborts the process (no error returned) allocating a hardware buffer for Unicorn Overlord's 860 MB pool, while its pools up to 250 MB are fine
static int zc_over(uint64_t size) { static uint64_t m = 1; if (m == 1) { const char *e = getenv("DM_VK_ZEROCOPY_MAX_MB"); m = e && atoi(e) > 0 ? (uint64_t)atoi(e) << 20 : 0; } return m && size > m; }
void mtl_pool_add(const void *mem, uint64_t size) {
    ucopy_drain();
    vk_init();
    if (!size) return;
    uint64_t a = (uint64_t)(uintptr_t)mem, c = ((a | size) & 0xfff) || zc_over(size) ? size : ZC_CHUNK;
    for (uint64_t o = 0; o < size; o += c) pool_add_one((const uint8_t *)mem + o, size - o < c ? size - o : c, a, a + size);
    if (size > c) VKLOG("memory pool %p +%#llx: %llu chunks of at most %#llx", mem, (unsigned long long)size, (unsigned long long)((size + c - 1) / c), c);
}
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end) {
    pthread_rwlock_wrlock(&g_pool_mu);
    pool_reserve();
    vkpool_t *p = g_pool[g_npool]; memset(p, 0, sizeof *p);
    p->lo = (uint64_t)(uintptr_t)mem; p->hi = p->lo + size; p->base = base; p->end = end; { static uint64_t ser; p->serial = ++ser; }
    int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3 : getenv("DM_VK_NO_ZEROCOPY") ? -4 : zc_over(end - base) ? -5 : pool_zero_copy(p, (uint8_t *)mem, size);
    if (zc) {
        static int said; if (!said++) VKLOG("[RED] memory pool %p +%#llx could not be zero-copy (%d): a GPU copy is refreshed before each submission (slow)", mem, (unsigned long long)size, zc);
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &p->buf));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, p->buf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
            vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vk_buffer_memory_allocate(&ai, &p->mem)); VKCHK(vkBindBufferMemory(V.dev, p->buf, p->mem, 0));
        void *m; VKCHK(vkMapMemory(V.dev, p->mem, 0, VK_WHOLE_SIZE, 0, &m)); p->shadow = m;
    }
    vk_ranges_add(g_pool_ranges, g_npool, p->lo, p->hi, g_npool);
    if (p->shadow) atomic_fetch_add_explicit(&g_ucopy_shadow_pools, 1, memory_order_relaxed);
    g_npool++; __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
    g_pool_adds++; pool_log("add", p);
    pthread_rwlock_unlock(&g_pool_mu);
    static uint64_t zc_bytes, cp_bytes; if (zc) cp_bytes += size; else zc_bytes += size;
    static int nlog; if (nlog++ < 24) VKLOG("memory pool %p +%#llx: %s (zero-copy %.1f MB, copied %.1f MB)", mem, (unsigned long long)size, zc ? "copied" : "zero-copy", zc_bytes / 1048576.0, cp_bytes / 1048576.0);
}
void mtl_pool_del(const void *mem) {
    ucopy_drain();
    for (;;) {   // every chunk of the pool
        pthread_rwlock_wrlock(&g_pool_mu);
        int i = 0; while (i < g_npool && g_pool[i]->base != (uint64_t)(uintptr_t)mem) i++;
        if (i == g_npool) { pthread_rwlock_unlock(&g_pool_mu); return; }
        vkpool_t p = *g_pool[i];
        if (p.shadow) atomic_fetch_sub_explicit(&g_ucopy_shadow_pools, 1, memory_order_relaxed);
        vk_ranges_remove(g_pool_ranges, g_npool, i, g_npool - 1);
        *g_pool[i] = *g_pool[--g_npool]; __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
        g_pool_dels++; pool_log("del", &p);
        pthread_rwlock_unlock(&g_pool_mu);
        if (p.zero_copy) pool_copy_back(&p);
        vk_defer_free_buffer(p.buf, p.mem);   // the AHardwareBuffer goes with the imported memory's last reference
        if (p.ahb) AHardwareBuffer_release(p.ahb);
    }
}
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len) {
    // the last pools this thread found, while none was added or removed: the replay looks up every buffer binding of every draw, and the read lock alone was ~6% of it
    static __thread struct { uint32_t gen; vkpool_t *p[4]; } c;   // gen: g_pool_gen + 1 (0: empty)
    uint32_t g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE) + 1;
    if (c.gen == g) for (int i = 0; i < 4; i++) { vkpool_t *p = c.p[i]; if (p && addr >= p->lo && addr + len <= p->hi) return p; }
    vkpool_t *r = NULL; int across = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    // Device sampling put over half of replay CPU time in the linear scan with thousands
    // of pools. Keep the registry's lock/lifetime rules and index its address intervals.
    int slot = vk_ranges_find(g_pool_ranges, g_npool, addr, len);
    if (slot >= 0) r = g_pool[slot];
    else {
        slot = vk_ranges_find(g_pool_ranges, g_npool, addr, 1);
        if (slot >= 0 && len <= g_pool[slot]->end - addr) across = 1;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (r) { if (c.gen != g) { memset(&c, 0, sizeof c); c.gen = g; } memmove(&c.p[1], &c.p[0], 3 * sizeof *c.p); c.p[0] = r; }
    if (!r && across) { static uint64_t n; uint64_t k = ++n; if (!(k & (k - 1))) LOG("[RED] [vk] %llu ranges across two chunks of a memory pool (ZC_CHUNK; this one %#llx +%#llx): no buffer for them", (unsigned long long)k, (unsigned long long)addr, (unsigned long long)len); }
    return r;
}
uint32_t vk_pool_gen(void) { return __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE); }   // pools added or removed (gfx_vk_exec.c's descriptor set memo: vk_pool_of's results hold while it is unchanged)
void vk_buffer_cache_config(int on) {
    on = on > 1 ? 2 : on != 0;
    if (__atomic_load_n(&g_buffer_cache_on, __ATOMIC_RELAXED) == on) return;
    __atomic_store_n(&g_buffer_cache_on, on, __ATOMIC_RELAXED);
    VKLOG("buffer range cache %s (DM_VK_BUFFER_CACHE)", on == 2 ? "verifying" : on ? "on" : "off");
}
void vk_buffer_cache_stats(uint64_t *hits, uint64_t *misses, uint64_t *checks, uint64_t *failures) {
    *hits = t_buflookup.hits; *misses = t_buflookup.misses; *checks = t_buflookup.checks; *failures = t_buflookup.failures;
}
int vk_buf_range_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, uint64_t *available) {
    uint64_t need = len ? len : 1;
    buf_lookup_t *cached = NULL;
    int mode = __atomic_load_n(&g_buffer_cache_on, __ATOMIC_RELAXED), verify = 0;
    if (mode) {
        cached = &t_buflookup.entry[((addr >> 8) ^ (addr >> 20) ^ (need >> 4)) & 63];
        uint32_t gen = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE);
        if (cached->buf && cached->gen == gen && cached->addr == addr && cached->len == need) {
            t_buflookup.hits++;
            if (mode < 2 || (t_buflookup.hits & 63)) {
                *buf = cached->buf; *off = cached->off; if (available) *available = cached->available;
                return 1;
            }
            verify = 1; t_buflookup.checks++;
        } else t_buflookup.misses++;
    }
    pthread_rwlock_rdlock(&g_pool_mu);
    int slot = vk_ranges_find(g_pool_ranges, g_npool, addr, need);
    const vkpool_t *p = slot >= 0 ? g_pool[slot] : NULL;
    uint32_t gen = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE);
    // A registration between the fast lookup and this lock legitimately changes
    // the answer; compare only snapshots from the same registry generation.
    if (verify && cached->gen == gen && (!p || cached->buf != p->buf || cached->off != addr - p->lo || cached->available != p->hi - addr)) {
        if (t_buflookup.failures++ < 8) VKLOG("[RED] buffer range cache: snapshot differs at %#llx +%#llx", (unsigned long long)addr, (unsigned long long)need);
    }
    if (slot >= 0) {
        *buf = p->buf; *off = addr - p->lo;
        if (available) *available = p->hi - addr;
        if (cached) *cached = (buf_lookup_t){addr, need, addr - p->lo, p->hi - addr,
            gen, p->buf};
    } else if (cached) cached->buf = VK_NULL_HANDLE;
    pthread_rwlock_unlock(&g_pool_mu);
    return slot >= 0;
}
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    return vk_buf_range_at(addr, len, buf, off, NULL);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    if (buf) *buf = (void *)(uintptr_t)p->lo; if (off) *off = addr - p->lo; return 1;
}
void mtl_alias_drop_pool(const void *pool) { void vk_tex_drop_pool(const void *); vk_tex_drop_pool(pool); }
// The NVN flags of the pool at mem (hle_nvn.c, after mtl_pool_add), for every chunk.
void vk_pool_set_flags(const void *mem, int flags) {
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->base == (uint64_t)(uintptr_t)mem) g_pool[i]->flags = flags;   // (every chunk of it)
    pthread_rwlock_unlock(&g_pool_mu);
}
static uint64_t g_sync_bytes, g_sync_full;   // copied pools: bytes copied by the incremental sync, full copies
// Copied pools: the guest's bytes into the GPU copy. The first sync copies the whole pool; later ones copy only the 64 KB blocks with pages the CPU wrote since the last
// sync (nvn_dirty's write-protect tracking: a block is protected again before it is copied, so a write during the copy is caught by the next sync). An 860 MB
// pool copied whole at every submission (Unicorn Overlord on a Legion Y700 2023) ran at a few frames a second
static void vk_pools_sync_(VkCommandBuffer cb);
void vk_pools_sync(VkCommandBuffer cb) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    vk_pools_sync_(cb); if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[2] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
static void vk_pools_sync_(VkCommandBuffer cb) {
    (void)cb;
    enum { BLK = 64 << 10 };
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = g_pool[i]; if (!p->shadow) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)p->lo; uint64_t size = p->hi - p->lo, st = nvn_dirty_now();
        if (!p->span && (p->span = nvn_dirty_span(lo, size))) p->synced = 0;   // tracked only now (nvnMemoryPoolInitialize registers the range after the pool): copied whole once more, protected
        if (!p->synced || !p->span) {   // untracked: every sync copies all of it, as before
            if (p->span) nvn_dirty_protect(lo, size);
            memcpy(p->shadow, lo, size); g_sync_bytes += size; g_sync_full++; p->synced = 1; p->stamp0 = p->stamp = st; continue; }
        // judged against the sync before the last: a write whose fault took its generation before the last sync's stamp but stored it after that sync looked
        // at its block would otherwise never count (its page is writable from then on, and clean by every later stamp). A dirty block is copied twice
        for (uint64_t off = 0; off < size; off += BLK) {
            uint64_t n = size - off < BLK ? size - off : BLK;
            if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) continue;
            nvn_dirty_protect(lo + off, n); memcpy(p->shadow + off, lo + off, n); g_sync_bytes += n;
        }
        p->stamp0 = p->stamp; p->stamp = st;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    static uint64_t n, last; if (++n % 600 == 0) { VKLOG("copied pools: %.1f MB synced in the last 600 submissions (%.2f MB each), %llu full copies so far", (g_sync_bytes - last) / 1048576.0, (g_sync_bytes - last) / 600.0 / 1048576.0, (unsigned long long)g_sync_full); last = g_sync_bytes; }
}
// Copied pools after a submission: only recorded GPU write spans return to the guest.
// A whole cached pool, or a gap between writes, can contain CPU changes made after
// the upload. Copying an older GPU snapshot there loses those changes. A pool
// never synced yet is left alone (its copy holds nothing of the game's).
struct vksub; static void pools_copy_back(const struct vksub *s);

// ---------------- submission slots ----------------
#define NSLOT 6
#define MAIN_CBS 32   // allocated as used; long depth-free passes may be split
_Static_assert(MAIN_CBS * 2 <= VK_GPU_SEGMENTS_MAX, "timestamp capacity covers main and upload buffers");
// GPU span: first to last main-buffer timestamp, including possible CPU-feed gaps
// between early submissions. Optional segment timestamps measure those gaps.
static VkQueryPool g_tsq; uint64_t g_gpu_ns;
static VkQueryPool g_segment_q;
static VkQueryPool g_upload_segment_q;
static pthread_mutex_t g_segment_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t span, covered, gaps, parts, submissions, incomplete, all_covered, all_gaps, all_span, upload_covered, all_incomplete;
    uint64_t cpu_setup, cpu_first, cpu_tail, cpu_count, cpu_early; } g_segment_stats;
// Timestamp counters (ReportCounter TIMESTAMP): each writes a GPU timestamp where it is in the command stream (CQ per slot), and at completion it gets the guest
// clock now less how long before the submission's end the GPU passed it. They used to all get the completion time of their submission: a game timing its GPU
// work between two counters read 0 when both were in one submission and the gap between completions (our CPU replay included) when not. Xenoblade Chronicles 3
// sizes its frame by that time (dynamic resolution): its main pass went between 960x544 and 776x440 every few frames while the camera turned; with GPU times
// the size holds steady
enum { CQ = 64 };
static VkQueryPool g_cq;
#define SCRATCH_BLOCK (8u << 20)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size, used; } scratch_blk;
typedef struct { int kind; void *a; void *b; } dfree_t;   // 0 buffer+memory, 1 image+memory, 2 view
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; int q; } post_op;   // q: a timestamp counter's query in g_cq, or -1
typedef struct { VkDescriptorSet *set; uint32_t count, used, capacity; } descriptor_reuse_entry;
typedef struct { nvn_hmap layout; uint32_t count; int enabled, discard; } descriptor_reuse;
enum { DESCRIPTOR_REUSE_LIMIT = 4096 };   // cache budget per slot, never an allocation limit
static _Atomic uint64_t g_descriptor_allocated, g_descriptor_reused, g_descriptor_resets, g_descriptor_discards;
struct vksub {
    VkCommandPool pool; VkCommandBuffer up, main; int up_used;
    VkCommandBuffer mains[MAIN_CBS]; int nmain_alloc, nmain_used, cb_err;
    VkCommandBuffer ups[MAIN_CBS]; int nup_alloc, nup_used; int nmain_sent;   // upload command buffers in order (one per part sent early), main buffers already submitted   // the submission's main command buffers in order (vk_cmd_split); main = the one being recorded
    vk_deferred_part_state deferred_part; int deferred_pending;
    uint64_t fence;   // the fence this slot last committed with (0 = never)
    uint64_t cpu_begin, cpu_ready, cpu_first;   // optional segment attribution, all CLOCK_MONOTONIC
    int ncq;          // timestamp counter queries used in this submission (g_cq)
    scratch_blk *blk; int nblk, capblk;
    scratch_blk *dbblk; int ndbblk, capdbblk;
    VkDescriptorPool *dp; int ndp, capdp, dp_cur;
    uint32_t dp_used[9], dp_sets; int dp_counted; int no_ts;   // what pool dp_cur has given out (vk_alloc_set)
    descriptor_reuse reuse;   // allocated sets of each layout, available only after this slot's fence
    dfree_t *df; int ndf, capdf;
    post_op *post; int npost, cappost;
    vk_pool_writes writes;   // exact GPU write-span union, retained until this slot's completion
};
static vksub_t g_slot[NSLOT]; static int g_cur = -1; uint64_t g_cmd_gen;   // g_cmd_gen: main command buffers begun (gfx_vk_exec.c bind cache)
static void pools_copy_back(const struct vksub *s) {
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { vkpool_t *p = g_pool[i]; if (!p->shadow || !p->synced) continue;
        vk_pool_writes_copy_back((uint8_t *)(uintptr_t)p->lo, p->shadow, p->lo, p->hi, &s->writes); }
    pthread_rwlock_unlock(&g_pool_mu);
}
static VkCommandBuffer g_slot_main_cb(void) { return g_slot[g_cur].main; }
static void segment_mark(vksub_t *s, int end) {
    if (g_segment_q) vkCmdWriteTimestamp(s->main, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        g_segment_q, (uint32_t)((g_cur * MAIN_CBS + s->nmain_used - 1) * 2 + end));
}
static void upload_segment_mark(vksub_t *s, int end) {
    if (g_upload_segment_q) vkCmdWriteTimestamp(s->up, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        g_upload_segment_q, (uint32_t)((g_cur * MAIN_CBS + s->nup_used - 1) * 2 + end));
}
void vk_segment_report(void) {
    if (!g_segment_q) return;
    pthread_mutex_lock(&g_segment_mu);
    VKLOG("GPU segments per frame: %.2f ms covered spans, %.2f ms between parts, %.2f ms envelope; %.1f parts in %.1f submissions, %llu incomplete submissions (covered spans include internal stalls)",
        g_segment_stats.covered / 6e8, g_segment_stats.gaps / 6e8, g_segment_stats.span / 6e8,
        g_segment_stats.parts / 600.0, g_segment_stats.submissions / 600.0, (unsigned long long)g_segment_stats.incomplete);
    if (g_upload_segment_q) VKLOG("GPU spans including uploads per frame: %.2f ms covered, %.2f ms between parts, %.2f ms envelope (%.2f ms in upload buffers); %llu incomplete submissions",
        g_segment_stats.all_covered / 6e8, g_segment_stats.all_gaps / 6e8, g_segment_stats.all_span / 6e8,
        g_segment_stats.upload_covered / 6e8, (unsigned long long)g_segment_stats.all_incomplete);
    if (g_segment_stats.cpu_count) {
        double d = g_segment_stats.cpu_count * 1e6;
        VKLOG("CPU feed per committed submission: setup+slot wait %.3f ms, recording before first queue call %.3f, record+submit tail %.3f; %llu submissions, %llu sent early",
            g_segment_stats.cpu_setup/d, g_segment_stats.cpu_first/d, g_segment_stats.cpu_tail/d,
            (unsigned long long)g_segment_stats.cpu_count, (unsigned long long)g_segment_stats.cpu_early);
    }
    memset(&g_segment_stats, 0, sizeof g_segment_stats);
    pthread_mutex_unlock(&g_segment_mu);
}
// DM_VK_PASS_TIMES=1: continuous pass timing; =2: only sample frames requested
// by a new DM_VK_PASS_CAPTURE path (DM_VK_PASS_CAPTURE_FRAMES=1..16, default 4).
enum { PASSQ = 512 };   // passes timed per submission; omissions are counted
static VkQueryPool g_pq; static int g_pq_on = -1;
typedef struct { uint32_t w, h, fmt, draws, fidx; } pass_meta;   // fidx: the pass's place in its frame (submissions are cut every DM_VK_CB_DRAWS draws)
enum { FPASSQ = 1024 }; static int g_fpass;   // true pass ordinal, including untimed passes
static int g_pass_sample;
static char g_pass_request[1024]; static unsigned g_pass_left;
static unsigned pass_capture_frames(void) { const char *e = getenv("DM_VK_PASS_CAPTURE_FRAMES"); int n = e ? atoi(e) : 4; return n < 1 ? 1u : n > 16 ? 16u : (unsigned)n; }
void vk_pass_frame(void) {
    g_fpass = 0;
    if (g_pq_on != 2) return;
    const char *p = getenv("DM_VK_PASS_CAPTURE");
    if (p && *p && strlen(p) < sizeof g_pass_request && strcmp(p, g_pass_request)) {
        snprintf(g_pass_request, sizeof g_pass_request, "%s", p); g_pass_left = pass_capture_frames();
    }
    g_pass_sample = g_pass_left != 0; if (g_pass_left) g_pass_left--;
}
static pass_meta g_pmeta[NSLOT][PASSQ]; static int g_npass[NSLOT];
static double g_pacc[FPASSQ]; static uint32_t g_pcnt[FPASSQ]; static pass_meta g_plast[FPASSQ]; static int g_pmax;   // by the pass's place in its frame (the completion thread adds)
static pthread_mutex_t g_pass_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pass_omitted;
static uint64_t g_pass_timed, g_pass_invalid, g_pass_unranked;   // under g_pass_mu
static int pass_times_on(void) { if (g_pq_on < 0) { const char *e = getenv("DM_VK_PASS_TIMES"); g_pq_on = e ? atoi(e) : 0; } return g_pq && (g_pq_on == 1 || (g_pq_on == 2 && g_pass_sample)); }
void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws) {
    if (!pass_times_on() || g_cur < 0) return;
    int *n = &g_npass[g_cur];
    if (*n >= PASSQ) { if (end) { g_fpass++; atomic_fetch_add_explicit(&g_pass_omitted, 1, memory_order_relaxed); } return; }
    vkCmdWriteTimestamp(g_slot_main_cb(), end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_pq, (uint32_t)(g_cur * PASSQ * 2 + *n * 2 + end));
    if (end) { g_pmeta[g_cur][*n] = (pass_meta){w, h, fmt, draws, (uint32_t)g_fpass++}; (*n)++; }
}
// The completion thread writes at most sixteen requested frames. Re-arm with a
// different path; raw same-queue ticks permit offline overlap/coverage checks.
static void pass_capture(uint64_t frame, uint64_t fence, const pass_meta *meta, const uint64_t *ticks, int count) {
    static FILE *out; static char opened[1024]; static uint64_t last = UINT64_MAX;
    static unsigned frames, limit;
    const char *p = getenv("DM_VK_PASS_CAPTURE"); if (!p || !*p || strlen(p) >= sizeof opened) return;
    if (strcmp(opened, p)) {
        if (out) fclose(out);
        snprintf(opened, sizeof opened, "%s", p); out = fopen(p, "w"); frames = 0; last = UINT64_MAX; limit = pass_capture_frames();
        if (!out) { VKLOG("[RED] cannot open pass capture %s", p); return; }
        fprintf(out, "frame\tfence\tpass\twidth\theight\tformat\tdraws\tbbox_width\tbegin_ticks\tend_ticks\ttick_ns\n");
    }
    if (!out) return;
    if (frame != last) {
        if ((last != UINT64_MAX && frame < last) || frames == limit) return;
        last = frame; frames++;
    }
    for (int i = 0; i < count; ++i) fprintf(out, "%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%.9g\n",
        (unsigned long long)frame, (unsigned long long)fence, meta[i].fidx, meta[i].w, meta[i].h, meta[i].fmt,
        meta[i].draws & 0xffff, meta[i].draws >> 16, (unsigned long long)ticks[2*i], (unsigned long long)ticks[2*i+1], (double)V.props.limits.timestampPeriod);
    fflush(out);
}
static void pass_accumulate(const pass_meta *meta, const uint64_t *ticks, int count) {
    pthread_mutex_lock(&g_pass_mu);
    for (int i = 0; i < count; ++i) {
        if (ticks[2*i+1] < ticks[2*i]) { g_pass_invalid++; continue; }
        unsigned f = meta[i].fidx; g_pass_timed++;
        if (f >= FPASSQ) { g_pass_unranked++; continue; }
        g_pacc[f] += (double)(ticks[2*i+1] - ticks[2*i]) * V.props.limits.timestampPeriod / 1e6;
        g_pcnt[f]++; g_plast[f] = meta[i]; if ((int)f + 1 > g_pmax) g_pmax = (int)f + 1;
    }
    pthread_mutex_unlock(&g_pass_mu);
}
void vk_pass_report(void) {   // the most expensive passes since the last report, then the counters start again
    if (!g_pq) return;
    pthread_mutex_lock(&g_pass_mu);
    static int idx[FPASSQ]; int m = g_pmax; for (int i = 0; i < m; i++) idx[i] = i;
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_pcnt[idx[j]] && (!g_pcnt[idx[i]] || g_pacc[idx[j]] / g_pcnt[idx[j]] > g_pacc[idx[i]] / g_pcnt[idx[i]])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    double tot = 0; for (int i = 0; i < m; i++) if (g_pcnt[i]) tot += g_pacc[i] / g_pcnt[i];
    char b[1600]; int o = snprintf(b, sizeof b, "passes by GPU time (%d ordinals, %.2f ms sum of ordinal means; %llu timed, %llu untimed, %llu invalid, %llu unranked):", m, tot,
        (unsigned long long)g_pass_timed, (unsigned long long)atomic_exchange_explicit(&g_pass_omitted, 0, memory_order_relaxed), (unsigned long long)g_pass_invalid, (unsigned long long)g_pass_unranked);
    for (int i = 0; i < m && i < 10 && o < (int)sizeof b - 80; i++) { int k = idx[i]; if (!g_pcnt[k]) continue;
        o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (%ux%u fmt %u, %u draws over %u px wide);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].fmt, g_plast[k].draws & 0xffff, g_plast[k].draws >> 16); }
    { extern uint64_t g_nvn_discards[2]; static uint64_t pd0, pd1; o += snprintf(b + o, sizeof b - (size_t)o, " discards since: colour %llu, depth %llu", (unsigned long long)(g_nvn_discards[0] - pd0), (unsigned long long)(g_nvn_discards[1] - pd1)); pd0 = g_nvn_discards[0]; pd1 = g_nvn_discards[1]; }
    VKLOG("%s", b);
    memset(g_pacc, 0, sizeof g_pacc); memset(g_pcnt, 0, sizeof g_pcnt); g_pmax = 0;
    g_pass_timed = g_pass_invalid = g_pass_unranked = 0;
    pthread_mutex_unlock(&g_pass_mu);
}
// one submission at a time: the game's (hle_nvn.c holds its own lock around them) and the backend's own (texel readback, present)
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;   // fence of the last commit
static VkSemaphore g_timeline;   // submission f signals the value f (V.has_timeline), else g_fring[f % NFRING]
#define NFRING 16   // more than the submissions in flight (NSLOT: mtl_sub_begin's slot_recycle waits for the slot's last one)
static VkFence g_fring[NFRING];
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done; // Retired: internal slots and resources still use this boundary.
static pthread_mutex_t g_guest_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_guest_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_guest_done; // GPU completion plus every CPU-visible post operation.
static _Atomic uint64_t g_guest_wait_calls, g_guest_wait_ahead;
static int guest_notify_enabled(void) {
    static _Atomic int cached;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (!v) { const char *e = getenv("DM_VK_GUEST_NOTIFY"); v = e && atoi(e) > 0 ? 1 : 2; atomic_store_explicit(&cached, v, memory_order_relaxed); }
    return v == 1;
}
static void completion_notify_guest(uint64_t fence) {
    if (!guest_notify_enabled()) return;
    pthread_mutex_lock(&g_guest_done_mu);
    if (fence > g_guest_done) g_guest_done = fence;
    dm_sync_fence_point("fence: guest visible", fence);
    pthread_cond_broadcast(&g_guest_done_cv);
    pthread_mutex_unlock(&g_guest_done_mu);
}
static void slots_init(void) {
    if (guest_notify_enabled()) VKLOG("guest fence visibility precedes command-buffer retirement (DM_VK_GUEST_NOTIFY)");
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (V.has_timeline) VKCHK(vkCreateSemaphore(V.dev, &si, NULL, &g_timeline));
    else for (int i = 0; i < NFRING; i++) { VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHK(vkCreateFence(V.dev, &fi, NULL, &g_fring[i])); }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * NSLOT};
    if (V.props.limits.timestampComputeAndGraphics && vkCreateQueryPool(V.dev, &qi, NULL, &g_tsq)) g_tsq = VK_NULL_HANDLE;
    { const char *e = getenv("DM_VK_SEGMENT_TIMES"), *u = getenv("DM_VK_UPLOAD_TIMES"); if (g_tsq && ((e && atoi(e)) || (u && atoi(u)))) {
        VkQueryPoolCreateInfo sq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * MAIN_CBS * 2};
        if (vkCreateQueryPool(V.dev, &sq, NULL, &g_segment_q)) g_segment_q = VK_NULL_HANDLE;
        VKLOG("command-buffer segment timing %s (DM_VK_SEGMENT_TIMES)", g_segment_q ? "on" : "unavailable");
    } }
    const char *upload_times = getenv("DM_VK_UPLOAD_TIMES");
    if (g_segment_q && upload_times && atoi(upload_times)) {
        VkQueryPoolCreateInfo sq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * MAIN_CBS * 2};
        if (vkCreateQueryPool(V.dev, &sq, NULL, &g_upload_segment_q)) g_upload_segment_q = VK_NULL_HANDLE;
        VKLOG("upload command-buffer timing %s (DM_VK_UPLOAD_TIMES)", g_upload_segment_q ? "on" : "unavailable");
    }
    VkQueryPoolCreateInfo cqi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * CQ};
    if (!g_tsq || vkCreateQueryPool(V.dev, &cqi, NULL, &g_cq)) g_cq = VK_NULL_HANDLE;
    { const char *e = getenv("DM_VK_PASS_TIMES"); if (e && atoi(e)) { VkQueryPoolCreateInfo pq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * PASSQ * 2}; if (vkCreateQueryPool(V.dev, &pq, NULL, &g_pq)) g_pq = VK_NULL_HANDLE; } }
    for (int i = 0; i < NSLOT; i++) {
        // not TRANSIENT, and reset by command buffer (see slot_recycle)
        VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
        VKCHK(vkCreateCommandPool(V.dev, &ci, NULL, &g_slot[i].pool));
        VkCommandBuffer cb[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_slot[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, cb)); g_slot[i].up = g_slot[i].ups[0] = cb[0]; g_slot[i].main = g_slot[i].mains[0] = cb[1]; g_slot[i].nmain_alloc = 1; g_slot[i].nup_alloc = 1;
    }
}
uint64_t vk_fence_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }   // written under g_done_mu (for the waiters) with a release store
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
// Without timeline semaphores only the completion thread waits on the ring's fences, in submission order (a fence tells nothing of the submissions before
// it); everyone else waits for the completion thread to pass the fence (g_done), so a fence is never waited on while the commit resets it for reuse
static void wait_fence(uint64_t f) {
    if (!f) return;
    DM_SYNC_FENCE_SCOPE("fence: GPU wait", f);
    if (!V.has_timeline) { mtl_wait(f, ~0ull); return; }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_timeline, &f};
    vkWaitSemaphores(V.dev, &wi, UINT64_MAX);
}
// The slot's command buffers are reset on the completion thread once its submission is done (completion_main), not when the replay begins them again: an
// implicit reset at vkBeginCommandBuffer took 7% of Xenoblade Chronicles 3's replay thread at a submission's start and more at every part (its battles
// replay ~3,000 draws a frame on a thread that is the frame's limit). The slot is then taken only after the completion thread has passed it (mtl_wait, g_done)
// rather than as soon as the GPU has. DM_VK_X_NOPRERESET=1 (start-up): reset as they are begun
static int g_prereset = -1;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void descriptor_reuse_free(void *value) {
    descriptor_reuse_entry *e = value; free(e->set); free(e);
}
static void descriptor_slot_recycle(vksub_t *s) {
    // Called only after the slot's existing GPU completion wait has passed.
    // Every returned set is still fully rewritten by stage_set before use. Set
    // content caches retain their existing per-submission epoch; no content is
    // assumed to survive recycling, and no set is returned twice in one epoch.
    const char *env = getenv("DM_VK_SET_REUSE"); int enabled = !env || atoi(env);   // =0: reset/reallocate as before (live at slot recycle)
    descriptor_reuse *r = &s->reuse;
    if (!enabled || !r->enabled || r->discard) {
        if (r->discard) atomic_fetch_add_explicit(&g_descriptor_discards, 1, memory_order_relaxed);
        nvn_hm_clear(&r->layout, descriptor_reuse_free); r->count = 0; r->discard = 0;
        for (int i = 0; i < s->ndp; i++) {
            VKCHK(vkResetDescriptorPool(V.dev, s->dp[i], 0));
            atomic_fetch_add_explicit(&g_descriptor_resets, 1, memory_order_relaxed);
        }
        s->dp_cur = 0; s->dp_counted = -1;
    } else for (uint32_t i = 0; i < r->layout.cap; i++) if (r->layout.vals[i])
        ((descriptor_reuse_entry *)r->layout.vals[i])->used = 0;
    if (!!enabled != !!r->enabled) VKLOG("descriptor set allocation reuse %s for a recycled slot (DM_VK_SET_REUSE)", enabled ? "on" : "off");
    r->enabled = !!enabled;
    if (!r->layout.klen) r->layout.klen = sizeof(VkDescriptorSetLayout);
}
void vk_descriptor_reuse_stats(uint64_t *allocated, uint64_t *reused, uint64_t *resets, uint64_t *discards) {
    *allocated = atomic_load_explicit(&g_descriptor_allocated, memory_order_relaxed);
    *reused = atomic_load_explicit(&g_descriptor_reused, memory_order_relaxed);
    *resets = atomic_load_explicit(&g_descriptor_resets, memory_order_relaxed);
    *discards = atomic_load_explicit(&g_descriptor_discards, memory_order_relaxed);
}
static void slot_recycle(vksub_t *s) {   // the GPU has passed this slot's last submission: free what it held, rewind its allocators
    ucopy_drain();   // a queued shadow may still read this slot's immutable inline updates
    if (g_prereset < 0) { const char *e = getenv("DM_VK_X_NOPRERESET"); g_prereset = !(e && atoi(e)); }
    if (g_prereset) mtl_wait(s->fence, ~0ull); else wait_fence(s->fence);
    for (int i = 0; i < s->ndf; i++) {
        dfree_t *d = &s->df[i];
        if (d->kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 1) { vkDestroyImage(V.dev, (VkImage)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)d->a, NULL); img_sub_free_(d->b); }
        else if (d->kind == 2) vkDestroyImageView(V.dev, (VkImageView)d->a, NULL);
        else if (d->kind == 3) vkDestroyBufferView(V.dev, (VkBufferView)d->a, NULL);
    }
    s->ndf = 0;
    for (int i = 0; i < s->nblk; i++) s->blk[i].used = 0;
    for (int i = 0; i < s->ndbblk; i++) s->dbblk[i].used = 0;
    descriptor_slot_recycle(s);
    // The pool is not reset: each command buffer is reset as it is begun again (the pool allows it). A reset of the transient pool made Adreno's driver free the
    // command memory and map it again as the next submission grew (ioctl, mmap and munmap): in Xenoblade Chronicles 3's battles the submission's start took 21%
    // of the replay thread, now 9%, and frames went from about 42 to 38 ms
}
static vksub_t *cur(void) { if (g_cur < 0) die("[vk] no open submission"); return &g_slot[g_cur]; }
int vk_sub_open(void) { return g_cur >= 0; }
// A long submission is recorded into several command buffers, submitted together in order: Adreno's driver refuses a command buffer past some size at
// vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY), which NEO's handheld frames reached at about the 712th draw. The caller has ended the render pass.
// Producer-only bookkeeping. Worker results are observed only after its drain.
static void split_advance_upload(vksub_t *s) {
    if (s->up_used) {
        if (s->nup_used == s->nup_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->ups[s->nup_alloc])); s->nup_alloc++; }
        s->up = s->ups[s->nup_used++]; s->up_used = 0;
    }
}
static void split_collect(vksub_t *s) {
    if (!s->deferred_pending) return;
    vk_encode_drain();
    s->cb_err |= s->deferred_part.end_error;
    if (s->deferred_part.last_sent > s->nmain_sent) s->nmain_sent = s->deferred_part.last_sent;
    if (!s->cpu_first) s->cpu_first = s->deferred_part.cpu_first;
    s->deferred_pending = 0;
}
static int split_defer(vksub_t *s, int send) {
    if (!send || s->cb_err || !vk_encode_deferred_parts()) return 0;
    if (s->up_used) { vk_img_barrier_all(s->up); upload_segment_mark(s, 1); }
    // This still precedes every GPU submit, including ones on the worker.
    ucopy_drain();
    vk_encode_defer_part(V.q, s->main, s->up_used ? s->up : VK_NULL_HANDLE,
                         s->nmain_used, g_segment_q != VK_NULL_HANDLE, &s->deferred_part);
    s->deferred_pending = 1;
    split_advance_upload(s);
    return 1;
}
static void vk_cmd_split_(void);
void vk_cmd_split(void) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    vk_cmd_split_(); if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[6] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
static void vk_cmd_split_(void) {
    vksub_t *s = cur(); if (s->nmain_used == MAIN_CBS || s->nup_used == MAIN_CBS) return;
    segment_mark(s, 1);
    const char *se = getenv("DM_VK_SUBMIT_SPLIT"); int send = !(se && !atoi(se));
    if (split_defer(s, send)) goto next_main;
    split_collect(s);
    if (vkEndCommandBuffer(s->main) < 0) { s->cb_err = 1; static int n; if (n++ < 3) VKLOG("[RED] a part of a long submission was refused at its end (part %d)", s->nmain_used); }
    // what is recorded so far goes to the GPU now, as a submission of its own (with its uploads first): Adreno refuses a single submission past about
    // 700 draws (NEO's handheld frames: VK_ERROR_OUT_OF_HOST_MEMORY at vkEndCommandBuffer whatever the draws were, and in one command buffer or several)

    if (send && !s->cb_err) {
        VkCommandBuffer cbs[2]; uint32_t n = 0;
        if (s->up_used) { vk_img_barrier_all(s->up); upload_segment_mark(s, 1); if (vkEndCommandBuffer(s->up) < 0) s->cb_err = 1; cbs[n++] = s->up; }
        cbs[n++] = s->main;
        if (!s->cb_err) {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, n, cbs, 0, NULL};
            ucopy_drain();   // these parts may already consume newly bound uniform shadows
            pthread_mutex_lock(&g_vk_queue_mu);
            if (g_segment_q && !s->cpu_first) s->cpu_first = ucopy_now();
            VkResult r = vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vk_queue_mu);
            if (r) { void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit (part): %d (device lost?)", r); }
            { static int n; if (n++ < 3) VKLOG("a long submission: part %d sent early", s->nmain_used); }
            s->nmain_sent = s->nmain_used;
            split_advance_upload(s);
        }
    }
next_main:
    if (s->nmain_used == s->nmain_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->mains[s->nmain_alloc])); s->nmain_alloc++; }
    s->main = s->mains[s->nmain_used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vk_encode_begin_checked(s->main, &bi); g_cmd_gen++;
    segment_mark(s, 0);
}   // outside a submission nothing may be recorded (present, a pool finalized between frames)
VkCommandBuffer vk_cmd(void) { return cur()->main; }
int vk_cmd_parts_left(void) { vksub_t *s = cur(); return MAIN_CBS - (s->nmain_used > s->nup_used ? s->nmain_used : s->nup_used); }   // parts vk_cmd_split can still begin (gfx_vk_exec.c pass splits)
int vk_sub_sent_early(void) { return g_cur >= 0 && (g_slot[g_cur].nmain_sent > 0 || g_slot[g_cur].deferred_pending); }
#ifndef DM_RELEASE
int vk_debug_stall_ready(void) {
    if (g_cur < 0) return 0;
    // A deferred part must have reached vkQueueSubmit before replay sleeps.
    vksub_t *s = &g_slot[g_cur]; split_collect(s);
    return s->nmain_sent > 0 && !s->cb_err;
}
#endif
uint64_t g_up_gen;   // upload command buffers begun (gfx_vk_tex.c lin_copy: once per upload buffer)
VkCommandBuffer vk_upload_cmd(void) {
    vksub_t *s = cur();
    if (!s->up_used) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vk_encode_begin_checked(s->up, &bi); s->up_used = 1; g_up_gen++;
        // Uploads execute before their main part. Their query pool must be
        // reset here, not by the first main buffer after the uploads ran.
        if (g_upload_segment_q && s->nup_used == 1) vkCmdResetQueryPool(s->up, g_upload_segment_q, (uint32_t)(g_cur * MAIN_CBS * 2), MAIN_CBS * 2);
        upload_segment_mark(s, 0);
    }
    return s->up;
}
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    vksub_t *s = cur(); if (!align) align = 16;
    for (int i = 0; i < s->nblk; i++) {
        scratch_blk *b = &s->blk[i]; VkDeviceSize o = (b->used + align - 1) & ~(align - 1);
        if (o + n <= b->size) { b->used = o + n; *buf = b->b; *off = o; return b->p + o; }
    }
    if (s->nblk == s->capblk) { s->capblk = s->capblk ? 2 * s->capblk : 8; s->blk = realloc(s->blk, s->capblk * sizeof *s->blk); }
    scratch_blk *b = &s->blk[s->nblk++]; memset(b, 0, sizeof *b);
    static VkDeviceSize blk = 0; if (!blk) { const char *e = getenv("DM_VK_SCRATCH_MB"); blk = e && atoi(e) > 0 ? (VkDeviceSize)atoi(e) << 20 : SCRATCH_BLOCK; }
    b->size = n > blk ? (n + 0xffff) & ~0xffffull : blk;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, b->size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b->b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->b, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKCHK(vk_buffer_memory_allocate(&ai, &b->m)); VKCHK(vkBindBufferMemory(V.dev, b->b, b->m, 0));
    void *m; VKCHK(vkMapMemory(V.dev, b->m, 0, VK_WHOLE_SIZE, 0, &m)); b->p = m;
    b->used = n; *buf = b->b; *off = 0; return b->p;
}
// Descriptor bytes share the submission slot's fence lifetime, but have their
// own bounded blocks: ordinary UBO/vertex scratch can exceed descriptor ranges.
void *vk_db_scratch(VkDeviceSize n, VkBuffer *buffer, VkDeviceSize *offset) {
    vksub_t *s = cur(); VkDeviceSize align = V.dbprops.descriptorBufferOffsetAlignment;
    VkDeviceSize limit = V.dbprops.maxResourceDescriptorBufferRange;
    if (V.dbprops.maxSamplerDescriptorBufferRange < limit) limit = V.dbprops.maxSamplerDescriptorBufferRange;
    if (!align || n > limit) die("[vk] descriptor block exceeds the device's range");
    for (int i = 0; i < s->ndbblk; i++) {
        scratch_blk *b = &s->dbblk[i]; VkDeviceSize off = (b->used + align - 1) / align * align;
        if (off <= b->size && n <= b->size - off) { b->used = off + n; *buffer = b->b; *offset = off; return b->p + off; }
    }
    if (s->ndbblk == s->capdbblk) {
        int capacity = s->capdbblk ? s->capdbblk * 2 : 4;
        void *blocks = realloc(s->dbblk, (size_t)capacity * sizeof *s->dbblk);
        if (!blocks) die("[vk] cannot allocate descriptor block metadata");
        s->dbblk = blocks; s->capdbblk = capacity;
    }
    scratch_blk *b = &s->dbblk[s->ndbblk++]; memset(b, 0, sizeof *b);
    b->size = limit < (1u << 20) ? limit : (1u << 20); if (b->size < n) b->size = n;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, b->size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &ci, NULL, &b->b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->b, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKCHK(vk_buffer_memory_allocate(&ai, &b->m)); VKCHK(vkBindBufferMemory(V.dev, b->b, b->m, 0));
    void *mapped; VKCHK(vkMapMemory(V.dev, b->m, 0, VK_WHOLE_SIZE, 0, &mapped)); b->p = mapped;
    b->used = n; *buffer = b->b; *offset = 0; return b->p;
}
static void df_add(int kind, void *a, void *b) {
    if (g_cur < 0) {   // outside a submission (a pool finalized between frames): after everything submitted so far
        wait_fence(g_fence);
        if (kind == 1 || kind == 4) { void vk_snapshot_source_free(VkImage); vk_snapshot_source_free((VkImage)a); }
        if (kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 1) { vkDestroyImage(V.dev, (VkImage)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)a, NULL); img_sub_free_(b); }
        else if (kind == 2) vkDestroyImageView(V.dev, (VkImageView)a, NULL);
        else vkDestroyBufferView(V.dev, (VkBufferView)a, NULL);
        return;
    }
    vksub_t *s = cur(); if (s->ndf == s->capdf) { s->capdf = s->capdf ? 2 * s->capdf : 64; s->df = realloc(s->df, s->capdf * sizeof *s->df); }
    s->df[s->ndf++] = (dfree_t){kind, a, b};
}
// Image memory. Small images share blocks: each allocation the driver makes is a mapping of the process (Adreno maps them all), and a game with thousands of textures
// ran towards the kernel's per-process mapping limit (Xenoblade Chronicles 3: about 9,400 driver mappings in its first area, 2,216 of them 172 KB). Images up to
// IMG_SLOT_MAX take a slot of a block (64 slots, 2 MB to IMG_BLOCK bytes), in size classes 2^(k/2) x 4 KB apart (a slot is at most 41% larger than the image); larger ones, or
// ones that need more than 4 KB alignment, get memory of their own
enum { IMG_CLASSES = 18, IMG_BLOCK = 32 << 20 };
#define IMG_SLOT_MAX ((VkDeviceSize)2 << 20)
typedef struct imgblk { VkDeviceMemory m; uint32_t type, cls, nslot, nfree; uint64_t bits[128]; struct imgblk *next; } imgblk;   // bits: 1 = slot free
typedef struct { imgblk *b; uint32_t slot; } imgsub;
static imgblk *g_imgblk[IMG_CLASSES]; static pthread_mutex_t g_imgblk_mu = PTHREAD_MUTEX_INITIALIZER;
static VkDeviceSize img_class_size(int c) { return (c & 1 ? 6144ull : 4096ull) << (c >> 1); }   // 4K, 6K, 8K, 12K, 16K, 24K, ... 2M
VkDeviceMemory vk_image_memory(const VkMemoryRequirements *req, uint32_t type, VkDeviceSize *off, void **sub) {
    *off = 0; *sub = NULL;
    int c = 0; while (c < IMG_CLASSES && img_class_size(c) < req->size) c++;
    if (c == IMG_CLASSES || req->size > IMG_SLOT_MAX || req->alignment > 4096) {   // memory of its own
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req->size, type}; VkDeviceMemory m;
        return vkAllocateMemory(V.dev, &ai, NULL, &m) ? VK_NULL_HANDLE : m;
    }
    pthread_mutex_lock(&g_imgblk_mu);
    imgblk *b = g_imgblk[c]; while (b && (b->type != type || !b->nfree)) b = b->next;
    if (!b) {
        b = calloc(1, sizeof *b); b->type = type; b->cls = (uint32_t)c; VkDeviceSize bs = img_class_size(c) * 64; if (bs < (2 << 20)) bs = 2 << 20; if (bs > IMG_BLOCK) bs = IMG_BLOCK; b->nslot = (uint32_t)(bs / img_class_size(c));   // 64 slots, 2 MB to 32 MB
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (VkDeviceSize)b->nslot * img_class_size(c), type};
        if (vkAllocateMemory(V.dev, &ai, NULL, &b->m)) { pthread_mutex_unlock(&g_imgblk_mu); free(b); return VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < b->nslot; i++) b->bits[i >> 6] |= 1ull << (i & 63);
        b->nfree = b->nslot; b->next = g_imgblk[c]; g_imgblk[c] = b;
    }
    uint32_t i = 0; while (!b->bits[i >> 6]) i += 64; i += (uint32_t)__builtin_ctzll(b->bits[i >> 6]);
    b->bits[i >> 6] &= ~(1ull << (i & 63)); b->nfree--;
    pthread_mutex_unlock(&g_imgblk_mu);
    imgsub *s = malloc(sizeof *s); s->b = b; s->slot = i; *sub = s; *off = (VkDeviceSize)i * img_class_size(c);
    return b->m;
}
static void img_sub_free(imgsub *s) {   // the slot is free again; blocks are kept (the next images of the class take them)
    pthread_mutex_lock(&g_imgblk_mu); s->b->bits[s->slot >> 6] |= 1ull << (s->slot & 63); s->b->nfree++; pthread_mutex_unlock(&g_imgblk_mu); free(s);
}
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m) { df_add(0, (void *)b, (void *)m); }
void vk_defer_free_image_sub(VkImage i, void *sub) { df_add(4, (void *)i, sub); }
void img_sub_free_(void *s) { img_sub_free((imgsub *)s); }
void vk_defer_free_image(VkImage i, VkDeviceMemory m) { df_add(1, (void *)i, (void *)m); }
void vk_defer_free_view(VkImageView v) { df_add(2, (void *)v, NULL); }
// Descriptor pools: the allocator counts what each set takes and moves to a new pool before any count runs out. Adreno's driver does not refuse an
// allocation past a pool's sizes: it succeeds and the submission then fails as a whole at vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY; NEO's
// handheld frames with 32-slot texture arrays crossed the old 8192 combined-sampler limit at about the 712th draw)
enum { DP_SETS = 4096 };
static const uint32_t DP_CAP[9] = {[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] = 16384, [VK_DESCRIPTOR_TYPE_SAMPLER] = 1024, [VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 65536, [VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] = 1024,
    [VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] = 2048, [VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] = 4096, [VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] = 2048,
    [VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] = 32768, [VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] = 4096};
static nvn_hmap g_dsl_need = {.klen = sizeof(VkDescriptorSetLayout)}; static pthread_mutex_t g_dsl_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_dsl_register(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n) {   // what one set of this layout takes, by type
    uint32_t *need = calloc(9, sizeof *need);
    for (uint32_t i = 0; i < n; i++) if ((uint32_t)b[i].descriptorType < 9) need[b[i].descriptorType] += b[i].descriptorCount;
    pthread_mutex_lock(&g_dsl_mu); nvn_hm_put(&g_dsl_need, &l, need); pthread_mutex_unlock(&g_dsl_mu);
}
static VkDescriptorSet descriptor_alloc(vksub_t *s, VkDescriptorSetLayout l, const uint32_t *need) {
    for (;;) {
        if (s->dp_cur == s->ndp) {
            VkDescriptorPoolSize sz[9]; uint32_t nz = 0;
            for (int t = 0; t < 9; t++) if (DP_CAP[t]) sz[nz++] = (VkDescriptorPoolSize){(VkDescriptorType)t, DP_CAP[t]};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, DP_SETS, nz, sz};
            if (s->ndp == s->capdp) { s->capdp = s->capdp ? 2 * s->capdp : 4; s->dp = realloc(s->dp, s->capdp * sizeof *s->dp); }
            VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &s->dp[s->ndp++]));
        }
        if (s->dp_cur != s->dp_counted) { memset(s->dp_used, 0, sizeof s->dp_used); s->dp_sets = 0; s->dp_counted = s->dp_cur; }
        int fits = s->dp_sets < DP_SETS;
        if (need) for (int t = 0; t < 9 && fits; t++) if (need[t] && s->dp_used[t] + need[t] > DP_CAP[t]) fits = 0;
        if (!fits) { s->dp_cur++; continue; }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dp[s->dp_cur], 1, &l};
        VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (r == VK_SUCCESS) { s->dp_sets++; if (need) for (int t = 0; t < 9; t++) s->dp_used[t] += need[t]; return set; }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) die("[vk] vkAllocateDescriptorSets: %d", r);
        s->dp_cur++;
    }
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout l) {
    vksub_t *s = cur(); descriptor_reuse *r = &s->reuse; descriptor_reuse_entry *e = NULL;
    if (r->enabled) {
        e = nvn_hm_get(&r->layout, &l);
        if (e && e->used < e->count) {
            atomic_fetch_add_explicit(&g_descriptor_reused, 1, memory_order_relaxed);
            return e->set[e->used++];
        }
    }
    pthread_mutex_lock(&g_dsl_mu); const uint32_t *need = nvn_hm_get(&g_dsl_need, &l); pthread_mutex_unlock(&g_dsl_mu);
    VkDescriptorSet set = descriptor_alloc(s, l, need);
    atomic_fetch_add_explicit(&g_descriptor_allocated, 1, memory_order_relaxed);
    // An unknown layout cannot be budgeted by descriptor type. Keep the normal
    // allocator for it and reset the slot at its next safe recycle. Oversized
    // working sets and entry/handle storage allocation failure fall back without limiting
    // the game's allocations or resetting a pool that still holds recorded work.
    if (r->enabled && !r->discard) {
        if (!need || r->count >= DESCRIPTOR_REUSE_LIMIT) r->discard = 1;
        else {
            if (!e) {
                e = calloc(1, sizeof *e);
                if (e) nvn_hm_put(&r->layout, &l, e); else r->discard = 1;
            }
            if (e && e->count == e->capacity) {
                uint32_t capacity = e->capacity ? e->capacity * 2 : 8;
                VkDescriptorSet *sets = realloc(e->set, sizeof *sets * (size_t)capacity);
                if (sets) { e->set = sets; e->capacity = capacity; } else r->discard = 1;
            }
            if (!r->discard) { e->set[e->count++] = set; e->used = e->count; r->count++; }
        }
    }
    return set;
}

// ---------------- submissions: begin, CPU-visible effects, commit ----------------
static void post(post_op o) { vksub_t *s = cur(); if (s->npost == s->cappost) { s->cappost = s->cappost ? 2 * s->cappost : 64; s->post = realloc(s->post, s->cappost * sizeof *s->post); } s->post[s->npost++] = o; }
void mtl_init(void) { vk_init(); }
static int g_shadow_pools;
static int restart_sync(void) {
#ifdef DM_RELEASE
    return 0;
#else
    static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_X_RESTART_SYNC"); v = e && atoi(e) == 1; if (v) VKLOG("DM_VK_X_RESTART_SYNC: copied pools sync at submission continuations too"); } return v;
#endif
}
static void sub_begin(int continuation) {
    vk_init();
    uint64_t cpu_begin = g_segment_q ? ucopy_now() : 0;
    pthread_mutex_lock(&g_sub_lock);
    // the next slot of the ring (g_cur is -1 between submissions, so it cannot say where the ring is: every submission took slot 0 and waited in slot_recycle
    // for the one before to finish on the GPU, which kept the replay of a frame from overlapping the GPU work of the last)
    static int next; g_cur = next; next = (next + 1) % NSLOT; vksub_t *s = &g_slot[g_cur];
    slot_recycle(s);
    vk_encode_begin();
    s->cpu_begin = cpu_begin; s->cpu_ready = s->cpu_first = 0;
    s->npost = 0; s->up_used = 0; s->writes.count = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->up = s->ups[0]; s->nup_used = 1; s->nmain_sent = 0;
    s->deferred_pending = 0; memset(&s->deferred_part, 0, sizeof s->deferred_part);
    vk_encode_begin_checked(s->main, &bi); g_cmd_gen++;
    // copied pools: what the game wrote before submitting goes into the GPU copy before any part of the submission can reach the GPU (DM_VK_CB_DRAWS sends
    // parts early; synced at the commit, those parts read the copy as the last submission left it), and only once no earlier submission still reads it
    pthread_rwlock_rdlock(&g_pool_mu); int shadow = 0; for (int i = 0; i < g_npool; i++) shadow |= g_pool[i]->shadow != NULL; pthread_rwlock_unlock(&g_pool_mu);
    g_shadow_pools = shadow;
    // A continuation (vk_sub_restart: one NVN submission split, as a depth-compare snapshot does) syncs nothing: its commands were submitted by the game
    // before the sync that began the first part, and the GPU copy already holds the first part's own writes, in queue order. Waiting there for the first part
    // and its copy-back serialized the frame where every pool is copied: Mali, V2405A, ~19.5 ms a frame in the BotW world inside one compare snapshot.
    // DM_VK_X_RESTART_SYNC=1 (start-up, debug builds): sync at continuations too, as before
    if (shadow && (!continuation || restart_sync())) { mtl_wait(g_fence, ~0ull); vk_pools_sync(NULL); }   // (after the completion thread's copy-back of the last submission, not only its GPU work)
    if (g_tsq) { vkCmdResetQueryPool(s->main, g_tsq, 2 * (uint32_t)g_cur, 2); vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur); }
    if (g_segment_q) { vkCmdResetQueryPool(s->main, g_segment_q, (uint32_t)(g_cur * MAIN_CBS * 2), MAIN_CBS * 2); segment_mark(s, 0); }
    if (g_cq) vkCmdResetQueryPool(s->main, g_cq, (uint32_t)g_cur * CQ, CQ);
    s->ncq = 0;
    g_npass[g_cur] = 0; if (pass_times_on()) vkCmdResetQueryPool(s->main, g_pq, (uint32_t)(g_cur * PASSQ * 2), PASSQ * 2);
    if (continuation) vk_exec_continue(); else vk_exec_reset();
    if (g_segment_q) s->cpu_ready = ucopy_now();
}
void mtl_sub_begin(void) { DM_SYNC_SCOPE("sync: sub begin"); sub_begin(0); }
void vk_sub_restart(void) { (void)mtl_sub_commit(); sub_begin(1); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, -1}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    vksub_t *s = cur(); int q = -1;
    if (g_cq && (type == 0 || type == 0x10) && s->ncq < CQ) { q = g_cur * CQ + s->ncq++; vkCmdWriteTimestamp(vk_cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_cq, (uint32_t)q); }
    post((post_op){2, NULL, 0, 0, addr, type, q});
}
static VkMemoryBarrier2 full_barrier(void) {
    return (VkMemoryBarrier2){VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}
void vk_img_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
void vk_gpu_wrote(uint64_t a, uint64_t n);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!n) return;
    if (!vk_buf_at(src, n, &sb, &so) || !vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(1, "buffer copy outside the memory pools (%#llx → %#llx, %llu bytes)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferCopy r = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &r);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    VkBuffer db; VkDeviceSize dof;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(2, "buffer fill outside the memory pools"); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    vkCmdFillBuffer(cb, db, dof, n & ~3ull, value);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
// an inline uniform update (UpdateUniformBuffer): the bytes are taken at record time, copied in GPU order from scratch memory
// Inline uniform updates (UpdateUniformBuffer) are deferred instead of ending the render pass (as nvn_mtl.m's ubd_*): a copy cannot run inside a pass, and
// ending it stores every attachment and loads it again in the next pass, which on a tiler is a round trip of the whole tile memory per update (Unity updates
// before nearly every draw: NEO had about 145 passes for 148 draws a frame). NVN applies the update in command order, so a draw after it whose uniform
// binding overlaps it reads a shadow: memory with the deferred updates applied in order (vk_ubd_shadow). The real copies are made in order when a pass
// ends anyway, and at the end of the submission (vk_ubd_land), so later passes, copies, dispatches and the CPU see them as before.
#include "uniform_overlay.h"
#include "uniform_versions.h"
typedef struct { vk_uniform_span span; VkBuffer sb; VkDeviceSize so; } ubd_t;
enum { UBD_MAX = 512 };
static ubd_t g_ubd[UBD_MAX]; static int g_nubd; static vk_uniform_versions g_ubd_versions = {.gen = 1};
uint64_t g_ubd_gen = 1, g_ubd_landed, g_ubd_shadows, g_ubd_bytes, g_ns_submit, g_ubd_superseded;
static void ubd_changed(uint64_t a, uint64_t n) {
    vk_uniform_changed(&g_ubd_versions, a, n); g_ubd_gen = g_ubd_versions.gen;
}
void vk_end_pass_land(void);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n) {
    VkBuffer db, sb; VkDeviceSize dof, so;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(3, "uniform update outside the memory pools"); return; }
    // an update of exactly the range of a deferred one supersedes it: the older one goes (Unity rewrites the same few constant buffers before every draw,
    // so the list keeps about one entry per buffer region, which keeps the landing to a few copies and the shadows cheap to build)
    for (int i = g_nubd - 1; i >= 0; i--) if (g_ubd[i].span.lo == dst && g_ubd[i].span.hi == dst + n) { memmove(&g_ubd[i], &g_ubd[i + 1], sizeof *g_ubd * (size_t)(g_nubd - i - 1)); g_nubd--; g_ubd_superseded++; break; }
    if (g_nubd == UBD_MAX) vk_end_pass_land();   // too many distinct ranges: they land now
    uint8_t *p = vk_scratch(n, 4, &sb, &so); memcpy(p, data, n);
    g_ubd[g_nubd++] = (ubd_t){{dst, dst + n, 0, p}, sb, so}; ubd_changed(dst, n);
}
// the draw bisection (gfx_vk_exec.c draw_allowed): the outcome of this submission
extern int g_bis_on, g_bis_lo, g_bis_hi, g_bis_trace, g_bis_var; void vk_bisect_reset_count(void);
static void vk_bisect_result(int ok) {
    static int done, draws_seen; extern uint32_t g_sub_ops[64]; int nd = (int)(g_sub_ops[25] + g_sub_ops[26]);
    if (done) {   // the found draw: one change per submission, then the draw is no longer traced
        if (g_bis_trace >= 0) {
            static const char *what[] = {"as recorded", "depth bias values 0", "no depth bias", "dummy textures", "vertex offset 0", "zero vertex buffer", "in a new pass", "skipped alone (the rest drawn)", "without their fragment shader", "with zero uniform buffers", "with their quad draws skipped", "with all but their quad draws skipped"};
            VKLOG("bisection: draws from %d on %s: %s", g_bis_trace, what[g_bis_var], ok ? "ACCEPTED" : "refused");
            if (++g_bis_var > 11) { g_bis_trace = -1; g_bis_var = 0; }
        }
        vk_bisect_reset_count(); return; }
    if (!g_bis_on) { if (!ok) { g_bis_on = 1; g_bis_lo = 0; g_bis_hi = nd + 1; draws_seen = nd; VKLOG("bisecting the refused submission's %d draws", nd); } vk_bisect_reset_count(); return; }
    int mid = (g_bis_lo + g_bis_hi) / 2;
    if (ok) g_bis_lo = mid; else g_bis_hi = mid;
    if (g_bis_hi - g_bis_lo <= 1) { done = 1; g_bis_on = 0; g_bis_trace = g_bis_lo; VKLOG("bisection: draw %d of %d is the first the driver refuses (traced in the next submission)", g_bis_lo, draws_seen); }
    vk_bisect_reset_count();
}
void vk_ubd_land_drop(void) { ucopy_drain(); g_nubd = 0; ubd_changed(0, 0); }
// Landed updates still in flight: their copies are queued but the GPU may not have run them, so memory may not hold them yet; a shadow built on the
// CPU (vk_ubd_read) applies them over memory until their submission completes. Without this a shadow mixed the newest update with the frame before's
// memory (shadows detached from moving characters, far pedestrians flickering).
typedef vk_uniform_span ubd_infl_t;   // data: the update's bytes in its submission's scratch memory, which is not reused before that submission completes
static ubd_infl_t *g_infl; static int g_ninfl, g_capinfl;
static void infl_prune(void) {
    uint64_t done = vk_fence_done(); int o = 0;
    for (int i = 0; i < g_ninfl; i++) if (g_infl[i].fence > done) g_infl[o++] = g_infl[i];
    g_ninfl = o;
}
static void sub_gpu_wrote(uint64_t a, uint64_t n);
void vk_ubd_land(VkCommandBuffer cb) {   // outside a render pass
    if (!g_nubd) return;
    infl_prune();
    for (int i = 0; i < g_nubd; i++) {
        if (g_ninfl == g_capinfl) { g_capinfl = g_capinfl ? 2 * g_capinfl : 256; g_infl = realloc(g_infl, sizeof *g_infl * (size_t)g_capinfl); }
        // (the bytes used to be copied into the heap here: a malloc, memcpy and free of up to 64 KB per update, for bytes the scratch memory keeps anyway)
        g_infl[g_ninfl++] = (ubd_infl_t){g_ubd[i].span.lo, g_ubd[i].span.hi, g_fence + 1, g_ubd[i].span.data};   // the fence of the submission being recorded
    }
    vk_img_barrier_all(cb);
    for (int i = 0; i < g_nubd; i++) { VkBuffer db; VkDeviceSize dof; if (vk_buf_at(g_ubd[i].span.lo, g_ubd[i].span.hi - g_ubd[i].span.lo, &db, &dof)) {
        VkBufferCopy r = {g_ubd[i].so, dof, g_ubd[i].span.hi - g_ubd[i].span.lo}; vkCmdCopyBuffer(cb, g_ubd[i].sb, db, 1, &r); sub_gpu_wrote(g_ubd[i].span.lo, g_ubd[i].span.hi - g_ubd[i].span.lo); } }
    vk_img_barrier_all(cb);
    g_ubd_landed += (uint64_t)g_nubd; g_nubd = 0; ubd_changed(0, 0);
}
int vk_ubd_overlaps(uint64_t a, uint64_t n) { for (int i = 0; i < g_nubd; i++) if (g_ubd[i].span.lo < a + n && a < g_ubd[i].span.hi) return 1; return 0; }
// GPU writes to pool memory (buffer copies and fills, texture → buffer copies, storage buffers of dispatches) whose submission may not have completed:
// until it has, a CPU read of that memory (vk_ubd_read: a shadow, a handle in a uniform buffer) may see the bytes from before them or after them
typedef struct { uint64_t lo, hi, fence; } gpuw_t; enum { GW_MAX = 1024 };
static gpuw_t g_gw[GW_MAX]; static int g_ngw; uint64_t g_ubd_racy;
static void sub_gpu_wrote(uint64_t a, uint64_t n) {   // into the open submission's list (pools_copy_back)
    if (g_cur < 0 || !n || !atomic_load_explicit(&g_ucopy_shadow_pools, memory_order_relaxed)) return;
    if (!vk_pool_writes_add(&g_slot[g_cur].writes, a, n))
        die("[vk] GPU write span could not be recorded (%#llx +%#llx)", (unsigned long long)a, (unsigned long long)n);
}
void vk_gpu_wrote(uint64_t a, uint64_t n) {
    uint64_t f = g_fence + 1;
    for (int i = g_ngw - 1; i >= 0 && g_gw[i].fence == f; i--) if (g_gw[i].lo <= a && a + n <= g_gw[i].hi) return;
    if (g_ngw == GW_MAX) { uint64_t done = vk_fence_done(); int o = 0; for (int i = 0; i < g_ngw; i++) if (g_gw[i].fence > done) g_gw[o++] = g_gw[i]; g_ngw = o; }
    if (g_ngw == GW_MAX) { memmove(g_gw, g_gw + 1, sizeof *g_gw * (GW_MAX - 1)); g_ngw--; }
    g_gw[g_ngw++] = (gpuw_t){a, a + n, f};
    sub_gpu_wrote(a, n);
}
int vk_gpu_pending(uint64_t a, uint64_t n) {
    if (!g_ngw) return 0;
    uint64_t done = vk_fence_done();
    for (int i = g_ngw - 1; i >= 0; i--) if (g_gw[i].fence > done && g_gw[i].lo < a + n && a < g_gw[i].hi) return 1;
    return 0;
}
// what a GPU read of [a, a + n) sees at this point of the command stream, into dst (memory, then the deferred updates in order)
typedef struct { vk_copy_op ops[64]; size_t count; } ucopy_batch;
static void ucopy_flush(ucopy_batch *batch) {
    if (batch->count) { vk_copyq_push_many(&g_ucopy, batch->ops, batch->count); batch->count = 0; }
}
static void ucopy_emit(void *context, uint8_t *dst, const uint8_t *src, size_t size) {
    ucopy_batch *batch = context;
    if (batch->count == sizeof batch->ops / sizeof *batch->ops) ucopy_flush(batch);
    batch->ops[batch->count++] = (vk_copy_op){dst, src, size}; g_ucopy_jobs++; g_ucopy_bytes += size;
}
static void ubd_read_(uint64_t a, uint64_t n, uint8_t *dst, int queued) {
    if (vk_gpu_pending(a, n)) { static int k; g_ubd_racy++; if (k < 20) { k++; int cov = 0; for (int i = 0; i < g_nubd; i++) if (g_ubd[i].span.lo <= a && a + n <= g_ubd[i].span.hi) cov = 1;
        VKLOG("[RED] a CPU read of %#llx +%llu (%s) overlaps a GPU write not known to be complete", (unsigned long long)a, (unsigned long long)n, cov ? "covered by a deferred update" : "partly from memory"); } }
    vk_uniform_list in = {g_infl, sizeof *g_infl, g_ninfl, 1, g_ninfl ? vk_fence_done() : 0};
    vk_uniform_list pending = {g_ubd, sizeof *g_ubd, g_nubd, 0, 0};
    extern int g_vk_bind_cache;
    ucopy_batch batch; batch.count = 0;
    vk_uniform_emit(dst, (const uint8_t *)(uintptr_t)a, a, n, in, pending, g_vk_bind_cache,
                    queued ? ucopy_emit : vk_uniform_copy_now, &batch);
    if (queued) ucopy_flush(&batch);
}
void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst) { ubd_read_(a, n, dst, 0); }
// A uniform binding [a, a + n) that deferred updates overlap: its immutable shadow.
// An unrelated update keeps it valid; landing invalidates every scratch reference.
int vk_ubd_shadow(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_nubd || !vk_ubd_overlaps(a, n)) return 0;   // (a binding no deferred update touches reads memory on the GPU, after the in-flight copies: no shadow needed)
    enum { NC = 512 }; static struct { uint64_t a, n, gen, copied; VkBuffer b; VkDeviceSize o; const uint8_t *data; } c[NC];
    extern int g_x_noshc, g_vk_bind_cache, g_vk_bind_verify;
    unsigned k = g_vk_bind_cache ? vk_uniform_cache_slot(a, n, NC) : (unsigned)((a ^ n * 0x9E3779B97F4A7C15ull) >> 6) % 64;
    if (c[k].a == a && c[k].n == n && !g_x_noshc && (g_vk_bind_cache ? vk_uniform_unchanged(&g_ubd_versions, c[k].gen, a, n) : c[k].gen == g_ubd_versions.gen)) {
        int ok = 1;
        if (g_vk_bind_verify) {
            ucopy_wait(c[k].copied);
            uint8_t *expected = malloc(n);
            if (expected) { vk_ubd_read(a, n, expected); ok = !memcmp(expected, c[k].data, n); free(expected); }
            if (!ok) VKRED_ONCE(95, "reused uniform shadow differs from current bytes (%#llx +%llu)", (unsigned long long)a, (unsigned long long)n);
        }
        if (ok) { *buf = c[k].b; *off = c[k].o; c[k].gen = g_ubd_versions.gen; return 1; }
    }
    uint8_t *d = vk_scratch(n, V.props.limits.minUniformBufferOffsetAlignment, buf, off);
    int queued = g_ucopy_on && n >= 4096 && !atomic_load_explicit(&g_ucopy_shadow_pools, memory_order_relaxed) && !vk_gpu_pending(a, n) && vk_copyq_start(&g_ucopy, ucopy_begin);
    ubd_read_(a, n, d, queued); g_ubd_bytes += n;
    c[k].a = a; c[k].n = n; c[k].gen = g_ubd_versions.gen; c[k].b = *buf; c[k].o = *off; c[k].data = d; g_ubd_shadows++;
    c[k].copied = queued ? vk_copyq_head(&g_ucopy) : 0;
    if (queued && g_vk_bind_verify) {
        ucopy_wait(c[k].copied);
        uint8_t *expected = malloc(n);
        if (expected) { vk_ubd_read(a, n, expected);
            if (memcmp(d, expected, n)) { VKRED_ONCE(96, "queued uniform shadow differs from synchronous bytes (%#llx +%llu)", (unsigned long long)a, (unsigned long long)n); memcpy(d, expected, n); }
            free(expected); }
    }
    return 1;
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

// completion: a queue of committed submissions, each with its CPU-visible effects
typedef struct cpend { uint64_t fence, frame; post_op *ops; int n; int slot, rslot; int npass, nsegments, nup_segments; pass_meta *pmeta; struct cpend *next; } cpend;
static cpend *g_ch, *g_ct; static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return g * 12 / 625; }
// GPU time per NVN frame (the sum of its submissions' timestamp spans), for the timing line's GPU column: the frame a submission belongs to is the one being replayed
static uint64_t g_cur_frame, g_frame_gpu[64], g_frame_gpu_of[64];
void mtl_set_frame(uint64_t f) { __atomic_store_n(&g_cur_frame, f, __ATOMIC_RELAXED); }
double mtl_gpu_ms(uint64_t f) { return g_frame_gpu_of[f % 64] == f + 1 ? g_frame_gpu[f % 64] / 1e6 : -1; }
double vk_gpu_ms_recent(void) {   // the latest finished frame's GPU time among the last 8 (-1: none known)
    uint64_t f = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
    for (uint64_t k = 2; k < 8 && k <= f; k++) { double g = mtl_gpu_ms(f - k); if (g >= 0) return g; }
    return -1;
}
double vk_gpu_ms_budget(double period_ms, double *trimmed_ms) {
    uint64_t f = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
    double spans[8], latest = -1; int n = 0;
    // Keep the raw latest lookup (f-2..f-7); only the median reaches further
    // back through known ring entries, up to eight finished spans.
    for (uint64_t k = 2; k < 64 && k <= f && n < 8; k++) {
        double g = mtl_gpu_ms(f - k); if (g < 0) continue;
        if (!n && k < 8) latest = g;
        spans[n++] = g;
    }
    return vk_pace_budget_estimate(latest, spans, n, period_ms, trimmed_ms);
}
// Internal completion protects command-pool host access and slot reuse.
static void completion_retire(cpend *c) {
    DM_SYNC_FENCE_SCOPE("fence: retire", c->fence);
    if (g_prereset > 0 && c->rslot >= 0) { vksub_t *s = &g_slot[c->rslot];
        if (s->fence == c->fence) { for (int i = 0; i < s->nmain_alloc; i++) vkResetCommandBuffer(s->mains[i], 0); for (int i = 0; i < s->nup_alloc; i++) vkResetCommandBuffer(s->ups[i], 0); } }
    pthread_mutex_lock(&g_done_mu); if (c->fence > g_done) __atomic_store_n(&g_done, c->fence, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_done_cv); pthread_mutex_unlock(&g_done_mu);
}
static void *completion_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-vk-complete");
    for (;;) {
        pthread_mutex_lock(&g_cmu);
        while (!g_ch) pthread_cond_wait(&g_ccv, &g_cmu);
        cpend *c = g_ch; pthread_mutex_unlock(&g_cmu);
        if (V.has_timeline) wait_fence(c->fence); else vkWaitForFences(V.dev, 1, &g_fring[c->fence % NFRING], VK_TRUE, UINT64_MAX);
        { DM_SYNC_FENCE_SCOPE("fence: CPU effects", c->fence);
        uint64_t segment_ticks[MAIN_CBS * 4]; unsigned segment_count = 0;
        if (g_shadow_pools && c->rslot >= 0) pools_copy_back(&g_slot[c->rslot]);
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            { uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod); g_gpu_ns += ns; int k = (int)(c->frame % 64);
              if (g_frame_gpu_of[k] != c->frame + 1) { g_frame_gpu[k] = 0; g_frame_gpu_of[k] = c->frame + 1; } g_frame_gpu[k] += ns; } }
        if (g_segment_q && c->slot >= 0 && c->nsegments) {
            uint64_t *t = segment_ticks, span, covered, gaps;
            if (vkGetQueryPoolResults(V.dev, g_segment_q, (uint32_t)(c->slot * MAIN_CBS * 2), (uint32_t)c->nsegments * 2,
                    sizeof segment_ticks, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && vk_gpu_segments(t, (unsigned)c->nsegments, &span, &covered, &gaps)) {
                segment_count = (unsigned)c->nsegments;
                double period = V.props.limits.timestampPeriod;
                pthread_mutex_lock(&g_segment_mu);
                g_segment_stats.span += (uint64_t)(span * period); g_segment_stats.covered += (uint64_t)(covered * period);
                g_segment_stats.gaps += (uint64_t)(gaps * period); g_segment_stats.parts += (uint64_t)c->nsegments; g_segment_stats.submissions++;
                pthread_mutex_unlock(&g_segment_mu);
            } else { pthread_mutex_lock(&g_segment_mu); g_segment_stats.incomplete++; pthread_mutex_unlock(&g_segment_mu); }
        }
        if (g_upload_segment_q && segment_count && c->nup_segments) {
            size_t offset = segment_count * 2 * sizeof *segment_ticks;
            if (g_upload_segment_q && vkGetQueryPoolResults(V.dev, g_upload_segment_q, (uint32_t)(c->slot * MAIN_CBS * 2), (uint32_t)c->nup_segments * 2,
                    sizeof segment_ticks - offset, segment_ticks + segment_count * 2, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) segment_count += (unsigned)c->nup_segments;
            else segment_count = 0;
        }
        if (g_upload_segment_q) {
            uint64_t span, covered, gaps, up_span = 0, up_covered = 0, up_gaps = 0;
            int valid = segment_count && vk_gpu_segments(segment_ticks, segment_count, &span, &covered, &gaps);
            if (valid && c->nup_segments) valid = vk_gpu_segments(segment_ticks + c->nsegments * 2, (unsigned)c->nup_segments, &up_span, &up_covered, &up_gaps);
            pthread_mutex_lock(&g_segment_mu);
            if (valid) { double period = V.props.limits.timestampPeriod;
                g_segment_stats.all_span += (uint64_t)(span * period); g_segment_stats.all_covered += (uint64_t)(covered * period);
                g_segment_stats.all_gaps += (uint64_t)(gaps * period); g_segment_stats.upload_covered += (uint64_t)(up_covered * period);
            } else g_segment_stats.all_incomplete++;
            pthread_mutex_unlock(&g_segment_mu);
        }
        if (g_pq && c->npass && c->slot >= 0) {   // read submitted samples even after capture mode stops recording
            int np = c->npass; uint64_t t[PASSQ * 2];
            if (vkGetQueryPoolResults(V.dev, g_pq, (uint32_t)(c->slot * PASSQ * 2), (uint32_t)np * 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                pass_capture(c->frame, c->fence, c->pmeta, t, np);
                pass_accumulate(c->pmeta, t, np);
            } else { pthread_mutex_lock(&g_pass_mu); g_pass_invalid += (uint64_t)np; pthread_mutex_unlock(&g_pass_mu); }
        } else if (g_pq && c->npass) { pthread_mutex_lock(&g_pass_mu); g_pass_invalid += (uint64_t)c->npass; pthread_mutex_unlock(&g_pass_mu); }
        uint64_t now = gpu_ticks(), tend = 0;
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) tend = t[1]; }
        for (int i = 0; i < c->n; i++) {
            post_op *o = &c->ops[i];
            if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
            uint64_t ts = now, tq;
            if (o->q >= 0 && tend && vkGetQueryPoolResults(V.dev, g_cq, (uint32_t)o->q, 1, 8, &tq, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && tq <= tend) {
                uint64_t back = (uint64_t)((double)(tend - tq) * V.props.limits.timestampPeriod * 12 / 625); if (back < now) ts = now - back; }
            // render_cond: a timestamp report's counter field is 0, as on the GPU (gfx_vk_exec.c render_cond evaluates conditions on it)
            uint64_t rec[2] = {o->type == 0 || o->type == 0x10 ? (DM_GAME_RENDER_COND ? 0 : ts) : o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (1u << 20) : 0, ts}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
        // All guest-visible effects above are complete. Internal retirement stays later.
        completion_notify_guest(c->fence);
        }
        pthread_mutex_lock(&g_cmu); g_ch = c->next; if (!g_ch) g_ct = NULL; pthread_mutex_unlock(&g_cmu);
        completion_retire(c);
        free(c->pmeta); free(c->ops); free(c);
    }
    return NULL;
}
static void completion_start(void) { pthread_t t; if (pthread_create(&t, NULL, completion_main, NULL)) die("[vk] completion thread"); pthread_detach(t); }
uint64_t mtl_sub_commit(void) {
    DM_SYNC_SCOPE("sync: sub commit");
    { void vk_prof_end(void); vk_prof_end(); }
    vksub_t *s = cur();
    vk_end_pass_land();
    if (g_tsq) vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur + 1);
    segment_mark(s, 1);
    VkResult er = vkEndCommandBuffer(s->main); split_collect(s); if (s->cb_err && er >= 0) er = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (er < 0) {   // the driver refused something recorded in it: logged with what the submission held, then it is dropped and the game goes on
        extern uint32_t g_sub_ops[64]; char ops[600]; int o = 0;
        for (int i = 0; i < 64 && o < (int)sizeof ops - 16; i++) if (g_sub_ops[i]) o += snprintf(ops + o, sizeof ops - (size_t)o, " %d:%u", i, g_sub_ops[i]);
        static int n; if (n++ < 3) VKLOG("[RED] vkEndCommandBuffer failed (%d): submission dropped; record ops (op:count)%s", er, ops);
        vk_bisect_result(0);
        if (s->nmain_sent) {   // parts already sent may still run: nothing more is submitted (the submission below only signals its fence)
            s->nmain_sent = s->nmain_used; s->up_used = 0; s->cb_err = 0; s->no_ts = 1; vk_ubd_land_drop();
        } else {
            vkResetCommandPool(V.dev, s->pool, 0); VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->no_ts = 1;   // no timestamps: the slot's queries still hold an older submission's
            if (s->nup_used > 1) { s->up = s->ups[0]; s->nup_used = 1; }
            vk_encode_begin_checked(s->main, &bi); g_cmd_gen++; s->up_used = 0; vk_ubd_land_drop();
            VKCHK(vkEndCommandBuffer(s->main));
        }
    }
    else vk_bisect_result(1);
    { extern uint32_t g_sub_ops[64]; memset(g_sub_ops, 0, sizeof g_sub_ops); }
    if (s->up_used) { vk_img_barrier_all(s->up); upload_segment_mark(s, 1); VKCHK(vkEndCommandBuffer(s->up)); }
    uint64_t f = ++g_fence;
    VkCommandBuffer cbs[1 + MAIN_CBS]; uint32_t ncb = 0; if (s->up_used) cbs[ncb++] = s->up; for (int i = s->nmain_sent; i < s->nmain_used; i++) cbs[ncb++] = s->mains[i];
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &f};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, ncb, cbs, 1, &g_timeline};
    VkFence fe = VK_NULL_HANDLE;
    if (!V.has_timeline) {   // the ring's fence for f: free once the completion thread has passed f - NFRING (already so: slot_recycle waited for f - NSLOT)
        if (f > NFRING) mtl_wait(f - NFRING, ~0ull);
        fe = g_fring[f % NFRING]; VKCHK(vkResetFences(V.dev, 1, &fe)); si.pNext = NULL; si.signalSemaphoreCount = 0;
    }
    ucopy_drain();   // no GPU read may see an unfinished shadow
    struct timespec t0, t1; VkResult r;
    { DM_SYNC_FENCE_SCOPE("fence: final submit", f);
    clock_gettime(CLOCK_MONOTONIC, &t0); pthread_mutex_lock(&g_vk_queue_mu);
    if (g_segment_q && !s->cpu_first) s->cpu_first = ucopy_now();
    r = vkQueueSubmit(V.q, 1, &si, fe); pthread_mutex_unlock(&g_vk_queue_mu); clock_gettime(CLOCK_MONOTONIC, &t1); g_ns_submit += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    }
    if (r) { void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit: %d (device lost?)", r); }
    if (g_segment_q && s->cpu_begin && s->cpu_ready >= s->cpu_begin && s->cpu_first >= s->cpu_ready) {
        uint64_t end = (uint64_t)t1.tv_sec * 1000000000ull + (uint64_t)t1.tv_nsec;
        if (end >= s->cpu_first) {
            pthread_mutex_lock(&g_segment_mu);
            g_segment_stats.cpu_setup += s->cpu_ready - s->cpu_begin;
            g_segment_stats.cpu_first += s->cpu_first - s->cpu_ready;
            g_segment_stats.cpu_tail += end - s->cpu_first;
            g_segment_stats.cpu_count++; g_segment_stats.cpu_early += s->nmain_sent != 0;
            pthread_mutex_unlock(&g_segment_mu);
        }
    }
    s->fence = f;
    cpend *c = calloc(1, sizeof *c); c->fence = f; c->frame = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED); c->n = s->npost; c->rslot = g_cur; c->slot = s->no_ts ? -1 : g_cur; s->no_ts = 0;
    dm_sync_pair_point("frame: submit", f, c->frame);
    c->nsegments = s->nmain_used;
    c->nup_segments = s->nup_used - 1 + (s->up_used != 0);
    c->npass = g_npass[g_cur];
    if (c->npass) {
        c->pmeta = malloc(sizeof *c->pmeta * (size_t)c->npass);
        if (c->pmeta) memcpy(c->pmeta, g_pmeta[g_cur], sizeof *c->pmeta * (size_t)c->npass);
        else { pthread_mutex_lock(&g_pass_mu); g_pass_invalid += (uint64_t)c->npass; pthread_mutex_unlock(&g_pass_mu); c->npass = 0; VKRED_ONCE(96, "cannot allocate pass timing metadata"); }
    }
    if (s->npost) { c->ops = malloc(s->npost * sizeof *c->ops); memcpy(c->ops, s->post, s->npost * sizeof *c->ops); }
    pthread_mutex_lock(&g_cmu); if (g_ct) g_ct->next = c; else g_ch = c; g_ct = c; pthread_cond_signal(&g_ccv); pthread_mutex_unlock(&g_cmu);
    s->npost = 0; g_cur = -1;
    vk_encode_end();
    pthread_mutex_unlock(&g_sub_lock);
    return f;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    DM_SYNC_FENCE_SCOPE(timeout_ns ? "fence: host wait" : "fence: host poll", fence);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_done_mu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &dl)) break;
    int ok = g_done >= fence; pthread_mutex_unlock(&g_done_mu);
    return ok;
}
// NVN SyncWait only needs its submitted work and CPU-visible effects complete.
// Backend drain/readback/slot reuse retain mtl_wait's stronger retirement boundary.
int vk_guest_wait(uint64_t fence, uint64_t timeout_ns) {
    DM_SYNC_FENCE_SCOPE(timeout_ns ? "fence: guest wait" : "fence: guest poll", fence);
    if (!guest_notify_enabled()) return mtl_wait(fence, timeout_ns);
    atomic_fetch_add_explicit(&g_guest_wait_calls, 1, memory_order_relaxed);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_guest_done_mu);
    while (g_guest_done < fence) if (pthread_cond_timedwait(&g_guest_done_cv, &g_guest_done_mu, &dl)) break;
    int ok = g_guest_done >= fence; pthread_mutex_unlock(&g_guest_done_mu);
    if (ok && fence > vk_fence_done()) atomic_fetch_add_explicit(&g_guest_wait_ahead, 1, memory_order_relaxed);
    return ok;
}
void vk_guest_notify_report(void) {
    if (!guest_notify_enabled()) return;
    pthread_mutex_lock(&g_guest_done_mu); uint64_t visible = g_guest_done; pthread_mutex_unlock(&g_guest_done_mu);
    VKLOG("guest fence notification (cumulative): visible %llu, retired %llu, %llu waits, %llu observations before retirement",
          (unsigned long long)visible, (unsigned long long)vk_fence_done(),
          (unsigned long long)atomic_load_explicit(&g_guest_wait_calls, memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g_guest_wait_ahead, memory_order_relaxed));
}
void mtl_finish(void) { mtl_wait(g_fence, ~0ull); }
void mtl_drain(void) { mtl_finish(); vk_pipeline_cache_save(); }
