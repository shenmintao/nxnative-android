// Bounded present snapshots: the game's image is copied before it is returned,
// while the present worker displays a host image. Capacity is waited at acquire,
// outside the game's present lock. All work uses the same Vulkan queue.
#include "gfx_vk.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ANativeWindow *dm_front_window(void);
extern pthread_mutex_t g_present_mu, g_vk_queue_mu;
int vk_present_ticket_ready(uint64_t ticket);

enum { SNAP_MAX = 4 };
static struct {
    vkimg_t image;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence copied;
    VkImage source;
    uint64_t ticket;
} g_snap[SNAP_MAX];
static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_snap_once = PTHREAD_ONCE_INIT;
static int g_snap_n;
static _Atomic int g_snap_failed;
static uint64_t g_snap_next;
uint64_t g_snapshot_copies, g_snapshot_fallbacks, g_snapshot_wait_ns;
int vk_snapshot_image(const vkimg_t *image) {
    for (int i = 0; i < SNAP_MAX; ++i) if (image == &g_snap[i].image) return 1;
    return 0;
}

static uint64_t snap_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void snap_config(void) {
    const char *e = getenv("DM_PRESENT_SNAP"); int n = e ? atoi(e) : 0;
    g_snap_n = n > 0 ? n < 2 ? 2 : n > SNAP_MAX ? SNAP_MAX : n : 0;
    if (g_snap_n) VKLOG("present snapshots: %d host images (DM_PRESENT_SNAP), capacity paced at acquire", g_snap_n);
}
int mtl_snap_on(void) {
    pthread_once(&g_snap_once, snap_config);
    return g_snap_n && !atomic_load_explicit(&g_snap_failed, memory_order_relaxed) && V.dev && V.has_dtiming && dm_front_window() != NULL;
}
// Also used before an immediate resource free outside an NVN submission: the
// last core timeline value precedes these independent snapshot copies.
void vk_snapshot_drain(void) {
    pthread_mutex_lock(&g_snap_mu);
    for (int i = 0; i < SNAP_MAX; ++i) if (g_snap[i].copied)
        VKCHK(vkWaitForFences(V.dev, 1, &g_snap[i].copied, VK_TRUE, UINT64_MAX));
    pthread_mutex_unlock(&g_snap_mu);
}
void vk_snapshot_source_free(VkImage image) {
    pthread_mutex_lock(&g_snap_mu);
    for (int i = 0; i < SNAP_MAX; ++i) if (g_snap[i].source == image && g_snap[i].copied)
        VKCHK(vkWaitForFences(V.dev, 1, &g_snap[i].copied, VK_TRUE, UINT64_MAX));
    pthread_mutex_unlock(&g_snap_mu);
}
uint64_t mtl_snap_acquire_wait(void) {
    if (!mtl_snap_on()) return 0;
    uint64_t before = snap_now(); int blocked = 0;
    pthread_mutex_lock(&g_snap_mu);
    unsigned k = (unsigned)(g_snap_next % (uint64_t)g_snap_n);
    if (!vk_present_ticket_ready(g_snap[k].ticket)) { blocked = 1; mtl_present_wait(g_snap[k].ticket); }
    if (g_snap[k].copied && vkWaitForFences(V.dev, 1, &g_snap[k].copied, VK_TRUE, 0) != VK_SUCCESS) {
        blocked = 1; VkResult r = vkWaitForFences(V.dev, 1, &g_snap[k].copied, VK_TRUE, 2000000000ull);
        if (r == VK_ERROR_DEVICE_LOST) die("[vk] device lost waiting for a present snapshot");
    }
    uint64_t wait = blocked ? snap_now() - before : 0;
    g_snapshot_wait_ns += wait;
    pthread_mutex_unlock(&g_snap_mu);
    return wait;
}
static int snap_resources(unsigned k, const vkimg_t *source) {
    if (!g_snap[k].pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
        if (vkCreateCommandPool(V.dev, &pi, NULL, &g_snap[k].pool)) return 0;
        VkCommandBufferAllocateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_snap[k].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vkAllocateCommandBuffers(V.dev, &ci, &g_snap[k].cb)) return 0;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        if (vkCreateFence(V.dev, &fi, NULL, &g_snap[k].copied)) return 0;
    }
    vkimg_t *image = &g_snap[k].image;
    if (image->img && image->w == source->w && image->h == source->h && image->fmt == source->fmt) return 1;
    // Keep the old image intact if this optional allocation fails. A host-menu
    // redraw may still hold its stable vkimg_t pointer.
    VkImage replacement; VkDeviceMemory memory;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, source->fmt, {source->w, source->h, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(V.dev, &ii, NULL, &replacement)) return 0;
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(V.dev, replacement, &requirements);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, requirements.size,
        vk_mem_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (vkAllocateMemory(V.dev, &ai, NULL, &memory)) { vkDestroyImage(V.dev, replacement, NULL); return 0; }
    if (vkBindImageMemory(V.dev, replacement, memory, 0)) { vkDestroyImage(V.dev, replacement, NULL); vkFreeMemory(V.dev, memory, NULL); return 0; }
    if (image->img) {
        pthread_mutex_lock(&g_vk_queue_mu); VKCHK(vkDeviceWaitIdle(V.dev)); pthread_mutex_unlock(&g_vk_queue_mu);
        vkDestroyImage(V.dev, image->img, NULL); vkFreeMemory(V.dev, image->mem, NULL);
    }
    memset(image, 0, sizeof *image); image->img = replacement; image->mem = memory; image->fmt = source->fmt;
    image->w = source->w; image->h = source->h; image->d = image->layers = image->levels = image->samples = 1; image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VKLOG("present snapshot slot %u: %ux%u format %d", k, image->w, image->h, image->fmt);
    return 1;
}
vkimg_t *vk_snapshot_take(vkimg_t *source, uint64_t ticket) {
    if (!mtl_snap_on() || source->samples != 1 || source->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return NULL;
    pthread_mutex_lock(&g_snap_mu);
    unsigned k = (unsigned)(g_snap_next % (uint64_t)g_snap_n);
    if (!vk_present_ticket_ready(g_snap[k].ticket) ||
        (g_snap[k].copied && vkWaitForFences(V.dev, 1, &g_snap[k].copied, VK_TRUE, 0) != VK_SUCCESS)) {
        g_snapshot_fallbacks++; pthread_mutex_unlock(&g_snap_mu); return NULL;
    }
    vkimg_t *image = &g_snap[k].image;
    int resizing = !image->img || image->w != source->w || image->h != source->h || image->fmt != source->fmt;
    if (resizing) pthread_mutex_lock(&g_present_mu);
    if (!snap_resources(k, source)) {
        atomic_store_explicit(&g_snap_failed, 1, memory_order_relaxed);
        VKLOG("present snapshots unavailable: keeping ordinary presentation");
        if (resizing) pthread_mutex_unlock(&g_present_mu);
        pthread_mutex_unlock(&g_snap_mu); return NULL;
    }
    VKCHK(vkResetCommandBuffer(g_snap[k].cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHK(vkBeginCommandBuffer(g_snap[k].cb, &bi));
    if (image->layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(g_snap[k].cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vk_img_barrier_all(g_snap[k].cb);
    VkImageCopy copy = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {source->w, source->h, 1}};
    vkCmdCopyImage(g_snap[k].cb, source->img, VK_IMAGE_LAYOUT_GENERAL, image->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_img_barrier_all(g_snap[k].cb);
    VKCHK(vkEndCommandBuffer(g_snap[k].cb)); VKCHK(vkResetFences(V.dev, 1, &g_snap[k].copied));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_snap[k].cb, 0, NULL};
    pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, g_snap[k].copied); pthread_mutex_unlock(&g_vk_queue_mu);
    if (r) die("[vk] present snapshot submit failed: %d", r);
    g_snap[k].source = source->img;
    if (resizing) pthread_mutex_unlock(&g_present_mu);
    uint64_t after = mtl_last_fence() + 1; if (source->last_use < after) source->last_use = after;
    g_snap[k].ticket = ticket; g_snap_next++; g_snapshot_copies++;
    pthread_mutex_unlock(&g_snap_mu);
    return image;
}
