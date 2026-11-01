// Map driver present IDs to game frame numbers. Callers serialize writers;
// completed timestamps may be read concurrently. Unknown
// feedback stays zero, rather than substituting enqueue time for display time.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

enum { VK_PRESENT_REQUESTS = 512, VK_PRESENT_FRAMES = 64 };
typedef struct {
    uint32_t next;
    struct { uint32_t id; uint64_t frame; } requests[VK_PRESENT_REQUESTS];
    struct { _Atomic uint64_t frame, actual; } frames[VK_PRESENT_FRAMES];
} vk_present_history;

static inline void vk_present_begin(vk_present_history *h, uint64_t frame) {
    if (frame) {
        unsigned k = (unsigned)(frame % VK_PRESENT_FRAMES);
        atomic_store_explicit(&h->frames[k].actual, 0, memory_order_relaxed);
        atomic_store_explicit(&h->frames[k].frame, frame, memory_order_release);
    }
}
static inline void vk_present_map(vk_present_history *h, uint32_t id, uint64_t frame) {
    unsigned k = id % VK_PRESENT_REQUESTS;
    h->requests[k].id = id; h->requests[k].frame = frame;
}
static inline void vk_present_clear(vk_present_history *h) {
    for (unsigned k = 0; k < VK_PRESENT_REQUESTS; ++k) {
        h->requests[k].id = 0; h->requests[k].frame = 0;
    }
    for (unsigned k = 0; k < VK_PRESENT_FRAMES; ++k) {
        atomic_store_explicit(&h->frames[k].frame, 0, memory_order_release);
        atomic_store_explicit(&h->frames[k].actual, 0, memory_order_release);
    }
    // Retain next: delayed feedback from an old swapchain cannot reuse its ID.
}
static inline uint32_t vk_present_issue(vk_present_history *h, uint64_t frame) {
    uint32_t id = ++h->next; if (!id) id = ++h->next;
    vk_present_begin(h, frame); vk_present_map(h, id, frame);
    return id;
}
static inline int vk_present_record(vk_present_history *h, uint32_t id, uint64_t actual) {
    unsigned k = id % VK_PRESENT_REQUESTS;
    if (!actual || actual == UINT64_MAX || h->requests[k].id != id) return 0;
    uint64_t frame = h->requests[k].frame;
    if (!frame) return 0;   // host-menu redraws do not count as new game frames
    k = (unsigned)(frame % VK_PRESENT_FRAMES);
    if (atomic_load_explicit(&h->frames[k].frame, memory_order_relaxed) != frame ||
        atomic_load_explicit(&h->frames[k].actual, memory_order_relaxed)) return 0;
    atomic_store_explicit(&h->frames[k].actual, actual, memory_order_release);
    return 1;
}
static inline void vk_present_discard(vk_present_history *h, uint64_t frame) {
    unsigned k = (unsigned)(frame % VK_PRESENT_FRAMES);
    if (frame && atomic_load_explicit(&h->frames[k].frame, memory_order_relaxed) == frame)
        atomic_store_explicit(&h->frames[k].actual, UINT64_MAX, memory_order_release);
}
static inline uint64_t vk_present_actual(const vk_present_history *h, uint64_t frame) {
    unsigned k = (unsigned)(frame % VK_PRESENT_FRAMES);
    if (!frame || atomic_load_explicit(&h->frames[k].frame, memory_order_acquire) != frame) return 0;
    uint64_t actual = atomic_load_explicit(&h->frames[k].actual, memory_order_acquire);
    return atomic_load_explicit(&h->frames[k].frame, memory_order_acquire) == frame ? actual : 0;
}
