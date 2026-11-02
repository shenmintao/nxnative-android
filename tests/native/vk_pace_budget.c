#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vk_pace_budget.h"

#if CASE == 0
static void check(double latest, const double *spans, int count, double period, double want) {
    double saved[8], trimmed = -1; if (count) memcpy(saved, spans, (size_t)count * sizeof *spans);
    double got = vk_pace_budget_estimate(latest, spans, count, period, &trimmed);
    assert(fabs(got - want) < 1e-9 && fabs(trimmed - (latest - want)) < 1e-9);
    if (count) assert(!memcmp(saved, spans, (size_t)count * sizeof *spans));
}
int main(void) {
    const double steady[] = {13,13,13,13,13,13,13,13}; check(13,steady,8,16.7,13);
    const double spike[] = {170,13,14,12,13,13,14,12}; check(170,spike,8,16.7,13);
    check(170,spike,8,33.3,13);  // interval 2 changes detection, not the typical GPU work
    const double period_boundary[] = {33.3,13,13}; check(33.3,period_boundary,3,33.3,33.3);
    const double first[] = {203,13,13,13,13,13,13,13}; check(203,first,8,16.7,13);
    const double second[] = {68,203,13,13,13,13,13,13}; check(68,second,8,16.7,13);
    const double heavy[] = {30,30,30,30,30,30,30,30}; check(30,heavy,8,16.7,30);
    const double low[] = {5,2,2,2,2,2,2,2}; check(5,low,8,16.7,5);
    const double short_history[] = {170,13};
    for (int n = 0; n < 3; n++) check(170,short_history,n,16.7,170);
    check(-1,steady,8,16.7,-1); check(-7,NULL,0,16.7,-7);
    const double period_cap[] = {170,2,2}; check(170,period_cap,3,16.7,2);
    const double even[] = {170,11,12,13,14,15,16,17}; check(170,even,8,16.7,14.5);
    const double boundary[] = {26,13,13}; check(26,boundary,3,16.7,26);
    const double missing[] = {170,-1,13,-1}; check(170,missing,4,16.7,170);
    assert(vk_pace_budget_estimate(170,spike,8,16.7,NULL) == 13);
    return 0;
}
#endif

#if CASE == 1 || CASE == 3
/* FRAME SPANS */
static void sample(uint64_t frame, double ms) {
    g_frame_gpu_of[frame % 64] = frame + 1; g_frame_gpu[frame % 64] = (uint64_t)(ms * 1e6);
}
static void fresh(void) { memset(g_frame_gpu_of,0,sizeof g_frame_gpu_of); memset(g_frame_gpu,0,sizeof g_frame_gpu); }
#endif

#if CASE == 1
int main(void) {
    double trimmed;
    fresh(); mtl_set_frame(0); assert(vk_gpu_ms_budget(16.7,&trimmed) == -1 && trimmed == 0);
    sample(0,13); mtl_set_frame(1); assert(vk_gpu_ms_budget(16.7,&trimmed) == vk_gpu_ms_recent());
    mtl_set_frame(2); assert(vk_gpu_ms_budget(16.7,&trimmed) == 13 && trimmed == 0);
    fresh(); mtl_set_frame(100); sample(100,999); sample(99,999); sample(98,13);
    assert(vk_gpu_ms_budget(16.7,&trimmed) == vk_gpu_ms_recent() && trimmed == 0);
    sample(98,170); sample(96,13); sample(90,13);  // sparse known spans, including history beyond f-7
    assert(vk_gpu_ms_recent() == 170 && vk_gpu_ms_budget(16.7,&trimmed) == 13 && trimmed == 157);
    fresh(); sample(92,13); sample(90,13); sample(89,13);  // only old entries: raw latest is still unknown
    assert(vk_gpu_ms_recent() == -1 && vk_gpu_ms_budget(16.7,&trimmed) == -1 && trimmed == 0);
    fresh(); sample(98,170);
    for (int i = 3; i <= 9; i++) sample(100 - (uint64_t)i,13);
    sample(90,999); assert(vk_gpu_ms_budget(16.7,&trimmed) == 13 && trimmed == 157);
    sample(98 - 64,999);  // stale ring slot must not count as frame 98
    assert(vk_gpu_ms_recent() == 13 && vk_gpu_ms_budget(16.7,&trimmed) == 13);
    for (uint64_t f = 0; f < 200; f++) {
        fresh(); mtl_set_frame(f);
        for (uint64_t k = 0; k <= f && k < 64; k++) if (k % 3) sample(f - k,13 + k % 5);
        assert(vk_gpu_ms_budget(16.7,&trimmed) == vk_gpu_ms_recent() && trimmed == 0);
    }
    return 0;
}
#endif

#if CASE == 2 || CASE == 3
static char log_line[1024]; static int logs;
static void test_log(const char *format, ...) {
    va_list args; va_start(args,format); vsnprintf(log_line,sizeof log_line,format,args); va_end(args); logs++;
}
#define VKLOG(...) test_log(__VA_ARGS__)
#endif

#if CASE == 2
static const char *request;
static unsigned sleeps, consumed, drains, interrupted, env_reads;
static uint64_t clock_ns = 1000000000ull;
static uint64_t vk_debug_next_present(void) { return 123; }
static int g_cur;
typedef struct { int end_error, last_sent; uint64_t cpu_first; } part;
typedef struct { int nmain_sent, deferred_pending, cb_err; part deferred_part; uint64_t cpu_first; } vksub_t;
static vksub_t g_slot[1];
static const char *stall_getenv(const char *name) { assert(!strcmp(name,"DM_VK_X_STALL_MS")); env_reads++; return request; }
static int stall_unsetenv(const char *name) { assert(!strcmp(name,"DM_VK_X_STALL_MS") && request); request = NULL; consumed++; return 0; }
static uint64_t tnow(void) { return clock_ns; }
static void vk_encode_drain(void) { assert(request && !sleeps); drains++; }
static int stall_sleep(const struct timespec *delay, struct timespec *left) {
    assert(!request && g_slot[0].nmain_sent > 0 && !g_slot[0].deferred_pending);
    uint64_t ns = (uint64_t)delay->tv_sec * 1000000000ull + (uint64_t)delay->tv_nsec;
    if (!interrupted++) { clock_ns += 1000000; *left = (struct timespec){0,(long)(ns - 1000000)}; errno = EINTR; return -1; }
    sleeps++; clock_ns += ns; return 0;
}
#define getenv stall_getenv
#define unsetenv stall_unsetenv
#define nanosleep stall_sleep
/* STALL */
int main(void) {
    request = "150"; replay_stall_read(); replay_stall(); assert(request && !sleeps && !consumed);  // no part sent
    for (int i = 0; i < 20; i++) replay_stall();
    assert(env_reads == 1 && request && !sleeps);  // draws never scan the environment
    g_slot[0].deferred_pending = 1; g_slot[0].deferred_part = (part){0,1,42};
    replay_stall(); assert(!request && sleeps == 1 && consumed == 1 && drains == 1 && logs == 1);
    assert(strstr(log_line,"present 123 requested_ms=150 start_ns=1000000000 end_ns=1150000000"));
    for (int i = 0; i < 20; i++) replay_stall();
    assert(sleeps == 1 && consumed == 1 && logs == 1 && env_reads == 1);
    request = "150"; replay_stall(); assert(sleeps == 1);  // next submission reads the re-armed request
    replay_stall_read(); replay_stall(); assert(!request && sleeps == 2 && consumed == 2 && logs == 2 && env_reads == 2);
    const char *bad[] = {"0","1001","-1","bad","150ms"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) { request = bad[i]; replay_stall_read(); replay_stall(); assert(request && sleeps == 2); }
    request = "1"; replay_stall_read(); replay_stall(); request = "1000"; replay_stall_read(); replay_stall(); assert(sleeps == 4 && consumed == 4);
    g_cur = -1; request = "150"; replay_stall_read(); replay_stall(); assert(request && sleeps == 4);
    g_cur = 0; g_slot[0].cb_err = 1; replay_stall(); assert(request && sleeps == 4);
    return 0;
}
#endif

#if CASE == 3
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
static struct { VkDevice dev; int has_dtiming; } V = {.has_dtiming = 1};
static VkSwapchainKHR g_swap = (VkSwapchainKHR)(uintptr_t)1;
static uint64_t g_pres_recorded;
static const char *budget_env, *pace_env, *lead_env;
static unsigned raw_calls, budget_calls;
static const char *pace_getenv(const char *name) {
    if (!strcmp(name,"DM_VK_PACE_BUDGET")) return budget_env;
    if (!strcmp(name,"DM_VK_PACE")) return pace_env;
    assert(!strcmp(name,"DM_VK_SNAPSHOT_LEAD")); return lead_env;
}
static int pace_clock(clockid_t id, struct timespec *t) { assert(id == CLOCK_MONOTONIC); *t = (struct timespec){1,0}; return 0; }
static VkResult vkGetRefreshCycleDurationGOOGLE(VkDevice dev, VkSwapchainKHR swap, VkRefreshCycleDurationGOOGLE *rc) {
    (void)dev; (void)swap; rc->refreshDuration = 8333333; return VK_SUCCESS;
}
static VkResult vkGetPastPresentationTimingGOOGLE(VkDevice dev, VkSwapchainKHR swap, uint32_t *n, VkPastPresentationTimingGOOGLE *pt) {
    (void)dev; (void)swap; *n = 1; *pt = (VkPastPresentationTimingGOOGLE){.actualPresentTime = 1000000000}; return VK_SUCCESS;
}
static void present_time_record(uint32_t id, uint64_t ns) { (void)id; (void)ns; }
static double budget_call(double period, double *trimmed) { budget_calls++; return vk_gpu_ms_budget(period,trimmed); }
static double raw_call(void) { raw_calls++; return vk_gpu_ms_recent(); }
#define getenv pace_getenv
#define clock_gettime pace_clock
#define vk_gpu_ms_budget budget_call
#define vk_gpu_ms_recent raw_call
/* PACER */
int main(void) {
    fresh(); mtl_set_frame(100); for (uint64_t k = 2; k < 10; k++) sample(100 - k,13);
    uint64_t normal = pace_desired(1.0 / 60,0); assert(normal && g_pace_budget_on && budget_calls == 1 && !raw_calls && !g_pace_trimmed);
    pace_reset(); sample(98,170); uint64_t capped = pace_desired(1.0 / 60,0);
    assert(capped < normal + 20000000 && g_pace_trimmed == 1 && g_pace_trimmed_ms == 157);
    budget_env = "0"; g_pace_n = 29; pace_desired(1.0 / 60,0); assert(g_pace_budget_on);
    pace_reset(); uint64_t legacy = pace_desired(1.0 / 60,0); assert(!g_pace_budget_on && raw_calls == 1 && legacy > capped + 100000000);
    budget_env = "1"; g_pace_n = 60; pace_reset(); pace_desired(1.0 / 60,0); assert(g_pace_budget_on);
    unsigned calls = budget_calls; uint64_t trims = g_pace_trimmed;
    pace_desired(1.0 / 60,1); assert(budget_calls == calls && g_pace_trimmed == trims);
    lead_env = "0"; g_pace_n = 90; pace_desired(1.0 / 60,1); assert(budget_calls == calls + 1 && g_pace_trimmed == trims + 1);
    g_pace_n = 599; pace_desired(1.0 / 60,0);
    assert(strstr(log_line,"[perf] display timing: 600 frames,") && strstr(log_line,"actual presentation samples (cumulative); budget_on=1 budget_trimmed="));
    assert(strstr(log_line,"budget_trimmed_ms="));
    pace_env = "0"; g_pace_n = 600; calls = budget_calls;
    assert(!pace_desired(1.0 / 60,0) && budget_calls == calls);
    pace_env = "1"; g_pace_n = 630; pace_reset();
    uint64_t half = pace_desired(1.0 / 30,0), next = pace_desired(1.0 / 30,0);
    assert(next >= half + 30000000);  // an outlier replacement still honours interval 2
    return 0;
}
#endif
