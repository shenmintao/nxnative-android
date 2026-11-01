// Recent writes invalidate only overlapping uniform shadows. A full event marks
// a landing/drop boundary, so scratch buffers cannot survive their submission.
#pragma once
#include <stdint.h>

enum { VK_UNIFORM_HISTORY = 128 };
typedef struct { uint64_t gen; struct { uint64_t lo, hi; } writes[VK_UNIFORM_HISTORY]; } vk_uniform_versions;
static inline void vk_uniform_changed(vk_uniform_versions *v, uint64_t a, uint64_t n) {
    uint64_t end = a + n; unsigned k = (unsigned)(++v->gen % VK_UNIFORM_HISTORY);
    v->writes[k].lo = n && end > a ? a : 0;
    v->writes[k].hi = n && end > a ? end : 0;   // zero size or overflow: invalidate everything
}
static inline int vk_uniform_unchanged(const vk_uniform_versions *v, uint64_t since, uint64_t a, uint64_t n) {
    if (!since || since > v->gen || v->gen - since > VK_UNIFORM_HISTORY) return 0;
    if (n > UINT64_MAX - a) return 0;
    for (uint64_t g = since; g != v->gen;) {
        unsigned k = (unsigned)(++g % VK_UNIFORM_HISTORY);
        if (!v->writes[k].hi || (v->writes[k].lo < a + n && a < v->writes[k].hi)) return 0;
    }
    return 1;
}
static inline unsigned vk_uniform_cache_slot(uint64_t a, uint64_t n, unsigned slots) {
    uint64_t h = a ^ n * 0x9e3779b97f4a7c15ull;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (unsigned)h & (slots - 1);
}
