// CPU view of ordered uniform updates. A recent update covering the whole read
// replaces both base memory and all earlier updates; later partial writes still apply.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t lo, hi, fence; const uint8_t *data; } vk_uniform_span;
typedef struct { const void *items; size_t stride; int count, fenced; uint64_t done; } vk_uniform_list;

static inline const vk_uniform_span *vk_uniform_at(vk_uniform_list l, int i) {
    return (const vk_uniform_span *)((const uint8_t *)l.items + l.stride * (size_t)i);
}
static inline int vk_uniform_live(vk_uniform_list l, const vk_uniform_span *s) { return !l.fenced || s->fence > l.done; }

typedef void (*vk_uniform_copy_fn)(void *context, uint8_t *dst, const uint8_t *src, size_t size);
static inline void vk_uniform_emit(uint8_t *dst, const uint8_t *base, uint64_t a, uint64_t n,
                                   vk_uniform_list inflight, vk_uniform_list pending, int recent_cover,
                                   vk_uniform_copy_fn copy, void *context) {
    vk_uniform_list lists[2] = {inflight, pending};
    int first_list = 0, first = 0, seeded = 0;
    const uint64_t end = a + n;
    // Bound the extra search when no update covers the read. Most inline updates
    // replace a complete binding and are among the last few writes before a draw.
    if (recent_cover) for (int l = 1; l >= 0 && !seeded; l--) {
        vk_uniform_list v = lists[l]; int stop = v.count > 8 ? v.count - 8 : 0;
        for (int i = v.count - 1; i >= stop; i--) {
            const vk_uniform_span *s = vk_uniform_at(v, i);
            if (vk_uniform_live(v, s) && s->lo <= a && end <= s->hi) {
                copy(context, dst, s->data + (a - s->lo), (size_t)n);
                first_list = l; first = i + 1; seeded = 1; break;
            }
        }
    }
    if (!seeded) copy(context, dst, base, (size_t)n);
    for (int l = first_list; l < 2; l++) {
        vk_uniform_list v = lists[l];
        for (int i = l == first_list ? first : 0; i < v.count; i++) {
            const vk_uniform_span *s = vk_uniform_at(v, i);
            if (!vk_uniform_live(v, s)) continue;
            uint64_t lo = s->lo > a ? s->lo : a, hi = s->hi < end ? s->hi : end;
            if (lo < hi) copy(context, dst + (lo - a), s->data + (lo - s->lo), (size_t)(hi - lo));
        }
    }
}
static inline void vk_uniform_copy_now(void *context, uint8_t *dst, const uint8_t *src, size_t size) {
    (void)context; memcpy(dst, src, size);
}
static inline void vk_uniform_read(uint8_t *dst, const uint8_t *base, uint64_t a, uint64_t n,
                                   vk_uniform_list inflight, vk_uniform_list pending, int recent_cover) {
    vk_uniform_emit(dst, base, a, n, inflight, pending, recent_cover, vk_uniform_copy_now, NULL);
}
