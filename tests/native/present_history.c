// Real display feedback must not be confused with enqueue time, stale ring
// entries, host redraws, duplicate driver reports or 32-bit ID wraparound.
#include "present_history.h"
#include <stdio.h>
#include <pthread.h>

static vk_present_history history;
static _Atomic uint64_t latest;
static _Atomic int finished, mismatch;
static void *read_actual(void *context) {
    (void)context; uint64_t k = 0;
    while (!atomic_load_explicit(&finished, memory_order_acquire)) {
        uint64_t n = atomic_load_explicit(&latest, memory_order_acquire), age = k++ % 96;
        if (n <= age) continue;
        n -= age; uint64_t t = vk_present_actual(&history, n);
        if (t && t != n * 1234567ull + 1048576ull) atomic_store(&mismatch, 1);
    }
    return NULL;
}
int main(void) {
    uint32_t ids[3000];
    if (vk_present_actual(&history, 1)) return 1;
    for (uint64_t n = 1; n < 3000; ++n) {
        ids[n] = vk_present_issue(&history, n);
        if (vk_present_actual(&history, n)) return 2;
        uint32_t redraw = vk_present_issue(&history, 0);
        if (vk_present_record(&history, redraw, n * 17000000ull)) return 3;
        if (n > 8) {
            uint64_t shown = n - 8, time = shown * 16666667ull;
            if (!vk_present_record(&history, ids[shown], time)) return 4;
            if (vk_present_record(&history, ids[shown], time + 1234)) return 5;
            if (vk_present_actual(&history, shown) != time) return 6;
        }
        if (n > 64 && vk_present_actual(&history, n - 64)) return 7;
    }
    if (vk_present_record(&history, ids[1], 999)) return 8;
    uint32_t last = vk_present_issue(&history, 10000);
    vk_present_discard(&history, 10000);
    if (vk_present_actual(&history, 10000) != UINT64_MAX) return 9;
    if (vk_present_record(&history, last, 100)) return 10;
    history.next = UINT32_MAX - 1;
    if (vk_present_issue(&history, 10001) != UINT32_MAX) return 11;
    if (vk_present_issue(&history, 10002) != 1) return 12;
    if (!vk_present_record(&history, 1, 200)) return 13;
    if (vk_present_actual(&history, 10002) != 200) return 14;
    uint32_t next = history.next;
    vk_present_clear(&history);
    if (history.next != next || vk_present_actual(&history, 10002)) return 17;
    vk_present_begin(&history, 10002);
    if (vk_present_record(&history, 1, 300)) return 18;  // old swapchain ID
    vk_present_map(&history, 0, 10002);
    if (!vk_present_record(&history, 0, 400) || vk_present_actual(&history, 10002) != 400) return 19;
    if (vk_present_record(&history, 0, 500)) return 20;
    vk_present_clear(&history);
    pthread_t reader;
    if (pthread_create(&reader, NULL, read_actual, NULL)) return 15;
    for (uint64_t n = 20000; n < 120000; ++n) {
        uint32_t id = vk_present_issue(&history, n);
        atomic_store_explicit(&latest, n, memory_order_release);
        vk_present_record(&history, id, n * 1234567ull + 1048576ull);
    }
    atomic_store_explicit(&finished, 1, memory_order_release); pthread_join(reader, NULL);
    if (atomic_load(&mismatch)) return 16;
    puts("present history: delayed feedback, redraws, stale rings, ID wrap and 100,000 concurrent frame updates passed");
    return 0;
}
