// Optional ordered Vulkan encoding worker. NVN state and resource decisions stay
// on the replay thread; only driver calls with owned argument snapshots move.
// DM_VK_ENCODE_THREAD=1 is experimental. Other threads keep their direct calls.
#include "gfx_vk.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VKE_FNS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) \
    X(vkCmdBindDescriptorBuffersEXT) X(vkCmdSetDescriptorBufferOffsetsEXT) X(vkUpdateDescriptorSets) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkResetDescriptorPool) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkWaitForFences) X(vkWaitSemaphores)
#define VKE_REAL(f) static PFN_##f real_##f;
VKE_FNS(VKE_REAL)

enum { VKE_BYTES = 32768, VKE_BLOCKS = 16, VKE_BATCH = 256 };
typedef struct { void (*execute)(const void *); uint32_t bytes; } vke_record;
_Static_assert(sizeof(vke_record) == 16, "encoding record alignment");
typedef struct vke_block {
    struct vke_block *next; unsigned used, calls;
    _Alignas(16) unsigned char data[VKE_BYTES];
} vke_block;
static vke_block g_vke_blocks[VKE_BLOCKS], *g_vke_free, *g_vke_head, *g_vke_tail;
static vke_block *g_vke_current; // producer only, inside the existing submission gate
static pthread_t g_vke_thread;
static pthread_mutex_t g_vke_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_vke_work = PTHREAD_COND_INITIALIZER, g_vke_idle = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned g_vke_ready, g_vke_outstanding;
static _Atomic int g_vke_started;
static int g_vke_stop;
static __thread int g_vke_owner;
static __thread int g_vke_placed;
void dm_perf_replay_thread(void); // existing opt-in DM_PIN_REPLAY policy
static struct { uint64_t calls, bytes, blocks, encode_ns, drain_ns, drains, backpressure, fallbacks; } g_vke_stats;

static _Atomic uint64_t g_vke_deferred_begins, g_vke_deferred_parts, g_vke_deferred_submits;
static _Atomic uint64_t g_vke_deferred_failed_ends, g_vke_deferred_skips, g_vke_deferred_submit_ns;
int vk_encode_deferred_parts(void) {
    if (!g_vke_owner) return 0;
    static _Atomic int cached;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (!value) {
        const char *e = getenv("DM_VK_DEFER_PARTS");
        value = e && atoi(e) > 0 ? 1 : 2;
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value == 1;
}

static uint64_t vke_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void *vke_worker(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "dm-vk-encode"); dm_perf_replay_thread();
    for (;;) {
        for (unsigned spin = 0; spin < 2048 && !atomic_load_explicit(&g_vke_ready, memory_order_acquire); ++spin) {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        pthread_mutex_lock(&g_vke_mu);
        while (!g_vke_head && !g_vke_stop) pthread_cond_wait(&g_vke_work, &g_vke_mu);
        if (!g_vke_head && g_vke_stop) { pthread_mutex_unlock(&g_vke_mu); return NULL; }
        vke_block *b = g_vke_head; g_vke_head = b->next; if (!g_vke_head) g_vke_tail = NULL;
        atomic_fetch_sub_explicit(&g_vke_ready, 1, memory_order_relaxed);
        pthread_mutex_unlock(&g_vke_mu);
        uint64_t begin = vke_now();
        for (unsigned at = 0; at < b->used;) {
            const vke_record *r = (const void *)(b->data + at);
            r->execute(r + 1); at += r->bytes;
        }
        uint64_t elapsed = vke_now() - begin;
        pthread_mutex_lock(&g_vke_mu);
        g_vke_stats.encode_ns += elapsed;
        b->next = g_vke_free; g_vke_free = b;
        atomic_fetch_sub_explicit(&g_vke_outstanding, 1, memory_order_release);
        pthread_cond_broadcast(&g_vke_idle);
        pthread_mutex_unlock(&g_vke_mu);
    }
}
static void vke_start(void) {
    if (g_vke_started) return;
    for (unsigned i = 0; i < VKE_BLOCKS; ++i) { g_vke_blocks[i].next = g_vke_free; g_vke_free = &g_vke_blocks[i]; }
    if (pthread_create(&g_vke_thread, NULL, vke_worker, NULL)) die("[RED] [vk] cannot start encoding worker");
    g_vke_started = 1;
    VKLOG("Vulkan encoding worker ready: %u bounded blocks, %u bytes each, %u calls per batch", VKE_BLOCKS, VKE_BYTES, VKE_BATCH);
}
static void vke_publish(void) {
    vke_block *b = g_vke_current; if (!b || !b->calls) return;
    g_vke_current = NULL; b->next = NULL;
    pthread_mutex_lock(&g_vke_mu);
    if (g_vke_tail) g_vke_tail->next = b; else g_vke_head = b; g_vke_tail = b;
    g_vke_stats.calls += b->calls; g_vke_stats.bytes += b->used; g_vke_stats.blocks++;
    atomic_fetch_add_explicit(&g_vke_outstanding, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_vke_ready, 1, memory_order_release);
    pthread_cond_signal(&g_vke_work); pthread_mutex_unlock(&g_vke_mu);
}
static vke_block *vke_take(void) {
    pthread_mutex_lock(&g_vke_mu);
    while (!g_vke_free) { g_vke_stats.backpressure++; pthread_cond_wait(&g_vke_idle, &g_vke_mu); }
    vke_block *b = g_vke_free; g_vke_free = b->next;
    pthread_mutex_unlock(&g_vke_mu);
    b->next = NULL; b->used = b->calls = 0; return b;
}
static size_t vke_align(size_t n) { return (n + 15) & ~(size_t)15; }
static int vke_bytes(size_t *size, size_t count, size_t stride) {
    if (!stride || count > (VKE_BYTES - sizeof(vke_record)) / stride) return 0;
    size_t n = vke_align(count * stride);
    if (*size > VKE_BYTES - sizeof(vke_record) || n > VKE_BYTES - sizeof(vke_record) - *size) return 0;
    *size += n; return 1;
}
static void *vke_alloc(size_t size, void (*execute)(const void *)) {
    size_t n = vke_align(sizeof(vke_record) + size);
    if (n > VKE_BYTES) die("[RED] [vk] oversized encoding packet");
    if (g_vke_current && (g_vke_current->calls == VKE_BATCH || n > VKE_BYTES - g_vke_current->used)) vke_publish();
    if (!g_vke_current) g_vke_current = vke_take();
    vke_record *r = (void *)(g_vke_current->data + g_vke_current->used);
    *r = (vke_record){execute, (uint32_t)n};
    g_vke_current->used += (unsigned)n; g_vke_current->calls++;
    return r + 1;
}
static void *vke_pack(unsigned char **at, const void *source, size_t count, size_t stride) {
    if (!source || !count) return NULL;
    void *copy = *at; size_t n = count * stride; memcpy(copy, source, n); *at += vke_align(n); return copy;
}
void vk_encode_drain(void) {
    if (!g_vke_owner || (!g_vke_current && !atomic_load_explicit(&g_vke_outstanding, memory_order_acquire))) return;
    uint64_t begin = vke_now(); vke_publish();
    pthread_mutex_lock(&g_vke_mu);
    while (atomic_load_explicit(&g_vke_outstanding, memory_order_acquire)) pthread_cond_wait(&g_vke_idle, &g_vke_mu);
    g_vke_stats.drains++; g_vke_stats.drain_ns += vke_now() - begin;
    pthread_mutex_unlock(&g_vke_mu);
}
static void vke_fallback(void) {
    vk_encode_drain(); pthread_mutex_lock(&g_vke_mu); g_vke_stats.fallbacks++; pthread_mutex_unlock(&g_vke_mu);
}
void vk_encode_begin(void) {
    if (g_vke_owner) die("[RED] [vk] encoding scope re-entered");
    const char *e = getenv("DM_VK_ENCODE_THREAD");
    if (!e || atoi(e) <= 0) return;
    // Both encoding participants use the same opt-in, hardware-derived policy.
    // Without DM_PIN_REPLAY the existing helper leaves placement unchanged.
    if (!g_vke_placed) { dm_perf_replay_thread(); g_vke_placed = 1; }
    vke_start(); g_vke_owner = 1;
}
void vk_encode_end(void) { vk_encode_drain(); g_vke_owner = 0; }
void vk_encode_report(void) {
    if (!g_vke_started) return;
    uint64_t parts = atomic_load_explicit(&g_vke_deferred_parts, memory_order_relaxed);
    if (parts) VKLOG("deferred split worker (cumulative): %llu begins, %llu parts, %llu submits, %llu failed ends, %llu skipped parts, %.2f ms submit wall",
        (unsigned long long)atomic_load_explicit(&g_vke_deferred_begins, memory_order_relaxed), (unsigned long long)parts,
        (unsigned long long)atomic_load_explicit(&g_vke_deferred_submits, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_vke_deferred_failed_ends, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_vke_deferred_skips, memory_order_relaxed),
        atomic_load_explicit(&g_vke_deferred_submit_ns, memory_order_relaxed) / 1e6);
    pthread_mutex_lock(&g_vke_mu);
    VKLOG("Vulkan encoding worker (cumulative): %llu calls, %.2f MB queued, %llu blocks, %.2f ms worker wall, %.2f ms drain wait, %llu drains, %llu backpressure waits, %llu synchronous fallbacks",
        (unsigned long long)g_vke_stats.calls, g_vke_stats.bytes / 1048576.0, (unsigned long long)g_vke_stats.blocks,
        g_vke_stats.encode_ns / 1e6, g_vke_stats.drain_ns / 1e6, (unsigned long long)g_vke_stats.drains,
        (unsigned long long)g_vke_stats.backpressure, (unsigned long long)g_vke_stats.fallbacks);
    pthread_mutex_unlock(&g_vke_mu);
}

// Pointer-bearing calls below either own their complete parameter graph or
// drain and execute synchronously. Unknown extension chains are never dropped.

typedef struct { VkCommandBuffer cb; VkSubpassContents contents; VkRenderPassBeginInfo info; VkRenderPassAttachmentBeginInfo attachments; } vke_renderpass;
static void vke_run_renderpass(const void *data) {
    const vke_renderpass *a = data; real_vkCmdBeginRenderPass(a->cb, &a->info, a->contents);
}
static void VKAPI_CALL vke_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    if (!g_vke_owner) { real_vkCmdBeginRenderPass(cb, info, contents); return; }
    size_t bytes = vke_align(sizeof(vke_renderpass));
    const VkRenderPassAttachmentBeginInfo *attachments = info ? info->pNext : NULL;
    if (!info || (info->clearValueCount && !info->pClearValues) ||
        !vke_bytes(&bytes, info->clearValueCount, sizeof(VkClearValue)) ||
        (attachments && (attachments->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO || attachments->pNext ||
            (attachments->attachmentCount && !attachments->pAttachments) || !vke_bytes(&bytes, attachments->attachmentCount, sizeof(VkImageView))))) {
        vke_fallback(); real_vkCmdBeginRenderPass(cb, info, contents); return;
    }
    vke_renderpass *a = vke_alloc(bytes, vke_run_renderpass);
    a->cb = cb; a->contents = contents; a->info = *info;
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->info.pClearValues = vke_pack(&tail, info->pClearValues, info->clearValueCount, sizeof(VkClearValue));
    if (attachments) { a->attachments = *attachments; a->attachments.pAttachments = vke_pack(&tail, attachments->pAttachments, attachments->attachmentCount, sizeof(VkImageView)); a->info.pNext = &a->attachments; }
}

typedef struct { VkCommandBuffer cb; VkRenderingInfo info; VkRenderingAttachmentInfo depth, stencil; } vke_rendering;
static void vke_run_rendering(const void *data) { const vke_rendering *a = data; real_vkCmdBeginRendering(a->cb, &a->info); }
static void VKAPI_CALL vke_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info) {
    if (!g_vke_owner) { real_vkCmdBeginRendering(cb, info); return; }
    size_t bytes = vke_align(sizeof(vke_rendering)); int bad = !info;
    if (info) {
        bad |= info->pNext != NULL || (info->colorAttachmentCount && !info->pColorAttachments);
        bad |= !vke_bytes(&bytes, info->colorAttachmentCount, sizeof(VkRenderingAttachmentInfo));
        if (!bad) for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) bad |= info->pColorAttachments[i].pNext != NULL;
        bad |= info->pDepthAttachment && info->pDepthAttachment->pNext;
        bad |= info->pStencilAttachment && info->pStencilAttachment->pNext;
    }
    if (bad) { vke_fallback(); real_vkCmdBeginRendering(cb, info); return; }
    vke_rendering *a = vke_alloc(bytes, vke_run_rendering); a->cb = cb; a->info = *info;
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->info.pColorAttachments = vke_pack(&tail, info->pColorAttachments, info->colorAttachmentCount, sizeof(VkRenderingAttachmentInfo));
    if (info->pDepthAttachment) { a->depth = *info->pDepthAttachment; a->info.pDepthAttachment = &a->depth; }
    if (info->pStencilAttachment) { a->stencil = *info->pStencilAttachment; a->info.pStencilAttachment = &a->stencil; }
}

typedef struct { VkDevice device; uint32_t nw, nc; const VkWriteDescriptorSet *writes; const VkCopyDescriptorSet *copies; } vke_descriptors;
static unsigned vke_descriptor_data(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 1;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return 2;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 3;
    default: return 0;
    }
}
static void vke_run_descriptors(const void *data) {
    const vke_descriptors *a = data; real_vkUpdateDescriptorSets(a->device, a->nw, a->writes, a->nc, a->copies);
}
static void VKAPI_CALL vke_vkUpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *writes, uint32_t nc, const VkCopyDescriptorSet *copies) {
    if (!g_vke_owner) { real_vkUpdateDescriptorSets(device, nw, writes, nc, copies); return; }
    size_t bytes = vke_align(sizeof(vke_descriptors));
    int bad = (nw && !writes) || (nc && !copies) || !vke_bytes(&bytes, nw, sizeof *writes) || !vke_bytes(&bytes, nc, sizeof *copies);
    if (!bad) for (uint32_t i = 0; i < nw; ++i) {
        const VkWriteDescriptorSet *w = &writes[i]; unsigned kind = vke_descriptor_data(w->descriptorType);
        if (w->pNext || !kind) { bad = 1; break; }
        const void *p = kind == 1 ? (const void *)w->pImageInfo : kind == 2 ? (const void *)w->pBufferInfo : (const void *)w->pTexelBufferView;
        size_t stride = kind == 1 ? sizeof(VkDescriptorImageInfo) : kind == 2 ? sizeof(VkDescriptorBufferInfo) : sizeof(VkBufferView);
        if ((w->descriptorCount && !p) || !vke_bytes(&bytes, w->descriptorCount, stride)) { bad = 1; break; }
    }
    if (!bad) for (uint32_t i = 0; i < nc; ++i) if (copies[i].pNext) { bad = 1; break; }
    if (bad) { vke_fallback(); real_vkUpdateDescriptorSets(device, nw, writes, nc, copies); return; }
    vke_descriptors *a = vke_alloc(bytes, vke_run_descriptors); a->device = device; a->nw = nw; a->nc = nc;
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    VkWriteDescriptorSet *out = vke_pack(&tail, writes, nw, sizeof *writes); a->writes = out;
    a->copies = vke_pack(&tail, copies, nc, sizeof *copies);
    for (uint32_t i = 0; i < nw; ++i) {
        VkWriteDescriptorSet *w = &out[i]; unsigned kind = vke_descriptor_data(w->descriptorType);
        w->pImageInfo = NULL; w->pBufferInfo = NULL; w->pTexelBufferView = NULL;
        if (kind == 1) w->pImageInfo = vke_pack(&tail, writes[i].pImageInfo, w->descriptorCount, sizeof(VkDescriptorImageInfo));
        else if (kind == 2) w->pBufferInfo = vke_pack(&tail, writes[i].pBufferInfo, w->descriptorCount, sizeof(VkDescriptorBufferInfo));
        else w->pTexelBufferView = vke_pack(&tail, writes[i].pTexelBufferView, w->descriptorCount, sizeof(VkBufferView));
    }
}

// Fixed scalar/array packets and synchronous lifecycle gates follow.

typedef struct { VkCommandBuffer cb; VkBuffer dst; VkDeviceSize offset; VkDeviceSize size; uint32_t data; } vke_args_vkCmdFillBuffer;
static void vke_run_vkCmdFillBuffer(const void *data) { const vke_args_vkCmdFillBuffer *a = data; real_vkCmdFillBuffer(a->cb, a->dst, a->offset, a->size, a->data); }
static void VKAPI_CALL vke_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    if (!g_vke_owner) { real_vkCmdFillBuffer(cb, dst, offset, size, data); return; }
    vke_args_vkCmdFillBuffer *a = vke_alloc(sizeof *a, vke_run_vkCmdFillBuffer); *a = (vke_args_vkCmdFillBuffer){cb, dst, offset, size, data};
}

typedef struct { VkCommandBuffer cb; } vke_args_vkCmdEndRenderPass;
static void vke_run_vkCmdEndRenderPass(const void *data) { const vke_args_vkCmdEndRenderPass *a = data; real_vkCmdEndRenderPass(a->cb); }
static void VKAPI_CALL vke_vkCmdEndRenderPass(VkCommandBuffer cb) {
    if (!g_vke_owner) { real_vkCmdEndRenderPass(cb); return; }
    vke_args_vkCmdEndRenderPass *a = vke_alloc(sizeof *a, vke_run_vkCmdEndRenderPass); *a = (vke_args_vkCmdEndRenderPass){cb};
}

typedef struct { VkCommandBuffer cb; } vke_args_vkCmdEndRendering;
static void vke_run_vkCmdEndRendering(const void *data) { const vke_args_vkCmdEndRendering *a = data; real_vkCmdEndRendering(a->cb); }
static void VKAPI_CALL vke_vkCmdEndRendering(VkCommandBuffer cb) {
    if (!g_vke_owner) { real_vkCmdEndRendering(cb); return; }
    vke_args_vkCmdEndRendering *a = vke_alloc(sizeof *a, vke_run_vkCmdEndRendering); *a = (vke_args_vkCmdEndRendering){cb};
}

typedef struct { VkCommandBuffer cb; VkPipelineBindPoint point; VkPipeline pipeline; } vke_args_vkCmdBindPipeline;
static void vke_run_vkCmdBindPipeline(const void *data) { const vke_args_vkCmdBindPipeline *a = data; real_vkCmdBindPipeline(a->cb, a->point, a->pipeline); }
static void VKAPI_CALL vke_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (!g_vke_owner) { real_vkCmdBindPipeline(cb, point, pipeline); return; }
    vke_args_vkCmdBindPipeline *a = vke_alloc(sizeof *a, vke_run_vkCmdBindPipeline); *a = (vke_args_vkCmdBindPipeline){cb, point, pipeline};
}

typedef struct { VkCommandBuffer cb; VkBuffer buffer; VkDeviceSize offset; VkIndexType type; } vke_args_vkCmdBindIndexBuffer;
static void vke_run_vkCmdBindIndexBuffer(const void *data) { const vke_args_vkCmdBindIndexBuffer *a = data; real_vkCmdBindIndexBuffer(a->cb, a->buffer, a->offset, a->type); }
static void VKAPI_CALL vke_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    if (!g_vke_owner) { real_vkCmdBindIndexBuffer(cb, buffer, offset, type); return; }
    vke_args_vkCmdBindIndexBuffer *a = vke_alloc(sizeof *a, vke_run_vkCmdBindIndexBuffer); *a = (vke_args_vkCmdBindIndexBuffer){cb, buffer, offset, type};
}

typedef struct { VkCommandBuffer cb; VkStencilFaceFlags faces; uint32_t reference; } vke_args_vkCmdSetStencilReference;
static void vke_run_vkCmdSetStencilReference(const void *data) { const vke_args_vkCmdSetStencilReference *a = data; real_vkCmdSetStencilReference(a->cb, a->faces, a->reference); }
static void VKAPI_CALL vke_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags faces, uint32_t reference) {
    if (!g_vke_owner) { real_vkCmdSetStencilReference(cb, faces, reference); return; }
    vke_args_vkCmdSetStencilReference *a = vke_alloc(sizeof *a, vke_run_vkCmdSetStencilReference); *a = (vke_args_vkCmdSetStencilReference){cb, faces, reference};
}

typedef struct { VkCommandBuffer cb; VkStencilFaceFlags faces; uint32_t mask; } vke_args_vkCmdSetStencilCompareMask;
static void vke_run_vkCmdSetStencilCompareMask(const void *data) { const vke_args_vkCmdSetStencilCompareMask *a = data; real_vkCmdSetStencilCompareMask(a->cb, a->faces, a->mask); }
static void VKAPI_CALL vke_vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags faces, uint32_t mask) {
    if (!g_vke_owner) { real_vkCmdSetStencilCompareMask(cb, faces, mask); return; }
    vke_args_vkCmdSetStencilCompareMask *a = vke_alloc(sizeof *a, vke_run_vkCmdSetStencilCompareMask); *a = (vke_args_vkCmdSetStencilCompareMask){cb, faces, mask};
}

typedef struct { VkCommandBuffer cb; VkStencilFaceFlags faces; uint32_t mask; } vke_args_vkCmdSetStencilWriteMask;
static void vke_run_vkCmdSetStencilWriteMask(const void *data) { const vke_args_vkCmdSetStencilWriteMask *a = data; real_vkCmdSetStencilWriteMask(a->cb, a->faces, a->mask); }
static void VKAPI_CALL vke_vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags faces, uint32_t mask) {
    if (!g_vke_owner) { real_vkCmdSetStencilWriteMask(cb, faces, mask); return; }
    vke_args_vkCmdSetStencilWriteMask *a = vke_alloc(sizeof *a, vke_run_vkCmdSetStencilWriteMask); *a = (vke_args_vkCmdSetStencilWriteMask){cb, faces, mask};
}

typedef struct { VkCommandBuffer cb; float constantFactor; float clamp; float slopeFactor; } vke_args_vkCmdSetDepthBias;
static void vke_run_vkCmdSetDepthBias(const void *data) { const vke_args_vkCmdSetDepthBias *a = data; real_vkCmdSetDepthBias(a->cb, a->constantFactor, a->clamp, a->slopeFactor); }
static void VKAPI_CALL vke_vkCmdSetDepthBias(VkCommandBuffer cb, float constantFactor, float clamp, float slopeFactor) {
    if (!g_vke_owner) { real_vkCmdSetDepthBias(cb, constantFactor, clamp, slopeFactor); return; }
    vke_args_vkCmdSetDepthBias *a = vke_alloc(sizeof *a, vke_run_vkCmdSetDepthBias); *a = (vke_args_vkCmdSetDepthBias){cb, constantFactor, clamp, slopeFactor};
}

typedef struct { VkCommandBuffer cb; float width; } vke_args_vkCmdSetLineWidth;
static void vke_run_vkCmdSetLineWidth(const void *data) { const vke_args_vkCmdSetLineWidth *a = data; real_vkCmdSetLineWidth(a->cb, a->width); }
static void VKAPI_CALL vke_vkCmdSetLineWidth(VkCommandBuffer cb, float width) {
    if (!g_vke_owner) { real_vkCmdSetLineWidth(cb, width); return; }
    vke_args_vkCmdSetLineWidth *a = vke_alloc(sizeof *a, vke_run_vkCmdSetLineWidth); *a = (vke_args_vkCmdSetLineWidth){cb, width};
}

typedef struct { VkCommandBuffer cb; uint32_t vertices; uint32_t instances; uint32_t firstVertex; uint32_t firstInstance; } vke_args_vkCmdDraw;
static void vke_run_vkCmdDraw(const void *data) { const vke_args_vkCmdDraw *a = data; real_vkCmdDraw(a->cb, a->vertices, a->instances, a->firstVertex, a->firstInstance); }
static void VKAPI_CALL vke_vkCmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    if (!g_vke_owner) { real_vkCmdDraw(cb, vertices, instances, firstVertex, firstInstance); return; }
    vke_args_vkCmdDraw *a = vke_alloc(sizeof *a, vke_run_vkCmdDraw); *a = (vke_args_vkCmdDraw){cb, vertices, instances, firstVertex, firstInstance};
}

typedef struct { VkCommandBuffer cb; uint32_t indices; uint32_t instances; uint32_t firstIndex; int32_t vertexOffset; uint32_t firstInstance; } vke_args_vkCmdDrawIndexed;
static void vke_run_vkCmdDrawIndexed(const void *data) { const vke_args_vkCmdDrawIndexed *a = data; real_vkCmdDrawIndexed(a->cb, a->indices, a->instances, a->firstIndex, a->vertexOffset, a->firstInstance); }
static void VKAPI_CALL vke_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    if (!g_vke_owner) { real_vkCmdDrawIndexed(cb, indices, instances, firstIndex, vertexOffset, firstInstance); return; }
    vke_args_vkCmdDrawIndexed *a = vke_alloc(sizeof *a, vke_run_vkCmdDrawIndexed); *a = (vke_args_vkCmdDrawIndexed){cb, indices, instances, firstIndex, vertexOffset, firstInstance};
}

typedef struct { VkCommandBuffer cb; VkBuffer buffer; VkDeviceSize offset; uint32_t draws; uint32_t stride; } vke_args_vkCmdDrawIndirect;
static void vke_run_vkCmdDrawIndirect(const void *data) { const vke_args_vkCmdDrawIndirect *a = data; real_vkCmdDrawIndirect(a->cb, a->buffer, a->offset, a->draws, a->stride); }
static void VKAPI_CALL vke_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride) {
    if (!g_vke_owner) { real_vkCmdDrawIndirect(cb, buffer, offset, draws, stride); return; }
    vke_args_vkCmdDrawIndirect *a = vke_alloc(sizeof *a, vke_run_vkCmdDrawIndirect); *a = (vke_args_vkCmdDrawIndirect){cb, buffer, offset, draws, stride};
}

typedef struct { VkCommandBuffer cb; VkBuffer buffer; VkDeviceSize offset; uint32_t draws; uint32_t stride; } vke_args_vkCmdDrawIndexedIndirect;
static void vke_run_vkCmdDrawIndexedIndirect(const void *data) { const vke_args_vkCmdDrawIndexedIndirect *a = data; real_vkCmdDrawIndexedIndirect(a->cb, a->buffer, a->offset, a->draws, a->stride); }
static void VKAPI_CALL vke_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride) {
    if (!g_vke_owner) { real_vkCmdDrawIndexedIndirect(cb, buffer, offset, draws, stride); return; }
    vke_args_vkCmdDrawIndexedIndirect *a = vke_alloc(sizeof *a, vke_run_vkCmdDrawIndexedIndirect); *a = (vke_args_vkCmdDrawIndexedIndirect){cb, buffer, offset, draws, stride};
}

typedef struct { VkCommandBuffer cb; uint32_t x; uint32_t y; uint32_t z; } vke_args_vkCmdDispatch;
static void vke_run_vkCmdDispatch(const void *data) { const vke_args_vkCmdDispatch *a = data; real_vkCmdDispatch(a->cb, a->x, a->y, a->z); }
static void VKAPI_CALL vke_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!g_vke_owner) { real_vkCmdDispatch(cb, x, y, z); return; }
    vke_args_vkCmdDispatch *a = vke_alloc(sizeof *a, vke_run_vkCmdDispatch); *a = (vke_args_vkCmdDispatch){cb, x, y, z};
}

typedef struct { VkCommandBuffer cb; VkBuffer buffer; VkDeviceSize offset; } vke_args_vkCmdDispatchIndirect;
static void vke_run_vkCmdDispatchIndirect(const void *data) { const vke_args_vkCmdDispatchIndirect *a = data; real_vkCmdDispatchIndirect(a->cb, a->buffer, a->offset); }
static void VKAPI_CALL vke_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset) {
    if (!g_vke_owner) { real_vkCmdDispatchIndirect(cb, buffer, offset); return; }
    vke_args_vkCmdDispatchIndirect *a = vke_alloc(sizeof *a, vke_run_vkCmdDispatchIndirect); *a = (vke_args_vkCmdDispatchIndirect){cb, buffer, offset};
}

typedef struct { VkCommandBuffer cb; VkQueryPool pool; uint32_t first; uint32_t count; } vke_args_vkCmdResetQueryPool;
static void vke_run_vkCmdResetQueryPool(const void *data) { const vke_args_vkCmdResetQueryPool *a = data; real_vkCmdResetQueryPool(a->cb, a->pool, a->first, a->count); }
static void VKAPI_CALL vke_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    if (!g_vke_owner) { real_vkCmdResetQueryPool(cb, pool, first, count); return; }
    vke_args_vkCmdResetQueryPool *a = vke_alloc(sizeof *a, vke_run_vkCmdResetQueryPool); *a = (vke_args_vkCmdResetQueryPool){cb, pool, first, count};
}

typedef struct { VkCommandBuffer cb; VkPipelineStageFlagBits stage; VkQueryPool pool; uint32_t query; } vke_args_vkCmdWriteTimestamp;
static void vke_run_vkCmdWriteTimestamp(const void *data) { const vke_args_vkCmdWriteTimestamp *a = data; real_vkCmdWriteTimestamp(a->cb, a->stage, a->pool, a->query); }
static void VKAPI_CALL vke_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    if (!g_vke_owner) { real_vkCmdWriteTimestamp(cb, stage, pool, query); return; }
    vke_args_vkCmdWriteTimestamp *a = vke_alloc(sizeof *a, vke_run_vkCmdWriteTimestamp); *a = (vke_args_vkCmdWriteTimestamp){cb, stage, pool, query};
}

typedef struct { VkCommandBuffer cb; VkBuffer src; VkBuffer dst; uint32_t count; const VkBufferCopy * regions; } vke_args_vkCmdCopyBuffer;
static void vke_run_vkCmdCopyBuffer(const void *data) { const vke_args_vkCmdCopyBuffer *a = data; real_vkCmdCopyBuffer(a->cb, a->src, a->dst, a->count, a->regions); }
static void VKAPI_CALL vke_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy * regions) {
    if (!g_vke_owner) { real_vkCmdCopyBuffer(cb, src, dst, count, regions); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdCopyBuffer)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkBufferCopy)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdCopyBuffer(cb, src, dst, count, regions); return; }
    vke_args_vkCmdCopyBuffer *a = vke_alloc(bytes, vke_run_vkCmdCopyBuffer); *a = (vke_args_vkCmdCopyBuffer){cb, src, dst, count, regions};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkBufferCopy));
}

typedef struct { VkCommandBuffer cb; VkBuffer src; VkImage dst; VkImageLayout layout; uint32_t count; const VkBufferImageCopy * regions; } vke_args_vkCmdCopyBufferToImage;
static void vke_run_vkCmdCopyBufferToImage(const void *data) { const vke_args_vkCmdCopyBufferToImage *a = data; real_vkCmdCopyBufferToImage(a->cb, a->src, a->dst, a->layout, a->count, a->regions); }
static void VKAPI_CALL vke_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count, const VkBufferImageCopy * regions) {
    if (!g_vke_owner) { real_vkCmdCopyBufferToImage(cb, src, dst, layout, count, regions); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdCopyBufferToImage)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkBufferImageCopy)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdCopyBufferToImage(cb, src, dst, layout, count, regions); return; }
    vke_args_vkCmdCopyBufferToImage *a = vke_alloc(bytes, vke_run_vkCmdCopyBufferToImage); *a = (vke_args_vkCmdCopyBufferToImage){cb, src, dst, layout, count, regions};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkBufferImageCopy));
}

typedef struct { VkCommandBuffer cb; VkImage src; VkImageLayout layout; VkBuffer dst; uint32_t count; const VkBufferImageCopy * regions; } vke_args_vkCmdCopyImageToBuffer;
static void vke_run_vkCmdCopyImageToBuffer(const void *data) { const vke_args_vkCmdCopyImageToBuffer *a = data; real_vkCmdCopyImageToBuffer(a->cb, a->src, a->layout, a->dst, a->count, a->regions); }
static void VKAPI_CALL vke_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t count, const VkBufferImageCopy * regions) {
    if (!g_vke_owner) { real_vkCmdCopyImageToBuffer(cb, src, layout, dst, count, regions); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdCopyImageToBuffer)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkBufferImageCopy)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdCopyImageToBuffer(cb, src, layout, dst, count, regions); return; }
    vke_args_vkCmdCopyImageToBuffer *a = vke_alloc(bytes, vke_run_vkCmdCopyImageToBuffer); *a = (vke_args_vkCmdCopyImageToBuffer){cb, src, layout, dst, count, regions};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkBufferImageCopy));
}

typedef struct { VkCommandBuffer cb; VkImage src; VkImageLayout srcLayout; VkImage dst; VkImageLayout dstLayout; uint32_t count; const VkImageCopy * regions; } vke_args_vkCmdCopyImage;
static void vke_run_vkCmdCopyImage(const void *data) { const vke_args_vkCmdCopyImage *a = data; real_vkCmdCopyImage(a->cb, a->src, a->srcLayout, a->dst, a->dstLayout, a->count, a->regions); }
static void VKAPI_CALL vke_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageCopy * regions) {
    if (!g_vke_owner) { real_vkCmdCopyImage(cb, src, srcLayout, dst, dstLayout, count, regions); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdCopyImage)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkImageCopy)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdCopyImage(cb, src, srcLayout, dst, dstLayout, count, regions); return; }
    vke_args_vkCmdCopyImage *a = vke_alloc(bytes, vke_run_vkCmdCopyImage); *a = (vke_args_vkCmdCopyImage){cb, src, srcLayout, dst, dstLayout, count, regions};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkImageCopy));
}

typedef struct { VkCommandBuffer cb; VkImage src; VkImageLayout srcLayout; VkImage dst; VkImageLayout dstLayout; uint32_t count; const VkImageBlit * regions; VkFilter filter; } vke_args_vkCmdBlitImage;
static void vke_run_vkCmdBlitImage(const void *data) { const vke_args_vkCmdBlitImage *a = data; real_vkCmdBlitImage(a->cb, a->src, a->srcLayout, a->dst, a->dstLayout, a->count, a->regions, a->filter); }
static void VKAPI_CALL vke_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageBlit * regions, VkFilter filter) {
    if (!g_vke_owner) { real_vkCmdBlitImage(cb, src, srcLayout, dst, dstLayout, count, regions, filter); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBlitImage)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkImageBlit)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdBlitImage(cb, src, srcLayout, dst, dstLayout, count, regions, filter); return; }
    vke_args_vkCmdBlitImage *a = vke_alloc(bytes, vke_run_vkCmdBlitImage); *a = (vke_args_vkCmdBlitImage){cb, src, srcLayout, dst, dstLayout, count, regions, filter};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkImageBlit));
}

typedef struct { VkCommandBuffer cb; VkImage src; VkImageLayout srcLayout; VkImage dst; VkImageLayout dstLayout; uint32_t count; const VkImageResolve * regions; } vke_args_vkCmdResolveImage;
static void vke_run_vkCmdResolveImage(const void *data) { const vke_args_vkCmdResolveImage *a = data; real_vkCmdResolveImage(a->cb, a->src, a->srcLayout, a->dst, a->dstLayout, a->count, a->regions); }
static void VKAPI_CALL vke_vkCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageResolve * regions) {
    if (!g_vke_owner) { real_vkCmdResolveImage(cb, src, srcLayout, dst, dstLayout, count, regions); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdResolveImage)); int bad = 0;
    bad |= regions ? !vke_bytes(&bytes, count, sizeof(VkImageResolve)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdResolveImage(cb, src, srcLayout, dst, dstLayout, count, regions); return; }
    vke_args_vkCmdResolveImage *a = vke_alloc(bytes, vke_run_vkCmdResolveImage); *a = (vke_args_vkCmdResolveImage){cb, src, srcLayout, dst, dstLayout, count, regions};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->regions = vke_pack(&tail, regions, count, sizeof(VkImageResolve));
}

typedef struct { VkCommandBuffer cb; VkBuffer dst; VkDeviceSize offset; VkDeviceSize size; const void * data; } vke_args_vkCmdUpdateBuffer;
static void vke_run_vkCmdUpdateBuffer(const void *data) { const vke_args_vkCmdUpdateBuffer *a = data; real_vkCmdUpdateBuffer(a->cb, a->dst, a->offset, a->size, a->data); }
static void VKAPI_CALL vke_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void * data) {
    if (!g_vke_owner) { real_vkCmdUpdateBuffer(cb, dst, offset, size, data); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdUpdateBuffer)); int bad = 0;
    bad |= data ? !vke_bytes(&bytes, size, sizeof(uint8_t)) : (size) != 0;
    if (bad) { vke_fallback(); real_vkCmdUpdateBuffer(cb, dst, offset, size, data); return; }
    vke_args_vkCmdUpdateBuffer *a = vke_alloc(bytes, vke_run_vkCmdUpdateBuffer); *a = (vke_args_vkCmdUpdateBuffer){cb, dst, offset, size, data};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->data = vke_pack(&tail, data, size, sizeof(uint8_t));
}

typedef struct { VkCommandBuffer cb; VkImage image; VkImageLayout layout; const VkClearColorValue * color; uint32_t count; const VkImageSubresourceRange * ranges; } vke_args_vkCmdClearColorImage;
static void vke_run_vkCmdClearColorImage(const void *data) { const vke_args_vkCmdClearColorImage *a = data; real_vkCmdClearColorImage(a->cb, a->image, a->layout, a->color, a->count, a->ranges); }
static void VKAPI_CALL vke_vkCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearColorValue * color, uint32_t count, const VkImageSubresourceRange * ranges) {
    if (!g_vke_owner) { real_vkCmdClearColorImage(cb, image, layout, color, count, ranges); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdClearColorImage)); int bad = 0;
    bad |= color ? !vke_bytes(&bytes, 1, sizeof(VkClearColorValue)) : (1) != 0;
    bad |= ranges ? !vke_bytes(&bytes, count, sizeof(VkImageSubresourceRange)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdClearColorImage(cb, image, layout, color, count, ranges); return; }
    vke_args_vkCmdClearColorImage *a = vke_alloc(bytes, vke_run_vkCmdClearColorImage); *a = (vke_args_vkCmdClearColorImage){cb, image, layout, color, count, ranges};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->color = vke_pack(&tail, color, 1, sizeof(VkClearColorValue));
    a->ranges = vke_pack(&tail, ranges, count, sizeof(VkImageSubresourceRange));
}

typedef struct { VkCommandBuffer cb; VkImage image; VkImageLayout layout; const VkClearDepthStencilValue * value; uint32_t count; const VkImageSubresourceRange * ranges; } vke_args_vkCmdClearDepthStencilImage;
static void vke_run_vkCmdClearDepthStencilImage(const void *data) { const vke_args_vkCmdClearDepthStencilImage *a = data; real_vkCmdClearDepthStencilImage(a->cb, a->image, a->layout, a->value, a->count, a->ranges); }
static void VKAPI_CALL vke_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue * value, uint32_t count, const VkImageSubresourceRange * ranges) {
    if (!g_vke_owner) { real_vkCmdClearDepthStencilImage(cb, image, layout, value, count, ranges); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdClearDepthStencilImage)); int bad = 0;
    bad |= value ? !vke_bytes(&bytes, 1, sizeof(VkClearDepthStencilValue)) : (1) != 0;
    bad |= ranges ? !vke_bytes(&bytes, count, sizeof(VkImageSubresourceRange)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdClearDepthStencilImage(cb, image, layout, value, count, ranges); return; }
    vke_args_vkCmdClearDepthStencilImage *a = vke_alloc(bytes, vke_run_vkCmdClearDepthStencilImage); *a = (vke_args_vkCmdClearDepthStencilImage){cb, image, layout, value, count, ranges};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->value = vke_pack(&tail, value, 1, sizeof(VkClearDepthStencilValue));
    a->ranges = vke_pack(&tail, ranges, count, sizeof(VkImageSubresourceRange));
}

typedef struct { VkCommandBuffer cb; uint32_t count; const VkClearAttachment * attachments; uint32_t rectCount; const VkClearRect * rects; } vke_args_vkCmdClearAttachments;
static void vke_run_vkCmdClearAttachments(const void *data) { const vke_args_vkCmdClearAttachments *a = data; real_vkCmdClearAttachments(a->cb, a->count, a->attachments, a->rectCount, a->rects); }
static void VKAPI_CALL vke_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t count, const VkClearAttachment * attachments, uint32_t rectCount, const VkClearRect * rects) {
    if (!g_vke_owner) { real_vkCmdClearAttachments(cb, count, attachments, rectCount, rects); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdClearAttachments)); int bad = 0;
    bad |= attachments ? !vke_bytes(&bytes, count, sizeof(VkClearAttachment)) : (count) != 0;
    bad |= rects ? !vke_bytes(&bytes, rectCount, sizeof(VkClearRect)) : (rectCount) != 0;
    if (bad) { vke_fallback(); real_vkCmdClearAttachments(cb, count, attachments, rectCount, rects); return; }
    vke_args_vkCmdClearAttachments *a = vke_alloc(bytes, vke_run_vkCmdClearAttachments); *a = (vke_args_vkCmdClearAttachments){cb, count, attachments, rectCount, rects};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->attachments = vke_pack(&tail, attachments, count, sizeof(VkClearAttachment));
    a->rects = vke_pack(&tail, rects, rectCount, sizeof(VkClearRect));
}

typedef struct { VkCommandBuffer cb; VkPipelineBindPoint point; VkPipelineLayout layout; uint32_t first; uint32_t count; const VkDescriptorSet * sets; uint32_t dynamicCount; const uint32_t * offsets; } vke_args_vkCmdBindDescriptorSets;
static void vke_run_vkCmdBindDescriptorSets(const void *data) { const vke_args_vkCmdBindDescriptorSets *a = data; real_vkCmdBindDescriptorSets(a->cb, a->point, a->layout, a->first, a->count, a->sets, a->dynamicCount, a->offsets); }
static void VKAPI_CALL vke_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet * sets, uint32_t dynamicCount, const uint32_t * offsets) {
    if (!g_vke_owner) { real_vkCmdBindDescriptorSets(cb, point, layout, first, count, sets, dynamicCount, offsets); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBindDescriptorSets)); int bad = 0;
    bad |= sets ? !vke_bytes(&bytes, count, sizeof(VkDescriptorSet)) : (count) != 0;
    bad |= offsets ? !vke_bytes(&bytes, dynamicCount, sizeof(uint32_t)) : (dynamicCount) != 0;
    if (bad) { vke_fallback(); real_vkCmdBindDescriptorSets(cb, point, layout, first, count, sets, dynamicCount, offsets); return; }
    vke_args_vkCmdBindDescriptorSets *a = vke_alloc(bytes, vke_run_vkCmdBindDescriptorSets); *a = (vke_args_vkCmdBindDescriptorSets){cb, point, layout, first, count, sets, dynamicCount, offsets};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->sets = vke_pack(&tail, sets, count, sizeof(VkDescriptorSet));
    a->offsets = vke_pack(&tail, offsets, dynamicCount, sizeof(uint32_t));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkBuffer * buffers; const VkDeviceSize * offsets; } vke_args_vkCmdBindVertexBuffers;
static void vke_run_vkCmdBindVertexBuffers(const void *data) { const vke_args_vkCmdBindVertexBuffers *a = data; real_vkCmdBindVertexBuffers(a->cb, a->first, a->count, a->buffers, a->offsets); }
static void VKAPI_CALL vke_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer * buffers, const VkDeviceSize * offsets) {
    if (!g_vke_owner) { real_vkCmdBindVertexBuffers(cb, first, count, buffers, offsets); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBindVertexBuffers)); int bad = 0;
    bad |= buffers ? !vke_bytes(&bytes, count, sizeof(VkBuffer)) : (count) != 0;
    bad |= offsets ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdBindVertexBuffers(cb, first, count, buffers, offsets); return; }
    vke_args_vkCmdBindVertexBuffers *a = vke_alloc(bytes, vke_run_vkCmdBindVertexBuffers); *a = (vke_args_vkCmdBindVertexBuffers){cb, first, count, buffers, offsets};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->buffers = vke_pack(&tail, buffers, count, sizeof(VkBuffer));
    a->offsets = vke_pack(&tail, offsets, count, sizeof(VkDeviceSize));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkViewport * viewports; } vke_args_vkCmdSetViewport;
static void vke_run_vkCmdSetViewport(const void *data) { const vke_args_vkCmdSetViewport *a = data; real_vkCmdSetViewport(a->cb, a->first, a->count, a->viewports); }
static void VKAPI_CALL vke_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport * viewports) {
    if (!g_vke_owner) { real_vkCmdSetViewport(cb, first, count, viewports); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdSetViewport)); int bad = 0;
    bad |= viewports ? !vke_bytes(&bytes, count, sizeof(VkViewport)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdSetViewport(cb, first, count, viewports); return; }
    vke_args_vkCmdSetViewport *a = vke_alloc(bytes, vke_run_vkCmdSetViewport); *a = (vke_args_vkCmdSetViewport){cb, first, count, viewports};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->viewports = vke_pack(&tail, viewports, count, sizeof(VkViewport));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkRect2D * scissors; } vke_args_vkCmdSetScissor;
static void vke_run_vkCmdSetScissor(const void *data) { const vke_args_vkCmdSetScissor *a = data; real_vkCmdSetScissor(a->cb, a->first, a->count, a->scissors); }
static void VKAPI_CALL vke_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D * scissors) {
    if (!g_vke_owner) { real_vkCmdSetScissor(cb, first, count, scissors); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdSetScissor)); int bad = 0;
    bad |= scissors ? !vke_bytes(&bytes, count, sizeof(VkRect2D)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdSetScissor(cb, first, count, scissors); return; }
    vke_args_vkCmdSetScissor *a = vke_alloc(bytes, vke_run_vkCmdSetScissor); *a = (vke_args_vkCmdSetScissor){cb, first, count, scissors};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->scissors = vke_pack(&tail, scissors, count, sizeof(VkRect2D));
}

typedef struct { VkCommandBuffer cb; const float * constants; } vke_args_vkCmdSetBlendConstants;
static void vke_run_vkCmdSetBlendConstants(const void *data) { const vke_args_vkCmdSetBlendConstants *a = data; real_vkCmdSetBlendConstants(a->cb, a->constants); }
static void VKAPI_CALL vke_vkCmdSetBlendConstants(VkCommandBuffer cb, const float * constants) {
    if (!g_vke_owner) { real_vkCmdSetBlendConstants(cb, constants); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdSetBlendConstants)); int bad = 0;
    bad |= constants ? !vke_bytes(&bytes, 4, sizeof(float)) : (4) != 0;
    if (bad) { vke_fallback(); real_vkCmdSetBlendConstants(cb, constants); return; }
    vke_args_vkCmdSetBlendConstants *a = vke_alloc(bytes, vke_run_vkCmdSetBlendConstants); *a = (vke_args_vkCmdSetBlendConstants){cb, constants};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->constants = vke_pack(&tail, constants, 4, sizeof(float));
}

typedef struct { VkCommandBuffer cb; VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t offset; uint32_t size; const void * values; } vke_args_vkCmdPushConstants;
static void vke_run_vkCmdPushConstants(const void *data) { const vke_args_vkCmdPushConstants *a = data; real_vkCmdPushConstants(a->cb, a->layout, a->stages, a->offset, a->size, a->values); }
static void VKAPI_CALL vke_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void * values) {
    if (!g_vke_owner) { real_vkCmdPushConstants(cb, layout, stages, offset, size, values); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdPushConstants)); int bad = 0;
    bad |= values ? !vke_bytes(&bytes, size, sizeof(uint8_t)) : (size) != 0;
    if (bad) { vke_fallback(); real_vkCmdPushConstants(cb, layout, stages, offset, size, values); return; }
    vke_args_vkCmdPushConstants *a = vke_alloc(bytes, vke_run_vkCmdPushConstants); *a = (vke_args_vkCmdPushConstants){cb, layout, stages, offset, size, values};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->values = vke_pack(&tail, values, size, sizeof(uint8_t));
}

typedef struct { VkCommandBuffer cb; VkPipelineStageFlags src; VkPipelineStageFlags dst; VkDependencyFlags dependencies; uint32_t memoryCount; const VkMemoryBarrier * memory; uint32_t bufferCount; const VkBufferMemoryBarrier * buffers; uint32_t imageCount; const VkImageMemoryBarrier * images; } vke_args_vkCmdPipelineBarrier;
static void vke_run_vkCmdPipelineBarrier(const void *data) { const vke_args_vkCmdPipelineBarrier *a = data; real_vkCmdPipelineBarrier(a->cb, a->src, a->dst, a->dependencies, a->memoryCount, a->memory, a->bufferCount, a->buffers, a->imageCount, a->images); }
static void VKAPI_CALL vke_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dependencies, uint32_t memoryCount, const VkMemoryBarrier * memory, uint32_t bufferCount, const VkBufferMemoryBarrier * buffers, uint32_t imageCount, const VkImageMemoryBarrier * images) {
    if (!g_vke_owner) { real_vkCmdPipelineBarrier(cb, src, dst, dependencies, memoryCount, memory, bufferCount, buffers, imageCount, images); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdPipelineBarrier)); int bad = 0;
    bad |= memory ? !vke_bytes(&bytes, memoryCount, sizeof(VkMemoryBarrier)) : (memoryCount) != 0;
    bad |= buffers ? !vke_bytes(&bytes, bufferCount, sizeof(VkBufferMemoryBarrier)) : (bufferCount) != 0;
    bad |= images ? !vke_bytes(&bytes, imageCount, sizeof(VkImageMemoryBarrier)) : (imageCount) != 0;
    if (!bad && memory) for (uint32_t i = 0; i < memoryCount; ++i) bad |= memory[i].pNext != NULL;
    if (!bad && buffers) for (uint32_t i = 0; i < bufferCount; ++i) bad |= buffers[i].pNext != NULL;
    if (!bad && images) for (uint32_t i = 0; i < imageCount; ++i) bad |= images[i].pNext != NULL;
    if (bad) { vke_fallback(); real_vkCmdPipelineBarrier(cb, src, dst, dependencies, memoryCount, memory, bufferCount, buffers, imageCount, images); return; }
    vke_args_vkCmdPipelineBarrier *a = vke_alloc(bytes, vke_run_vkCmdPipelineBarrier); *a = (vke_args_vkCmdPipelineBarrier){cb, src, dst, dependencies, memoryCount, memory, bufferCount, buffers, imageCount, images};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->memory = vke_pack(&tail, memory, memoryCount, sizeof(VkMemoryBarrier));
    a->buffers = vke_pack(&tail, buffers, bufferCount, sizeof(VkBufferMemoryBarrier));
    a->images = vke_pack(&tail, images, imageCount, sizeof(VkImageMemoryBarrier));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkBuffer * buffers; const VkDeviceSize * offsets; const VkDeviceSize * sizes; } vke_args_vkCmdBindTransformFeedbackBuffersEXT;
static void vke_run_vkCmdBindTransformFeedbackBuffersEXT(const void *data) { const vke_args_vkCmdBindTransformFeedbackBuffersEXT *a = data; real_vkCmdBindTransformFeedbackBuffersEXT(a->cb, a->first, a->count, a->buffers, a->offsets, a->sizes); }
static void VKAPI_CALL vke_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer * buffers, const VkDeviceSize * offsets, const VkDeviceSize * sizes) {
    if (!g_vke_owner) { real_vkCmdBindTransformFeedbackBuffersEXT(cb, first, count, buffers, offsets, sizes); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBindTransformFeedbackBuffersEXT)); int bad = 0;
    bad |= buffers ? !vke_bytes(&bytes, count, sizeof(VkBuffer)) : (count) != 0;
    bad |= offsets ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : (count) != 0;
    bad |= sizes ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : 0;
    if (bad) { vke_fallback(); real_vkCmdBindTransformFeedbackBuffersEXT(cb, first, count, buffers, offsets, sizes); return; }
    vke_args_vkCmdBindTransformFeedbackBuffersEXT *a = vke_alloc(bytes, vke_run_vkCmdBindTransformFeedbackBuffersEXT); *a = (vke_args_vkCmdBindTransformFeedbackBuffersEXT){cb, first, count, buffers, offsets, sizes};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->buffers = vke_pack(&tail, buffers, count, sizeof(VkBuffer));
    a->offsets = vke_pack(&tail, offsets, count, sizeof(VkDeviceSize));
    a->sizes = vke_pack(&tail, sizes, count, sizeof(VkDeviceSize));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkBuffer * buffers; const VkDeviceSize * offsets; } vke_args_vkCmdBeginTransformFeedbackEXT;
static void vke_run_vkCmdBeginTransformFeedbackEXT(const void *data) { const vke_args_vkCmdBeginTransformFeedbackEXT *a = data; real_vkCmdBeginTransformFeedbackEXT(a->cb, a->first, a->count, a->buffers, a->offsets); }
static void VKAPI_CALL vke_vkCmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer * buffers, const VkDeviceSize * offsets) {
    if (!g_vke_owner) { real_vkCmdBeginTransformFeedbackEXT(cb, first, count, buffers, offsets); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBeginTransformFeedbackEXT)); int bad = 0;
    bad |= buffers ? !vke_bytes(&bytes, count, sizeof(VkBuffer)) : 0;
    bad |= offsets ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : 0;
    if (bad) { vke_fallback(); real_vkCmdBeginTransformFeedbackEXT(cb, first, count, buffers, offsets); return; }
    vke_args_vkCmdBeginTransformFeedbackEXT *a = vke_alloc(bytes, vke_run_vkCmdBeginTransformFeedbackEXT); *a = (vke_args_vkCmdBeginTransformFeedbackEXT){cb, first, count, buffers, offsets};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->buffers = vke_pack(&tail, buffers, count, sizeof(VkBuffer));
    a->offsets = vke_pack(&tail, offsets, count, sizeof(VkDeviceSize));
}

typedef struct { VkCommandBuffer cb; uint32_t first; uint32_t count; const VkBuffer * buffers; const VkDeviceSize * offsets; } vke_args_vkCmdEndTransformFeedbackEXT;
static void vke_run_vkCmdEndTransformFeedbackEXT(const void *data) { const vke_args_vkCmdEndTransformFeedbackEXT *a = data; real_vkCmdEndTransformFeedbackEXT(a->cb, a->first, a->count, a->buffers, a->offsets); }
static void VKAPI_CALL vke_vkCmdEndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer * buffers, const VkDeviceSize * offsets) {
    if (!g_vke_owner) { real_vkCmdEndTransformFeedbackEXT(cb, first, count, buffers, offsets); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdEndTransformFeedbackEXT)); int bad = 0;
    bad |= buffers ? !vke_bytes(&bytes, count, sizeof(VkBuffer)) : 0;
    bad |= offsets ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : 0;
    if (bad) { vke_fallback(); real_vkCmdEndTransformFeedbackEXT(cb, first, count, buffers, offsets); return; }
    vke_args_vkCmdEndTransformFeedbackEXT *a = vke_alloc(bytes, vke_run_vkCmdEndTransformFeedbackEXT); *a = (vke_args_vkCmdEndTransformFeedbackEXT){cb, first, count, buffers, offsets};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->buffers = vke_pack(&tail, buffers, count, sizeof(VkBuffer));
    a->offsets = vke_pack(&tail, offsets, count, sizeof(VkDeviceSize));
}

typedef struct { VkCommandBuffer cb; uint32_t count; const VkDescriptorBufferBindingInfoEXT * bindings; } vke_args_vkCmdBindDescriptorBuffersEXT;
static void vke_run_vkCmdBindDescriptorBuffersEXT(const void *data) { const vke_args_vkCmdBindDescriptorBuffersEXT *a = data; real_vkCmdBindDescriptorBuffersEXT(a->cb, a->count, a->bindings); }
static void VKAPI_CALL vke_vkCmdBindDescriptorBuffersEXT(VkCommandBuffer cb, uint32_t count, const VkDescriptorBufferBindingInfoEXT * bindings) {
    if (!g_vke_owner) { real_vkCmdBindDescriptorBuffersEXT(cb, count, bindings); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdBindDescriptorBuffersEXT)); int bad = 0;
    bad |= bindings ? !vke_bytes(&bytes, count, sizeof(VkDescriptorBufferBindingInfoEXT)) : (count) != 0;
    if (!bad && bindings) for (uint32_t i = 0; i < count; ++i) bad |= bindings[i].pNext != NULL;
    if (bad) { vke_fallback(); real_vkCmdBindDescriptorBuffersEXT(cb, count, bindings); return; }
    vke_args_vkCmdBindDescriptorBuffersEXT *a = vke_alloc(bytes, vke_run_vkCmdBindDescriptorBuffersEXT); *a = (vke_args_vkCmdBindDescriptorBuffersEXT){cb, count, bindings};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->bindings = vke_pack(&tail, bindings, count, sizeof(VkDescriptorBufferBindingInfoEXT));
}

typedef struct { VkCommandBuffer cb; VkPipelineBindPoint point; VkPipelineLayout layout; uint32_t first; uint32_t count; const uint32_t * indices; const VkDeviceSize * offsets; } vke_args_vkCmdSetDescriptorBufferOffsetsEXT;
static void vke_run_vkCmdSetDescriptorBufferOffsetsEXT(const void *data) { const vke_args_vkCmdSetDescriptorBufferOffsetsEXT *a = data; real_vkCmdSetDescriptorBufferOffsetsEXT(a->cb, a->point, a->layout, a->first, a->count, a->indices, a->offsets); }
static void VKAPI_CALL vke_vkCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const uint32_t * indices, const VkDeviceSize * offsets) {
    if (!g_vke_owner) { real_vkCmdSetDescriptorBufferOffsetsEXT(cb, point, layout, first, count, indices, offsets); return; }
    size_t bytes = vke_align(sizeof(vke_args_vkCmdSetDescriptorBufferOffsetsEXT)); int bad = 0;
    bad |= indices ? !vke_bytes(&bytes, count, sizeof(uint32_t)) : (count) != 0;
    bad |= offsets ? !vke_bytes(&bytes, count, sizeof(VkDeviceSize)) : (count) != 0;
    if (bad) { vke_fallback(); real_vkCmdSetDescriptorBufferOffsetsEXT(cb, point, layout, first, count, indices, offsets); return; }
    vke_args_vkCmdSetDescriptorBufferOffsetsEXT *a = vke_alloc(bytes, vke_run_vkCmdSetDescriptorBufferOffsetsEXT); *a = (vke_args_vkCmdSetDescriptorBufferOffsetsEXT){cb, point, layout, first, count, indices, offsets};
    unsigned char *tail = (unsigned char *)a + vke_align(sizeof *a);
    a->indices = vke_pack(&tail, indices, count, sizeof(uint32_t));
    a->offsets = vke_pack(&tail, offsets, count, sizeof(VkDeviceSize));
}

// Internal checked operations do not fabricate VkResult returns. The regular
// Vulkan wrappers below remain synchronous and unchanged.
typedef struct { VkCommandBuffer cb; VkCommandBufferBeginInfo info; } vke_checked_begin_args;
static void vke_run_checked_begin(const void *payload) {
    const vke_checked_begin_args *a = payload;
    VkResult result = real_vkBeginCommandBuffer(a->cb, &a->info);
    if (result < 0) die("[RED] [vk] deferred vkBeginCommandBuffer failed: %d", result);
    atomic_fetch_add_explicit(&g_vke_deferred_begins, 1, memory_order_relaxed);
}
void vk_encode_begin_checked(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    if (!vk_encode_deferred_parts() || !info || info->pNext || info->pInheritanceInfo) {
        vk_encode_drain();
        VkResult result = real_vkBeginCommandBuffer(cb, info);
        if (result < 0) die("[RED] [vk] vkBeginCommandBuffer failed: %d", result);
        return;
    }
    vke_checked_begin_args *a = vke_alloc(sizeof(*a), vke_run_checked_begin);
    *a = (vke_checked_begin_args){cb, *info};
}
typedef struct {
    VkQueue queue;
    VkCommandBuffer main, up;
    int main_count, stamp;
    vk_deferred_part_state *state; // persistent slot; producer reads after drain
} vke_deferred_part_args;
static void vke_run_deferred_part(const void *payload) {
    const vke_deferred_part_args *a = payload;
    vk_deferred_part_state *s = a->state;
    VkResult result = real_vkEndCommandBuffer(a->main);
    if (result < 0) {
        s->end_error = 1;
        atomic_fetch_add_explicit(&g_vke_deferred_failed_ends, 1, memory_order_relaxed);
        VKLOG("[RED] deferred main part %d refused at its end: %d", a->main_count, result);
    }
    if (a->up) {
        result = real_vkEndCommandBuffer(a->up);
        if (result < 0) {
            s->end_error = 1;
            atomic_fetch_add_explicit(&g_vke_deferred_failed_ends, 1, memory_order_relaxed);
            VKLOG("[RED] deferred upload part %d refused at its end: %d", a->main_count, result);
        }
    }
    atomic_fetch_add_explicit(&g_vke_deferred_parts, 1, memory_order_relaxed);
    if (s->end_error) {
        atomic_fetch_add_explicit(&g_vke_deferred_skips, 1, memory_order_relaxed);
        return;
    }
    VkCommandBuffer cbs[2]; uint32_t count = 0;
    if (a->up) cbs[count++] = a->up;
    cbs[count++] = a->main;
    VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, count, cbs, 0, NULL};
    extern pthread_mutex_t g_vk_queue_mu;
    uint64_t begin = vke_now();
    pthread_mutex_lock(&g_vk_queue_mu);
    if (a->stamp && !s->cpu_first) s->cpu_first = vke_now();
    result = real_vkQueueSubmit(a->queue, 1, &info, VK_NULL_HANDLE);
    pthread_mutex_unlock(&g_vk_queue_mu);
    atomic_fetch_add_explicit(&g_vke_deferred_submit_ns, vke_now() - begin, memory_order_relaxed);
    if (result) die("[RED] [vk] deferred vkQueueSubmit (part): %d (device lost?)", result);
    s->last_sent = a->main_count;
    atomic_fetch_add_explicit(&g_vke_deferred_submits, 1, memory_order_relaxed);
}
void vk_encode_defer_part(VkQueue queue, VkCommandBuffer main, VkCommandBuffer up,
                          int main_count, int stamp, vk_deferred_part_state *state) {
    if (!vk_encode_deferred_parts()) die("[RED] [vk] deferred part outside ordered worker scope");
    vke_deferred_part_args *a = vke_alloc(sizeof(*a), vke_run_deferred_part);
    *a = (vke_deferred_part_args){queue, main, up, main_count, stamp, state};
    // Start feeding the GPU promptly; the producer can prepare the next part.
    vke_publish();
}

static VkResult VKAPI_CALL vke_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo * info) {
    vk_encode_drain(); return real_vkBeginCommandBuffer(cb, info);
}
static VkResult VKAPI_CALL vke_vkEndCommandBuffer(VkCommandBuffer cb) {
    vk_encode_drain(); return real_vkEndCommandBuffer(cb);
}
static VkResult VKAPI_CALL vke_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    vk_encode_drain(); return real_vkResetCommandBuffer(cb, flags);
}
static VkResult VKAPI_CALL vke_vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    vk_encode_drain(); return real_vkResetCommandPool(device, pool, flags);
}
static VkResult VKAPI_CALL vke_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo * info, VkCommandBuffer * buffers) {
    vk_encode_drain(); return real_vkAllocateCommandBuffers(device, info, buffers);
}
static VkResult VKAPI_CALL vke_vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
    vk_encode_drain(); return real_vkResetDescriptorPool(device, pool, flags);
}
static VkResult VKAPI_CALL vke_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo * submissions, VkFence fence) {
    vk_encode_drain(); return real_vkQueueSubmit(queue, count, submissions, fence);
}
static VkResult VKAPI_CALL vke_vkQueueWaitIdle(VkQueue queue) {
    vk_encode_drain(); return real_vkQueueWaitIdle(queue);
}
static VkResult VKAPI_CALL vke_vkDeviceWaitIdle(VkDevice device) {
    vk_encode_drain(); return real_vkDeviceWaitIdle(device);
}
static VkResult VKAPI_CALL vke_vkWaitForFences(VkDevice device, uint32_t count, const VkFence * fences, VkBool32 all, uint64_t timeout) {
    vk_encode_drain(); return real_vkWaitForFences(device, count, fences, all, timeout);
}
static VkResult VKAPI_CALL vke_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo * info, uint64_t timeout) {
    vk_encode_drain(); return real_vkWaitSemaphores(device, info, timeout);
}

void vk_encode_init(void) {
    static int installed; if (installed) return; installed = 1;
#define VKE_INSTALL(f) if (f) { real_##f = f; f = vke_##f; }
    VKE_FNS(VKE_INSTALL)
#undef VKE_INSTALL
}
