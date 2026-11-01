// Compare the production reader with the original full chronological overlay,
// including completed copies, partial overlaps, strided pending records and guards.
#include "uniform_overlay.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { vk_uniform_span span; uint64_t gpu_buffer, gpu_offset; } pending_t;
static uint32_t rng = 0xa53891b2u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void reference(uint8_t *dst, const uint8_t *base, uint64_t a, uint64_t n, vk_uniform_list in, vk_uniform_list pend) {
    memcpy(dst, base, (size_t)n);
    vk_uniform_list lists[2] = {in, pend};
    for (int l = 0; l < 2; l++) for (int i = 0; i < lists[l].count; i++) {
        const vk_uniform_span *s = vk_uniform_at(lists[l], i);
        if (lists[l].fenced && s->fence <= lists[l].done) continue;
        for (uint64_t j = 0; j < n; j++) if (a + j >= s->lo && a + j < s->hi) dst[j] = s->data[a + j - s->lo];
    }
}
int main(void) {
    uint8_t memory[1024], data[80][1024], expected[1056], got[1056];
    vk_uniform_span inflight[40]; pending_t pending[40];
    for (int trial = 0; trial < 10000; trial++) {
        uint64_t origin = trial % 7 ? 0x100000000ull : UINT64_MAX - 1024;
        for (int i = 0; i < 1024; i++) memory[i] = (uint8_t)next();
        int ni = (int)(next() % 41), np = (int)(next() % 41);
        for (int i = 0; i < ni + np; i++) {
            int lo = (int)(next() % 1024), n = (int)(next() % (1025 - lo));
            if (i % 11 == 0) { lo = 0; n = 1024; }
            for (int j = 0; j < n; j++) data[i][j] = (uint8_t)next();
            vk_uniform_span s = {origin + (uint64_t)lo, origin + (uint64_t)lo + (uint64_t)n, next() % 16, data[i]};
            if (i < ni) inflight[i] = s; else pending[i - ni].span = s;
        }
        vk_uniform_list in = {inflight, sizeof *inflight, ni, 1, next() % 16};
        vk_uniform_list pend = {pending, sizeof *pending, np, 0, 0};
        for (int q = 0; q < 10; q++) {
            int off = (int)(next() % 1024), n = (int)(next() % (1025 - off));
            memset(expected, 0xa5, sizeof expected); memset(got, 0xa5, sizeof got);
            reference(expected + 16, memory + off, origin + (uint64_t)off, n, in, pend);
            vk_uniform_read(got + 16, memory + off, origin + (uint64_t)off, n, in, pend, 1);
            if (memcmp(expected, got, sizeof got)) { fprintf(stderr, "uniform overlay mismatch: trial %d read %d\n", trial, q); return 1; }
        }
    }
    puts("uniform overlay: 100000 randomized comparisons passed");
    return 0;
}
