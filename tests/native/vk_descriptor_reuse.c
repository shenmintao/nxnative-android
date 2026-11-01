#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"
#include "pool_write_ranges.h"
#ifdef _WIN32
#define setenv(key, value, overwrite) _putenv_s(key, value)
#define unsetenv(key) _putenv_s(key, "")
#endif
#define MAIN_CBS 32
#define VKLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define VKCHK(call) assert((call) == VK_SUCCESS)
#define die(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
typedef struct vksub vksub_t;
#include "descriptor_reuse_types.inc"
static struct { VkDevice dev; } V;
static vksub_t slot;
static vksub_t *cur(void) { return &slot; }
static int g_prereset = 1;
static _Atomic uint64_t completed;
static _Atomic int entered, finished, wrong_order;
static pthread_mutex_t completion_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completion_cv = PTHREAD_COND_INITIALIZER;
static void complete(uint64_t fence) {
    pthread_mutex_lock(&completion_mu); atomic_store(&completed, fence);
    pthread_cond_broadcast(&completion_cv); pthread_mutex_unlock(&completion_mu);
}
static int mtl_wait(uint64_t fence, uint64_t timeout) {
    (void)timeout; pthread_mutex_lock(&completion_mu); atomic_store(&entered, 1);
    while (atomic_load(&completed) < fence) pthread_cond_wait(&completion_cv, &completion_mu);
    pthread_mutex_unlock(&completion_mu); return 1;
}
static void wait_fence(uint64_t fence) { (void)mtl_wait(fence, UINT64_MAX); }
static void ucopy_drain(void) {}
static const char *guard_getenv(const char *name) {
    if (!strcmp(name, "DM_VK_SET_REUSE") && atomic_load(&completed) < slot.fence) atomic_store(&wrong_order, 1);
    return getenv(name);
}
static int fail_entry, fail_array;
static void *guard_calloc(size_t n, size_t size) {
    if (fail_entry && n == 1 && size == sizeof(descriptor_reuse_entry)) { fail_entry = 0; return NULL; }
    return calloc(n, size);
}
static void *guard_realloc(void *p, size_t size) {
    if (fail_array && size == 8 * sizeof(VkDescriptorSet)) { fail_array = 0; return NULL; }
    return realloc(p, size);
}
static void vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)b; (void)a; }
static void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; }
static void vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; }
static void vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
static void vkDestroyBufferView(VkDevice d, VkBufferView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
void img_sub_free_(void *p) { (void)p; }
static struct { uint32_t generation, sets, maximum, cap[9], used[9]; } driver_pool[64];
static struct { uintptr_t pool, layout; uint32_t generation; } driver_set[50000];
static uint32_t wants[32][9], pool_count, set_count, reset_count;
static VkResult vkCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)a; unsigned k = ++pool_count; assert(k < 64);
    driver_pool[k].generation = 1; driver_pool[k].maximum = ci->maxSets;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) driver_pool[k].cap[ci->pPoolSizes[i].type] = ci->pPoolSizes[i].descriptorCount;
    *out = (VkDescriptorPool)(uintptr_t)k; return VK_SUCCESS;
}
static VkResult vkResetDescriptorPool(VkDevice d, VkDescriptorPool handle, VkDescriptorPoolResetFlags flags) {
    (void)d; (void)flags; assert(atomic_load(&completed) >= slot.fence || atomic_load(&wrong_order));
    unsigned k = (unsigned)(uintptr_t)handle; driver_pool[k].generation++;
    driver_pool[k].sets = 0; memset(driver_pool[k].used, 0, sizeof driver_pool[k].used); reset_count++; return VK_SUCCESS;
}
static VkResult vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    (void)d; assert(ai->descriptorSetCount == 1); unsigned k = (unsigned)(uintptr_t)ai->descriptorPool;
    unsigned l = (unsigned)(uintptr_t)ai->pSetLayouts[0]; assert(l < 32);
    assert(driver_pool[k].sets < driver_pool[k].maximum);
    for (unsigned t = 0; t < 9; t++) {
        // A driver that accepts an oversubscribed pool must never be relied on.
        assert(driver_pool[k].used[t] + wants[l][t] <= driver_pool[k].cap[t]);
        driver_pool[k].used[t] += wants[l][t];
    }
    driver_pool[k].sets++; unsigned h = ++set_count; assert(h < 50000);
    driver_set[h].pool = k; driver_set[h].layout = l; driver_set[h].generation = driver_pool[k].generation;
    *out = (VkDescriptorSet)(uintptr_t)h; return VK_SUCCESS;
}
#define getenv guard_getenv
#define calloc guard_calloc
#define realloc guard_realloc
#include "descriptor_reuse_impl.inc"
#undef getenv
#undef calloc
#undef realloc
static VkDescriptorSet take(unsigned layout) {
    VkDescriptorSet set = vk_alloc_set((VkDescriptorSetLayout)(uintptr_t)layout);
    unsigned h = (unsigned)(uintptr_t)set; assert(h && h <= set_count);
    assert(driver_set[h].layout == layout);
    assert(driver_set[h].generation == driver_pool[driver_set[h].pool].generation);
    return set;
}
static void layout(unsigned id, VkDescriptorType type, uint32_t count) {
    wants[id][type] = count;
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = type, .descriptorCount = count};
    vk_dsl_register((VkDescriptorSetLayout)(uintptr_t)id, &b, 1);
}
static void recycle(void) { complete(++slot.fence); slot_recycle(&slot); }
static void *blocked_recycle(void *unused) { (void)unused; slot_recycle(&slot); atomic_store(&finished, 1); return NULL; }
int main(void) {
    layout(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2);
    layout(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2);
    layout(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32);
    layout(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
    vk_dsl_register((VkDescriptorSetLayout)(uintptr_t)5, NULL, 0);
    assert(!unsetenv("DM_VK_SET_REUSE")); recycle(); assert(slot.reuse.enabled);
    VkDescriptorSet a[5], b[3];
    for (unsigned i = 0; i < 4; i++) { a[i] = take(1); for (unsigned j = 0; j < i; j++) assert(a[i] != a[j]); }
    for (unsigned i = 0; i < 3; i++) b[i] = take(2);
    unsigned before = set_count, resets = reset_count; recycle();
    for (unsigned i = 0; i < 4; i++) assert(take(1) == a[i]);
    for (unsigned i = 0; i < 3; i++) assert(take(2) == b[i]);
    assert(set_count == before && reset_count == resets); a[4] = take(1); assert(set_count == before + 1);
    // Neither of the two supported wait paths may rewind a busy slot early.
    scratch_blk descriptor_block = {.used = 8192}; slot.dbblk = &descriptor_block; slot.ndbblk = 1;
    for (int mode = 0; mode < 2; mode++) {
        descriptor_block.used = 8192;
        g_prereset = mode; slot.fence++; atomic_store(&entered, 0); atomic_store(&finished, 0);
        pthread_t worker; assert(!pthread_create(&worker, NULL, blocked_recycle, NULL));
        while (!atomic_load(&entered)) sched_yield();
        assert(!atomic_load(&finished) && descriptor_block.used == 8192);
        complete(slot.fence); assert(!pthread_join(worker, NULL)); assert(descriptor_block.used == 0);
        for (unsigned i = 0; i < 5; i++) assert(take(1) == a[i]);
    }
    slot.dbblk = NULL; slot.ndbblk = 0;
    // Unknown metadata falls back until the next safe recycle, never mid-epoch.
    take(11); assert(slot.reuse.discard); resets = reset_count; take(1); assert(reset_count == resets);
    recycle(); assert(reset_count > resets && !slot.reuse.discard); assert(take(1) != a[0]);
    // A mode change also takes effect only when the slot can be safely reused.
    assert(!setenv("DM_VK_SET_REUSE", "0", 1)); assert(slot.reuse.enabled); recycle(); assert(!slot.reuse.enabled);
    before = set_count; take(1); recycle(); take(1); assert(set_count == before + 2);
    assert(!setenv("DM_VK_SET_REUSE", "1", 1)); recycle();
    // Three descriptor pools are needed by type count before the set-count cap.
    resets = reset_count; for (unsigned i = 0; i < DESCRIPTOR_REUSE_LIMIT + 1; i++) take(3);
    assert(slot.reuse.discard && slot.reuse.count == DESCRIPTOR_REUSE_LIMIT && reset_count == resets);
    assert(slot.ndp >= 3); recycle(); assert(reset_count > resets && !slot.reuse.count);
    fail_entry = 1; take(4); assert(!fail_entry && slot.reuse.discard); recycle();
    fail_array = 1; take(4); assert(!fail_array && slot.reuse.discard); recycle();
    VkDescriptorSet empty = take(5); recycle(); assert(take(5) == empty);
    uint64_t allocated, reused, pool_resets, discards;
    vk_descriptor_reuse_stats(&allocated, &reused, &pool_resets, &discards);
    assert(allocated == set_count && reused >= 17 && pool_resets == reset_count && discards >= 4);
    assert(!setenv("DM_VK_SET_REUSE", "0", 1)); recycle();
    nvn_hm_clear(&slot.reuse.layout, descriptor_reuse_free); free(slot.reuse.layout.keys); free(slot.reuse.layout.vals); free(slot.dp);
    nvn_hm_clear(&g_dsl_need, free); free(g_dsl_need.keys); free(g_dsl_need.vals);
    if (atomic_load(&wrong_order)) { fputs("Recycling preceded the completed fence\n", stderr); return 77; }
    return 0;
}
