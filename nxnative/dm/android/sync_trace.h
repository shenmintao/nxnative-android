// Opt-in diagnostic scopes. Release builds do not evaluate trace arguments.
// Collection can start after warmup, so ATrace state is checked on every entry.
#pragma once
#include <stdint.h>
#if !defined(DM_RELEASE) && (defined(__ANDROID__) || defined(DM_SYNC_TRACE_TEST))
#include <android/trace.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
static inline int dm_sync_requested(void) {
    static _Atomic int cached;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (!v) {
        const char *e = getenv("DM_TRACE_SYNC");
        v = e && e[0] == '1' && e[1] == 0 ? 1 : 2;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v == 1;
}
static inline int dm_sync_enabled(void) { return dm_sync_requested() && ATrace_isEnabled(); }
static inline int dm_sync_begin(const char *name) {
    if (!dm_sync_enabled()) return 0;
    ATrace_beginSection(name); return 1;
}
static inline void dm_sync_end(int *active) { if (*active) ATrace_endSection(); }
static inline void dm_sync_counter(const char *name, long long value) {
    if (dm_sync_enabled()) ATrace_setCounter(name, value);
}
// Preserve all 64 bits: Android's asynchronous 32-bit cookies can alias fences.
static inline int dm_sync_fence_begin(const char *phase, uint64_t fence) {
    if (!dm_sync_enabled()) return 0;
    char name[128]; snprintf(name, sizeof name, "%s f=%llu", phase, (unsigned long long)fence);
    ATrace_beginSection(name); return 1;
}
static inline void dm_sync_fence_point(const char *phase, uint64_t fence) {
    int active = dm_sync_fence_begin(phase, fence); dm_sync_end(&active);
}
static inline void dm_sync_pair_point(const char *phase, uint64_t a, uint64_t b) {
    if (!dm_sync_enabled()) return;
    char name[128]; snprintf(name, sizeof name, "%s a=%llu b=%llu", phase, (unsigned long long)a, (unsigned long long)b);
    ATrace_beginSection(name); ATrace_endSection();
}
#define DM_SYNC_CAT_INNER(a,b) a##b
#define DM_SYNC_CAT(a,b) DM_SYNC_CAT_INNER(a,b)
// cleanup keeps nesting balanced across returns, breaks and error paths.
#define DM_SYNC_SCOPE(name) int DM_SYNC_CAT(dm_sync_scope_,__COUNTER__) __attribute__((cleanup(dm_sync_end),unused)) = dm_sync_begin(name)
#define DM_SYNC_FENCE_SCOPE(name,fence) int DM_SYNC_CAT(dm_sync_fence_,__COUNTER__) __attribute__((cleanup(dm_sync_end),unused)) = dm_sync_fence_begin(name,fence)
void dm_sync_vulkan_install(void);
#else
#define dm_sync_requested() 0
#define dm_sync_enabled() 0
#define dm_sync_counter(...) ((void)0)
#define dm_sync_fence_point(...) ((void)0)
#define dm_sync_pair_point(...) ((void)0)
#define dm_sync_vulkan_install() ((void)0)
#define DM_SYNC_SCOPE(...) ((void)0)
#define DM_SYNC_FENCE_SCOPE(...) ((void)0)
#endif
