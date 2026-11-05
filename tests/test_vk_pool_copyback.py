"""Exercise real pool readback and exact spans without a graphics driver."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PoolCopyback(unittest.TestCase):
    def test_exact_ranges_preserve_cpu_updates_and_cached_pools(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf8')
        start = source.index('static void pools_copy_back(const struct vksub *s) {')
        body = source[start:source.index('\n}', start) + 3]
        prefix = r'''
#define DM_RELEASE 1   /* the debug builds' measurement counters around the copy-back are not part of this contract */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
static int reject_alloc;
static void *test_realloc(void *p, size_t n) { return reject_alloc ? NULL : realloc(p,n); }
#define VK_POOL_WRITES_REALLOC test_realloc
#include "pool_write_ranges.h"
typedef struct { uint64_t lo,hi; uint8_t *shadow; int synced,flags; } vkpool_t;
struct vksub { vk_pool_writes writes; };
static vkpool_t *g_pool[4]; static int g_npool;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
'''
        main = r'''
static uint32_t rng = 0x7912a851;
static uint32_t next(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }
int main(void) {
    struct vksub sub = {0};
    uint8_t cpu[4096+32],gpu[4096],before[sizeof cpu],expected[sizeof cpu];
    memset(cpu,0xa1,sizeof cpu);memset(gpu,0x31,sizeof gpu);
    uint64_t base=(uint64_t)(uintptr_t)(cpu+16);
    vkpool_t pool={base,base+sizeof gpu,gpu,1,4};g_pool[0]=&pool;g_npool=1;
    // An upload may precede unrelated CPU changes while the GPU is running.
    // With no GPU writes, even CPU_CACHED storage must remain untouched.
    memcpy(before,cpu,sizeof cpu);pools_copy_back(&sub);
    if(memcmp(before,cpu,sizeof cpu)) return 77;
    // Neither a 64-byte gap nor >64 distant spans may be overwritten.
    assert(vk_pool_writes_add(&sub.writes,base+32,8));
    assert(vk_pool_writes_add(&sub.writes,base+104,8));
    for(int i=0;i<180;i++) assert(vk_pool_writes_add(&sub.writes,base+512+16*i,3));
    assert(sub.writes.count==182 && sub.writes.capacity>=182);
    memcpy(expected,cpu,sizeof cpu);
    memcpy(expected+16+32,gpu+32,8);memcpy(expected+16+104,gpu+104,8);
    for(int i=0;i<180;i++) memcpy(expected+16+512+16*i,gpu+512+16*i,3);
    for(int flags=0;flags<8;flags++) {
        memcpy(cpu,before,sizeof cpu);pool.flags=flags;pools_copy_back(&sub);
        assert(!memcmp(cpu,expected,sizeof cpu));
    }
    // Uninitialized copies and genuine zero-copy pools never read back here.
    memcpy(cpu,before,sizeof cpu);pool.synced=0;pools_copy_back(&sub);assert(!memcmp(cpu,before,sizeof cpu));
    pool.synced=1;pool.shadow=NULL;pools_copy_back(&sub);assert(!memcmp(cpu,before,sizeof cpu));pool.shadow=gpu;
    // All overlaps, insert orders and adjacency must produce exactly a union.
    free(sub.writes.spans);memset(&sub,0,sizeof sub);
    uint8_t coverage[4096]={0};
    for(int round=0;round<700;round++) {
        unsigned lo=next()%4096,n=next()%80;if(n>4096-lo)n=4096-lo;
        assert(vk_pool_writes_add(&sub.writes,base+lo,n));memset(coverage+lo,1,n);
        memset(cpu,0xa1,sizeof cpu);pools_copy_back(&sub);
        for(unsigned i=0;i<4096;i++) assert(cpu[16+i]==(coverage[i]?0x31:0xa1));
        for(unsigned i=0;i<16;i++) assert(cpu[i]==0xa1 && cpu[4112+i]==0xa1);
        for(size_t i=0;i<sub.writes.count;i++) for(size_t j=0;j<i;j++)
            assert(sub.writes.spans[i].hi<sub.writes.spans[j].lo || sub.writes.spans[j].hi<sub.writes.spans[i].lo);
    }
    // A span can straddle separate pool chunks; only its intersection is read.
    sub.writes.count=0;
    assert(vk_pool_writes_add(&sub.writes,base-8,24));
    assert(vk_pool_writes_add(&sub.writes,base+4090,32));
    memset(cpu,0xa1,sizeof cpu);pools_copy_back(&sub);
    for(unsigned i=0;i<4096;i++) assert(cpu[16+i]==(i<16 || i>=4090 ? 0x31 : 0xa1));
    // Reset keeps capacity for this submission slot; it discards old coverage.
    vk_pool_write_span *saved=sub.writes.spans;size_t capacity=sub.writes.capacity;
    sub.writes.count=0;memset(cpu,0xa1,sizeof cpu);pools_copy_back(&sub);
    assert(sub.writes.spans==saved && sub.writes.capacity==capacity && cpu[16]==0xa1);
    assert(vk_pool_writes_add(&sub.writes,UINT64_MAX,0));
    assert(!vk_pool_writes_add(&sub.writes,UINT64_MAX-4,8));assert(!sub.writes.count);
    free(sub.writes.spans);memset(&sub,0,sizeof sub);
    reject_alloc=1;assert(!vk_pool_writes_add(&sub.writes,base,1));assert(!sub.writes.count && !sub.writes.spans);
    reject_alloc=0;assert(vk_pool_writes_add(&sub.writes,base,1));
    while(sub.writes.count<sub.writes.capacity) assert(vk_pool_writes_add(&sub.writes,base+sub.writes.count*3+3,1));
    size_t count=sub.writes.count;reject_alloc=1;
    assert(!vk_pool_writes_add(&sub.writes,base+10000,1));assert(sub.writes.count==count);
    reject_alloc=0;free(sub.writes.spans);puts("PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            c = path / 'copyback.c'
            exe = path / 'copyback.exe'

            def build_and_run(function):
                c.write_text(prefix + function + main, encoding='utf8')
                subprocess.run([cc, '-std=gnu11', '-O2', '-pthread', '-I', str(ROOT / 'nxnative/dm/android'), str(c), '-o', str(exe)],
                               check=True, capture_output=True, text=True)
                return subprocess.run([str(exe)], capture_output=True, text=True)

            result = build_and_run(body)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)
            # The original whole-cached-pool path must fail this same scenario.
            anchor = 'if (!p->shadow || !p->synced) continue;'
            self.assertEqual(body.count(anchor), 1)
            old = body.replace(anchor, anchor + '\n        if (!(p->flags & 0x2)) { memcpy((void *)(uintptr_t)p->lo, p->shadow, p->hi-p->lo); continue; }')
            self.assertEqual(build_and_run(old).returncode, 77)


if __name__ == '__main__':
    unittest.main()
