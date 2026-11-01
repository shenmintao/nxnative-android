"""Worker pool finalization excludes replay and presentation in either mode."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, pattern):
    match = re.search(pattern, source, re.MULTILINE)
    start = match.start()
    begin = source.index('{', start)
    depth = 1
    for end in range(begin + 1, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise AssertionError('Unclosed production function')


class NvnResourceLock(unittest.TestCase):
    def test_present_and_worker_finalization_share_the_gate_in_both_modes(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        source = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        helpers = '\n'.join(function(source, pattern) for pattern in (
            r'^static void replay_lock\(void\) \{',
            r'^static void replay_unlock\(void\) \{',
            r'^static uint64_t present_now\([^;\n]+\) \{',
            r'^NVN\(nvnMemoryPoolFinalize,[^\n]+ \{',
        ))
        prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct { int unused; } tex_t;
typedef struct { int magic, flags; uint8_t *mem; uint64_t size; } pool_t;
typedef pool_t NVNmemoryPool;
#define NVN(name, result, ...) static result h_##name(__VA_ARGS__)
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int active, release_present, lock_requested, finished, mutations;
static _Atomic int dirty_removed, flush_removed, drains, violations;
static _Thread_local int finalizer;
static int mode;
static int nvn_async(void) { return mode; }
static int lock_observed(pthread_mutex_t *mutex) {
    if (finalizer && mutex == &g_sub_mu)
        atomic_fetch_add_explicit(&lock_requested, 1, memory_order_release);
    return pthread_mutex_lock(mutex);
}
#define pthread_mutex_lock lock_observed
static void replay_lock(void);
static void replay_unlock(void);
static void nvn_drain(void) { (void)nvn_async(); atomic_fetch_add(&drains, 1); }
static void nvn_dirty_remove_range(void *p) { assert(p); atomic_fetch_add(&dirty_removed, 1); }
static void fl_drop(void *p, uint64_t size) { assert(p && size == 4096); atomic_fetch_add(&flush_removed, 1); }
static void mtl_pool_del(void *p) {
    assert(p);
    if (atomic_load_explicit(&active, memory_order_acquire)) atomic_fetch_add(&violations, 1);
    atomic_fetch_add(&mutations, 1);
}
static void mtl_alias_drop_pool(void *p) {
    assert(p);
    if (atomic_load_explicit(&active, memory_order_acquire)) atomic_fetch_add(&violations, 1);
    atomic_fetch_add(&mutations, 1);
}
static void vpool_release(const pool_t *p) { (void)p; abort(); }
static uint64_t mtl_present(const tex_t *tex, double period, const int crop[4]) {
    assert(tex && period == 0.5 && crop[0] == 3 && crop[3] == 6);
    atomic_store_explicit(&active, 1, memory_order_release);
    while (!atomic_load_explicit(&release_present, memory_order_acquire)) sched_yield();
    atomic_store_explicit(&active, 0, memory_order_release);
    return 0x123456789abcdef0ull;
}
'''
        suffix = r'''
static void *present_thread(void *unused) {
    (void)unused;tex_t tex={0};int crop[4]={3,4,5,6};
    assert(present_now(&tex,0.5,crop)==0x123456789abcdef0ull);
    return NULL;
}
static void *finalize_thread(void *arg) {
    finalizer=1;h_nvnMemoryPoolFinalize(arg);
    atomic_store_explicit(&finished,1,memory_order_release);return NULL;
}
int main(void) {
    for (mode=0;mode<2;mode++) {
        atomic_store(&active,0);atomic_store(&release_present,0);
        atomic_store(&lock_requested,0);atomic_store(&finished,0);
        atomic_store(&mutations,0);atomic_store(&dirty_removed,0);
        atomic_store(&flush_removed,0);atomic_store(&drains,0);atomic_store(&violations,0);
        pool_t pool={123,0,(uint8_t *)(uintptr_t)0x10000,4096};
        pthread_t presenter,worker;
        assert(!pthread_create(&presenter,NULL,present_thread,NULL));
        while(!atomic_load_explicit(&active,memory_order_acquire)) sched_yield();
        // The real present helper must own the gate while its backend uses it.
        int result=pthread_mutex_trylock(&g_sub_mu);
        if(!result) pthread_mutex_unlock(&g_sub_mu);
        assert(result==EBUSY);
        assert(!pthread_create(&worker,NULL,finalize_thread,&pool));
        while(!atomic_load_explicit(&lock_requested,memory_order_acquire)
              && !atomic_load_explicit(&finished,memory_order_acquire)) sched_yield();
        // Old synchronous replay_lock bypassed this gate and finalized live state.
        assert(atomic_load(&lock_requested)==1);
        assert(!atomic_load(&finished) && !atomic_load(&mutations));
        atomic_store_explicit(&release_present,1,memory_order_release);
        assert(!pthread_join(presenter,NULL));assert(!pthread_join(worker,NULL));
        assert(atomic_load(&finished) && atomic_load(&mutations)==2);
        assert(atomic_load(&lock_requested)==2 && !atomic_load(&violations));
        assert(atomic_load(&drains)==1 && atomic_load(&dirty_removed)==1);
        assert(atomic_load(&flush_removed)==1 && pool.magic==0);
    }
    return 0;
}
'''
        old_lock = helpers.replace(
            'static void replay_lock(void) { pthread_mutex_lock(&g_sub_mu); }',
            'static void replay_lock(void) { if (nvn_async()) pthread_mutex_lock(&g_sub_mu); }',
        ).replace(
            'static void replay_unlock(void) { pthread_mutex_unlock(&g_sub_mu); }',
            'static void replay_unlock(void) { if (nvn_async()) pthread_mutex_unlock(&g_sub_mu); }',
        )
        self.assertNotEqual(old_lock, helpers)
        with tempfile.TemporaryDirectory(prefix='nvn-resource-lock-') as temp:
            for name, variant in [('production', helpers), ('old-sync-bypass', old_lock)]:
                path, exe = Path(temp) / (name + '.c'), Path(temp) / name
                trace_header = (ROOT / 'nxnative/dm/android/sync_trace.h').as_posix()
                path.write_text('#include "' + trace_header + '"\n' + prefix + variant + suffix,
                                encoding='utf-8')
                built = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                        '-pthread', str(path), '-o', str(exe)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                if name == 'production':
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                else:
                    self.assertNotEqual(ran.returncode, 0, 'negative control must detect the old bypass')


if __name__ == '__main__':
    unittest.main()
