// Check invalidations against the last write to every byte, including a full
// submission boundary, history overflow and ranges that only touch at an edge.
#include "uniform_versions.h"
#include <assert.h>
#include <stdio.h>

static uint32_t rng = 0xd5482371;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int main(void) {
    vk_uniform_versions v = {.gen = 1}; uint64_t last[4096] = {0}, full = 0;
    struct { uint64_t gen; unsigned a, n; } cache[32] = {0};
    for (int step = 0; step < 100000; step++) {
        unsigned a = next() % 4096, n = next() % (4097 - a);
        vk_uniform_changed(&v, 0x100000ull + a, n);
        if (!n) full = v.gen; else for (unsigned j = a; j < a + n; j++) last[j] = v.gen;
        for (int q = 0; q < 8; q++) {
            unsigned k = next() % 32;
            if (!cache[k].gen || next() % 4 == 0) {
                cache[k].a = next() % 4096; cache[k].n = 1 + next() % (4096 - cache[k].a); cache[k].gen = v.gen;
            }
            int expected = v.gen - cache[k].gen <= VK_UNIFORM_HISTORY && full <= cache[k].gen;
            for (unsigned j = cache[k].a; j < cache[k].a + cache[k].n; j++) if (last[j] > cache[k].gen) expected = 0;
            int actual = vk_uniform_unchanged(&v, cache[k].gen, 0x100000ull + cache[k].a, cache[k].n);
            assert(actual == expected);
        }
    }
    uint64_t before = v.gen; vk_uniform_changed(&v, 100, 20);
    assert(vk_uniform_unchanged(&v, before, 80, 20)); assert(vk_uniform_unchanged(&v, before, 120, 20));
    assert(!vk_uniform_unchanged(&v, before, 99, 2)); assert(!vk_uniform_unchanged(&v, before, 119, 2));
    before = v.gen; vk_uniform_changed(&v, 0, 0); assert(!vk_uniform_unchanged(&v, before, 900, 8));
    before = v.gen; for (int i = 0; i <= VK_UNIFORM_HISTORY; i++) vk_uniform_changed(&v, 10, 2);
    assert(!vk_uniform_unchanged(&v, before, 900, 8));
    before = v.gen; vk_uniform_changed(&v, UINT64_MAX - 1, 4); assert(!vk_uniform_unchanged(&v, before, 900, 8));
    puts("uniform versions: 800000 byte-history comparisons and boundary cases passed"); return 0;
}
