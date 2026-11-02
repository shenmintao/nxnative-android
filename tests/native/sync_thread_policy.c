#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) ((void)0)
typedef uint32_t cpu_set_t;
#define CPU_ZERO(set) (*(set) = 0)
#define CPU_SET(cpu, set) (*(set) |= (uint32_t)1 << (cpu))
#define _SC_NPROCESSORS_CONF 1
static _Thread_local int test_tid;
static int count_cpus = 8, uniform, unknown, refused;
static _Atomic unsigned opens, attempts;
static int selected_tid;
static cpu_set_t selected_mask;
static long test_sysconf(int key) { assert(key == _SC_NPROCESSORS_CONF); return count_cpus; }
static int test_gettid(void) { return test_tid; }
static FILE *test_fopen(const char *path, const char *mode) {
    int cpu = -1; assert(sscanf(path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", &cpu) == 1);
    assert(cpu >= 0 && cpu < count_cpus && !strcmp(mode, "r")); atomic_fetch_add(&opens, 1);
    if (unknown) return NULL;
    FILE *file = tmpfile(); assert(file);
    fprintf(file, "%d", uniform || cpu < count_cpus-2 ? 2000000 : 4000000);
    rewind(file); return file;
}
static int test_affinity(int tid, size_t size, const cpu_set_t *mask) {
    assert(tid == 0 && size == sizeof *mask && test_tid != 10);
    atomic_fetch_add(&attempts, 1); selected_tid = test_tid; selected_mask = *mask;
    return refused ? -1 : 0;
}
#define fopen test_fopen
#define sysconf test_sysconf
#define gettid test_gettid
#define sched_setaffinity test_affinity
/* PRODUCTION POLICY */
static void *caller(void *arg) {
    test_tid = (int)(intptr_t)arg;
    for (unsigned i = 0; i < 100; i++) dm_perf_sync_wait(0);
    for (unsigned i = 0; i < 100; i++) dm_perf_sync_wait(1);
    if (test_tid == 20) assert(opens == 0 && attempts == 0);
    for (unsigned i = 2; i <= 31; i++) dm_perf_sync_wait(i);
    if (test_tid == 20) assert(opens == 0 && attempts == 0);
    dm_perf_sync_wait(32);
    for (unsigned i = 33; i < 1000; i++) dm_perf_sync_wait(i);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int enabled = strcmp(argv[1], "off") != 0;
    putenv(enabled ? "DM_PIN_SYNC_THREAD=prime" : "DM_PIN_SYNC_THREAD=0");
    uniform = !strcmp(argv[1], "uniform"); unknown = !strcmp(argv[1], "unknown"); refused = !strcmp(argv[1], "refused");
    if (!strcmp(argv[1], "quad")) count_cpus = 4;
    test_tid = 10; atomic_store(&g_perf_frame_tid, 10);
    for (unsigned i = 1; i < 100; i++) dm_perf_sync_wait(i);
    assert(opens == 0 && attempts == 0 && !atomic_load(&g_sync_pin_tid));
    pthread_t thread; pthread_create(&thread, NULL, caller, (void *)20); pthread_join(thread, NULL);
    pthread_t others[4];
    for (unsigned i = 0; i < 4; i++) pthread_create(&others[i], NULL, caller, (void *)(intptr_t)(30+i));
    for (unsigned i = 0; i < 4; i++) pthread_join(others[i], NULL);
    assert(opens == (unsigned)(enabled ? count_cpus : 0));
    assert(attempts == (unsigned)(enabled && !uniform && !unknown));
    if (attempts) { assert(selected_tid == 20); assert(selected_mask == ((uint32_t)3 << (count_cpus-2))); }
    return 0;
}
