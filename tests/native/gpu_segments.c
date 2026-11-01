// Compare interval union against a discrete reference, including pipeline overlap.
#include "gpu_segments.h"
#include <stdio.h>
#include <string.h>

static uint32_t rng = 0x4197bac3;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int check(const uint64_t *t, unsigned n, uint64_t span, uint64_t covered, uint64_t gaps) {
    uint64_t s, c, g;
    return vk_gpu_segments(t, n, &s, &c, &g) && s == span && c == covered && g == gaps;
}
int main(void) {
    uint64_t boundary[] = {UINT64_MAX - 4, UINT64_MAX, UINT64_MAX - 10, UINT64_MAX - 2};
    if (!check(boundary, 2, 10, 10, 0)) return 1;
    uint64_t nested[] = {20, 25, 10, 40, 60, 65, 40, 45};
    if (!check(nested, 4, 55, 40, 15)) return 2;
    uint64_t invalid[] = {10, 9}, a = 1, b = 2, c = 3;
    if (vk_gpu_segments(invalid, 1, &a, &b, &c) || a != 1 || b != 2 || c != 3) return 3;
    if (vk_gpu_segments(NULL, 0, &a, &b, &c) || vk_gpu_segments(NULL, VK_GPU_SEGMENTS_MAX + 1, &a, &b, &c)) return 4;
    for (int trial = 0; trial < 100000; ++trial) {
        uint64_t t[VK_GPU_SEGMENTS_MAX * 2], lo = 256, hi = 0, used = 0;
        uint8_t reference[256] = {0}; unsigned n = 1 + next() % VK_GPU_SEGMENTS_MAX;
        for (unsigned i = 0; i < n; ++i) {
            uint64_t x = next() % 256, y = next() % 256;
            if (x > y) { uint64_t q = x; x = y; y = q; }
            t[2 * i] = x; t[2 * i + 1] = y;
            if (x < lo) lo = x;
            if (y > hi) hi = y;
            for (uint64_t k = x; k < y; ++k) reference[k] = 1;
        }
        for (unsigned i = 0; i < 256; ++i) used += reference[i];
        if (!check(t, n, hi - lo, used, hi - lo - used)) return 5;
    }
    puts("GPU segment union: 100,000 random overlapping interval sets, invalid counters and large timestamps passed");
    return 0;
}
