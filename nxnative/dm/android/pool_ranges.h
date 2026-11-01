// Address index for the Vulkan pool registry. Slots keep registration order semantics:
// when ranges overlap, the lowest current slot wins, as the original linear lookup did.
#pragma once
#include <stdint.h>

typedef struct { uint64_t lo, hi, max_hi; int slot; } vk_pool_range;
static inline void vk_ranges_prefix(vk_pool_range *r, int first, int n) {
    uint64_t hi = first ? r[first - 1].max_hi : 0;
    for (int i = first; i < n; ++i) { if (r[i].hi > hi) hi = r[i].hi; r[i].max_hi = hi; }
}
static inline void vk_ranges_add(vk_pool_range *r, int n, uint64_t lo, uint64_t hi, int slot) {
    int a = 0, b = n;
    while (a < b) { int m = a + (b - a) / 2; if (r[m].lo <= lo) a = m + 1; else b = m; }
    for (int i = n; i > a; --i) r[i] = r[i - 1];
    r[a] = (vk_pool_range){lo, hi, 0, slot}; vk_ranges_prefix(r, a, n + 1);
}
static inline void vk_ranges_remove(vk_pool_range *r, int n, int slot, int moved_slot) {
    int out = 0;
    for (int i = 0; i < n; ++i) {
        if (r[i].slot == slot) continue;
        r[out] = r[i]; if (r[out].slot == moved_slot) r[out].slot = slot; ++out;
    }
    vk_ranges_prefix(r, 0, out);
}
static inline int vk_ranges_find(const vk_pool_range *r, int n, uint64_t addr, uint64_t len) {
    if (len > UINT64_MAX - addr) return -1;
    uint64_t end = addr + len; int a = 0, b = n, best = -1;
    while (a < b) { int m = a + (b - a) / 2; if (r[m].lo <= addr) a = m + 1; else b = m; }
    // Prefix maxima skip every earlier non-overlapping range. Usually only one is visited.
    for (int i = a - 1; i >= 0 && r[i].max_hi >= end; --i)
        if (r[i].hi >= end && (best < 0 || r[i].slot < best)) best = r[i].slot;
    return best;
}
