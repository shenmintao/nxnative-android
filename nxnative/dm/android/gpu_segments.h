// Union of timestamp intervals: command-buffer spans can overlap in a pipeline.
// Covered time still includes stalls within a buffer; it is not GPU active time.
#pragma once
#include <stdint.h>

enum { VK_GPU_SEGMENTS_MAX = 64 };
static inline int vk_gpu_segments(const uint64_t *pairs, unsigned count, uint64_t *span, uint64_t *covered, uint64_t *gaps) {
    if (!count || count > VK_GPU_SEGMENTS_MAX) return 0;
    struct { uint64_t lo, hi; } v[VK_GPU_SEGMENTS_MAX];
    for (unsigned i = 0; i < count; ++i) {
        if (pairs[2 * i + 1] < pairs[2 * i]) return 0; // a wrapped/invalid counter cannot establish a gap
        unsigned j = i;
        while (j && v[j - 1].lo > pairs[2 * i]) { v[j] = v[j - 1]; --j; }
        v[j].lo = pairs[2 * i]; v[j].hi = pairs[2 * i + 1];
    }
    uint64_t first = v[0].lo, lo = first, hi = v[0].hi, sum = 0;
    for (unsigned i = 1; i < count; ++i) {
        if (v[i].lo > hi) { sum += hi - lo; lo = v[i].lo; hi = v[i].hi; }
        else if (v[i].hi > hi) hi = v[i].hi;
    }
    sum += hi - lo;
    *span = hi - first; *covered = sum; *gaps = *span - sum;
    return 1;
}
