"""Exact buffer lookup caching preserves overlap, bounds and registry mutations."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class BufferLookupCache(unittest.TestCase):
    def test_exact_queries_generation_handoffs_and_concurrent_registration(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        pool_type = next(s for s in header.splitlines() if '} vkpool_t;' in s)
        start = source.index('typedef struct { uint64_t addr, len, off, available;')
        declarations = source[start:source.index('// Grow only', start)]
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pool_ranges.h"
#define VKLOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
static pthread_rwlock_t g_pool_mu=PTHREAD_RWLOCK_INITIALIZER;
static _Atomic unsigned reads;
static int counted_read(pthread_rwlock_t *lock) { atomic_fetch_add(&reads,1); return pthread_rwlock_rdlock(lock); }
#define pthread_rwlock_rdlock counted_read
''' + pool_type + r'''
static vkpool_t pools[3],*g_pool[3]={&pools[0],&pools[1],&pools[2]};
static vk_pool_range ranges[3],*g_pool_ranges=ranges;
static int g_npool=3;
static uint32_t g_pool_gen=1;
''' + declarations + '\n'.join(function(source, signature) for signature in (
            'void vk_buffer_cache_config(', 'void vk_buffer_cache_stats(', 'int vk_buf_range_at(')) + r'''
static void rebuild(void) {
    for(int i=0;i<g_npool;i++) vk_ranges_add(ranges,i,pools[i].lo,pools[i].hi,i);
    __atomic_fetch_add(&g_pool_gen,1,__ATOMIC_RELEASE);
}
static void check(uint64_t a,uint64_t len,int want_available) {
    uint64_t need=len ? len : 1;int found=-1;
    if(need<=UINT64_MAX-a) for(int i=0;i<g_npool;i++)
        if(a>=pools[i].lo && a+need<=pools[i].hi) {found=i;break;}
    VkBuffer buffer=(VkBuffer)(uintptr_t)0xfed;
    VkDeviceSize off=0xfeed;uint64_t available=0xbeef;
    assert(vk_buf_range_at(a,len,&buffer,&off,want_available ? &available : NULL)==(found>=0));
    if(found>=0) {
        assert(buffer==pools[found].buf && off==a-pools[found].lo);
        if(want_available) assert(available==pools[found].hi-a);
    } else assert(buffer==(VkBuffer)(uintptr_t)0xfed && off==0xfeed && available==0xbeef);
}
static _Atomic int phase;
static void *handoff(void *unused) {
    (void)unused;check(0x20400,32,1);check(0x20400,32,1);
    atomic_store_explicit(&phase,1,memory_order_release);
    while(atomic_load_explicit(&phase,memory_order_acquire)!=2) sched_yield();
    // A completed pool update must invalidate this thread's earlier handle/size.
    check(0x20400,32,1);return NULL;
}
static void *reader(void *unused) {
    (void)unused;
    for(int i=0;i<50000;i++) {
        VkBuffer buffer;VkDeviceSize offset;uint64_t available;
        assert(vk_buf_range_at(0x20400,32,&buffer,&offset,&available));
        assert(buffer==(VkBuffer)(uintptr_t)0x99 && offset==0x400 && available==0x3c00);
    }
    return NULL;
}
int main(void) {
    pools[0]=(vkpool_t){.lo=0x1000,.hi=0x1100,.buf=(VkBuffer)(uintptr_t)0x10};
    pools[1]=(vkpool_t){.lo=0,.hi=0x10000,.buf=(VkBuffer)(uintptr_t)0x20};
    pools[2]=(vkpool_t){.lo=0x20000,.hi=0x28000,.buf=(VkBuffer)(uintptr_t)0x30};rebuild();
    vk_buffer_cache_config(0);
    for(int i=0;i<4;i++) check(0x1040,16,1);
    assert(reads==4);uint64_t hits,misses,checks,failures;vk_buffer_cache_stats(&hits,&misses,&checks,&failures);assert(!hits && !misses);
    vk_buffer_cache_config(1);unsigned before=reads;
    check(0x1040,16,0);check(0x1040,16,1);check(0x1040,16,1);
    assert(reads==before+1);vk_buffer_cache_stats(&hits,&misses,&checks,&failures);assert(hits==2 && misses==1);
    // Overlap precedence depends on length: the larger pool wins only when the
    // earlier narrow registration cannot contain the whole requested range.
    check(0x1080,0x100,1);check(0x1080,1,1);check(0x1080,0,1);
    check(0x1080,0x100,1);check(0x1080,1,1);
    // Hash collisions and offsets cannot alias, even inside the same pool.
    for(int i=0;i<20;i++) {check(0x1200,16,1);check(0x5200,16,1);}
    check(0,0,1);check(0x10000,0,1);check(0x800000,32,1);
    check(UINT64_MAX,0,1);check(UINT64_MAX-3,8,1);
    for(int i=0;i<16000;i++) {
        uint64_t a=(uint64_t)(i*1543u)%0x30000, n=(uint64_t)(i*17u)%0x500;
        check(a,n,1);check(a,n,1);
    }
    pthread_t thread;assert(!pthread_create(&thread,NULL,handoff,NULL));
    while(atomic_load_explicit(&phase,memory_order_acquire)!=1) sched_yield();
    pthread_rwlock_wrlock(&g_pool_mu);
    pools[2].hi=0x24000;pools[2].buf=(VkBuffer)(uintptr_t)0x99;rebuild();
    pthread_rwlock_unlock(&g_pool_mu);atomic_store_explicit(&phase,2,memory_order_release);
    assert(!pthread_join(thread,NULL));
    pthread_t readers[4];for(int i=0;i<4;i++) assert(!pthread_create(&readers[i],NULL,reader,NULL));
    for(int i=0;i<5000;i++) {
        pthread_rwlock_wrlock(&g_pool_mu);pools[0].hi=0x1080+(i&127);pools[0].buf=(VkBuffer)(uintptr_t)(i+1);rebuild();pthread_rwlock_unlock(&g_pool_mu);
    }
    for(int i=0;i<4;i++) assert(!pthread_join(readers[i],NULL));
    // Removing the lowest slot moves another record into it; cached values from
    // both the deleted record and the moved record must follow the new index.
    check(0x1040,16,1);pools[0]=pools[2];g_npool=2;rebuild();
    check(0x1040,16,1);check(0x20400,32,1);
    vk_buffer_cache_config(0);before=reads;check(0x20400,32,1);check(0x20400,32,1);assert(reads==before+2);
    vk_buffer_cache_config(1);check(0x20400,32,1);
    vk_buffer_cache_config(2);t_buflookup.hits=63;before=reads;check(0x20400,32,1);
    assert(reads==before+1 && t_buflookup.checks==1 && !t_buflookup.failures);
    // Negative control: verification must detect and repair a wrong snapshot.
    unsigned k=((0x20400u>>8)^(0x20400u>>20)^(32u>>4))&63;
    t_buflookup.entry[k].off++;t_buflookup.hits=127;check(0x20400,32,1);
    assert(t_buflookup.failures==1 && t_buflookup.checks==2);check(0x20400,32,1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-buffer-cache-') as temp:
            path, exe = Path(temp) / 'cache.c', Path(temp) / 'cache'
            path.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     '-idirafter', str(headers), '-I', str(ROOT / 'nxnative/dm/android'),
                                     str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
