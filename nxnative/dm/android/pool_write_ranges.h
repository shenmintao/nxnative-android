// Exact unions of recorded GPU write spans. Gaps may contain newer CPU data,
// so a readback must never fill them merely to bound the number of spans.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_POOL_WRITES_REALLOC
#define VK_POOL_WRITES_REALLOC realloc
#endif

typedef struct { uint64_t lo, hi; } vk_pool_write_span;
typedef struct {
    vk_pool_write_span *spans;
    size_t count, capacity;
} vk_pool_writes;

static inline int vk_pool_writes_add(vk_pool_writes *writes, uint64_t address, uint64_t size) {
    if (!size) return 1;
    if (size > UINT64_MAX - address) return 0;
    uint64_t lo = address, hi = address + size;
    for (size_t i = 0; i < writes->count;) {
        vk_pool_write_span span = writes->spans[i];
        if (span.hi < lo || hi < span.lo) { i++; continue; }
        if (span.lo < lo) lo = span.lo;
        if (span.hi > hi) hi = span.hi;
        writes->spans[i] = writes->spans[--writes->count];
        i = 0;
    }
    if (writes->count == writes->capacity) {
        if (writes->capacity > SIZE_MAX / 2) return 0;
        size_t capacity = writes->capacity ? writes->capacity * 2 : 64;
        if (capacity > SIZE_MAX / sizeof *writes->spans) return 0;
        void *spans = VK_POOL_WRITES_REALLOC(writes->spans, capacity * sizeof *writes->spans);
        if (!spans) return 0;
        writes->spans = spans; writes->capacity = capacity;
    }
    writes->spans[writes->count++] = (vk_pool_write_span){lo, hi};
    return 1;
}

static inline void vk_pool_writes_copy_back(uint8_t *cpu, const uint8_t *gpu,
                                          uint64_t pool_lo, uint64_t pool_hi, const vk_pool_writes *writes) {
    if (pool_lo >= pool_hi) return;
    for (size_t i = 0; i < writes->count; i++) {
        uint64_t lo = writes->spans[i].lo > pool_lo ? writes->spans[i].lo : pool_lo;
        uint64_t hi = writes->spans[i].hi < pool_hi ? writes->spans[i].hi : pool_hi;
        if (lo < hi) memcpy(cpu + (lo - pool_lo), gpu + (lo - pool_lo), hi - lo);
    }
}
