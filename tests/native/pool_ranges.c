// Exercise the actual lookup index against the old scan, including overlapping pools
// and swap-deletions. No Android, Vulkan, game files or keys are needed.
#include "pool_ranges.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t rng = 0x73211234;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int main(void) {
    vk_pool_range index[512], slots[512]; int n = 0;
    for (int step = 0; step < 50000; ++step) {
        if (n && (n == 512 || next() % 4 == 0)) {
            int remove = (int)(next() % (uint32_t)n);
            vk_ranges_remove(index, n, remove, n - 1); slots[remove] = slots[--n];
        } else {
            uint64_t lo = next() % 10000, hi = lo + next() % 300;
            slots[n] = (vk_pool_range){lo, hi, 0, n}; vk_ranges_add(index, n, lo, hi, n); ++n;
        }
        for (int q = 0; q < 20; ++q) {
            uint64_t addr = next() % 11000, len = next() % 350; int want = -1;
            for (int i = 0; i < n; ++i) if (addr >= slots[i].lo && addr + len <= slots[i].hi) { want = i; break; }
            int have = vk_ranges_find(index, n, addr, len);
            if (want != have) { fprintf(stderr, "lookup mismatch at step %d: %d != %d\n", step, have, want); return 1; }
        }
    }
    if (vk_ranges_find(index, n, UINT64_MAX - 3, 8) != -1) return 2;
    puts("1,000,000 indexed lookups match the original scan, with insertions, overlaps, swap-deletions and overflow checks");
    return 0;
}
