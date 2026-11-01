"""Grow the actual Vulkan registry past its former limit and exercise cached deletion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}', start) + 3]


class PoolRegistry(unittest.TestCase):
    def test_growth_pointer_lifetime_deletion_chunks_and_allocation_failure(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        pool_type = next(line for line in header.splitlines() if '} vkpool_t;' in line)
        prefix = r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static jmp_buf failed;
static int allocation, fail_at, deaths;
static void *test_malloc(size_t n) { return ++allocation == fail_at ? NULL : malloc(n); }
static void *test_calloc(size_t n, size_t s) { return ++allocation == fail_at ? NULL : calloc(n,s); }
static void die(const char *format, ...) { (void)format; deaths++; longjmp(failed,1); }
#define malloc test_malloc
#define calloc test_calloc
#define VKLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LOG VKLOG
static struct { struct { struct { uint32_t maxMemoryAllocationCount; } limits; } props; } V;
static _Atomic int g_ucopy_shadow_pools;
static unsigned drained, copied, deferred, released;
static void ucopy_drain(void) { drained++; }
static void pool_copy_back(vkpool_t *p);
static void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m) { assert(b && m); deferred++; }
static void AHardwareBuffer_release(void *b) { assert(b); released++; }
'''
        # The forward declaration needs the production pool type first.
        prefix = prefix.replace('static void pool_copy_back(vkpool_t *p);', pool_type + '\nstatic void pool_copy_back(vkpool_t *p) { assert(p->zero_copy); copied++; }')
        registry = src[src.index('#include "pool_ranges.h"'):src.index('static int recv_fds(')]
        code = prefix + registry + '\n' + '\n'.join(function(src, name) for name in (
            'void mtl_pool_del(', 'vkpool_t *vk_pool_of(', 'int vk_buf_range_at('))
        code += r'''
static uint64_t serial;
static vkpool_t *add(uint64_t lo, uint64_t hi, uint64_t base, uint64_t end) {
    pthread_rwlock_wrlock(&g_pool_mu); pool_reserve();
    vkpool_t *p=g_pool[g_npool]; memset(p,0,sizeof *p);
    p->lo=lo; p->hi=hi; p->base=base; p->end=end; p->serial=++serial;
    p->buf=(VkBuffer)(uintptr_t)serial; p->mem=(VkDeviceMemory)(uintptr_t)serial;
    p->ahb=(void *)(uintptr_t)serial; p->zero_copy=1;
    vk_ranges_add(g_pool_ranges,g_npool,lo,hi,g_npool);
    g_npool++; g_pool_adds++; __atomic_fetch_add(&g_pool_gen,1,__ATOMIC_RELEASE); pool_log("add",p);
    pthread_rwlock_unlock(&g_pool_mu); return p;
}
static uint32_t rng=0x72134961;
static uint32_t next(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void check(uint64_t a) {
    int want=-1; for(int i=0;i<g_npool;i++) if(a>=g_pool[i]->lo && a+1<=g_pool[i]->hi) {want=i;break;}
    vkpool_t *have=vk_pool_of(a,1); assert(have==(want<0 ? NULL : g_pool[want]));
    VkBuffer b=VK_NULL_HANDLE; VkDeviceSize off=0; uint64_t available=0;
    assert(vk_buf_range_at(a,1,&b,&off,&available)==(want>=0));
    if(want>=0) {assert(b==have->buf && off==a-have->lo && available==have->hi-a);}
}
static void fail_growth(int point) {
    allocation=0;fail_at=point;
    if(!setjmp(failed)) {pool_reserve();abort();}
}
int main(void) {
    // Failed growth must leave every live record, pointer and range usable.
    for(int i=1;i<=3;i++) {fail_growth(i);assert(!g_pool_cap && !g_pool);}
    fail_at=0; pool_reserve(); assert(g_pool_cap==64);
    for(int i=0;i<64;i++) add(0x100000+(uint64_t)i*0x1000,0x100080+(uint64_t)i*0x1000,0x100000+(uint64_t)i*0x1000,0x100080+(uint64_t)i*0x1000);
    vkpool_t *first=g_pool[0], *last=g_pool[63]; vk_pool_range *ranges=g_pool_ranges;
    for(int i=1;i<=3;i++) {fail_growth(i);assert(g_pool_cap==64 && g_pool[0]==first && g_pool[63]==last && g_pool_ranges==ranges);check(first->lo);}
    fail_at=0;
    // Includes the 4,097th live pool that aborted the user's run. Cached pointers
    // remain valid across several table reallocations, without another lookup.
    for(int i=64;i<12000;i++) add(0x100000+(uint64_t)i*0x1000,0x100080+(uint64_t)i*0x1000,0x100000+(uint64_t)i*0x1000,0x100080+(uint64_t)i*0x1000);
    assert(g_pool_cap==16384 && g_pool[0]==first && g_pool[63]==last && first->serial==1 && last->serial==64);
    for(int i=0;i<12000;i++) check(0x100000+(uint64_t)i*0x1000);
    // Swap-removal invalidates both recent cached slots, then reuses storage.
    uint64_t moved_lo=g_pool[g_npool-1]->lo, moved_serial=g_pool[g_npool-1]->serial;
    check(moved_lo);check(first->lo);mtl_pool_del((void *)(uintptr_t)first->base);
    assert(g_pool[0]==first && first->serial==moved_serial);check(moved_lo);check(0x100000);
    for(int step=0;step<8000;step++) {
        int i=(int)(next()%(uint32_t)g_npool);uint64_t old=g_pool[i]->base;
        check(old);mtl_pool_del((void *)(uintptr_t)old);check(old);
        uint64_t a=0x10000000+(uint64_t)step*0x1000;add(a,a+0x800,a,a+0x800);
        check(a);check(g_pool[next()%(uint32_t)g_npool]->lo);
    }
    // All chunks of a single guest pool are removed and each GPU handle is
    // deferred exactly once; copying back and AHB release are retained.
    uint64_t base=0x90000000; for(int i=0;i<3;i++) add(base+i*0x1000,base+(i+1)*0x1000,base,base+0x3000);
    int before=g_npool;unsigned done=deferred;
    assert(vk_pool_of(base+0xff0,0x20)==NULL);check(base+0x1000);
    mtl_pool_del((void *)(uintptr_t)base);assert(g_npool==before-3 && deferred==done+3);check(base+0x1000);
    mtl_pool_del((void *)(uintptr_t)base);assert(deferred==done+3);
    while(g_npool) mtl_pool_del((void *)(uintptr_t)g_pool[g_npool-1]->base);
    assert(g_pool_adds==g_pool_dels && copied==deferred && released==deferred && deaths==6 && drained>deferred-3);
    puts("12,000 live pools, stable growth, 8,000 reuse cycles, cached swap-removal, multi-chunk release and allocation failures passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-pool-registry-') as temp:
            c = Path(temp) / 'registry.c'
            exe = Path(temp) / 'registry-check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'nxnative/dm/android'), '-idirafter', str(headers),
                                     str(c), '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
