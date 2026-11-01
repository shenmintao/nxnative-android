"""Exercise production query bounds, finite capture and concurrent reporting."""
from pathlib import Path
import csv
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class PassTiming(unittest.TestCase):
    def test_query_bounds_capture_limits_and_report_accounting(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        block = src[src.index('enum { PASSQ'):src.index('// one submission at a time:')]
        code = r'''
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
static int setenv(const char *name, const char *value, int overwrite) {
    return !overwrite && getenv(name) ? 0 : _putenv_s(name, value);
}
static int unsetenv(const char *name) { return _putenv_s(name, ""); }
#endif
#define NSLOT 6
static int g_cur;
static unsigned stamps;
static uint64_t reported;
static struct { struct { struct { float timestampPeriod; } limits; } props; } V;
uint64_t g_nvn_discards[2];
static VkCommandBuffer g_slot_main_cb(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void stamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t);
#define vkCmdWriteTimestamp stamp
#define VKLOG(...) do { reported += g_pass_timed; if (0) fprintf(stderr, __VA_ARGS__); } while (0)
''' + block + r'''
static unsigned seen[NSLOT * PASSQ * 2];
static void stamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    assert(cb && pool && query < NSLOT * PASSQ * 2);
    assert(query / (PASSQ * 2) == (unsigned)g_cur);
    if (stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) assert(!(query & 1));
    else { assert(stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && (query & 1)); assert(seen[query-1]); }
    seen[query]++; stamps++;
}
static void *samples(void *unused) {
    (void)unused; pass_meta m = {640, 360, 37, 1, 0}; uint64_t t[] = {100, 105};
    for (int i = 0; i < 10000; ++i) pass_accumulate(&m, t, 1);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2); V.props.limits.timestampPeriod = 2;
    g_pq = (VkQueryPool)(uintptr_t)1; g_pq_on = 1;
    for (g_cur = 0; g_cur < NSLOT; ++g_cur) {
        g_npass[g_cur] = 0;
        unsigned ordinal = (unsigned)g_fpass;
        for (int i = 0; i < PASSQ+5; ++i) {
            vk_pass_ts(0, 0, 0, 0, 0); vk_pass_ts(1, 640, 360, 37, 1);
            if (i < PASSQ) assert(g_pmeta[g_cur][i].fidx == ordinal + (unsigned)i);
        }
        assert(g_npass[g_cur] == PASSQ);
    }
    assert(stamps == NSLOT * PASSQ * 2 && g_fpass == NSLOT * (PASSQ+5));
    assert(atomic_load(&g_pass_omitted) == NSLOT * 5);
    unsigned before = stamps; g_cur = -1; vk_pass_ts(0,0,0,0,0); vk_pass_ts(1,1,1,1,1);
    assert(stamps == before);
    g_cur = 0; g_pq_on = 0; vk_pass_ts(0,0,0,0,0); assert(stamps == before);

    pass_meta m[4] = {{1,1,37,1,2},{1,1,37,1,FPASSQ-1},{1,1,37,1,3},{1,1,37,1,FPASSQ+4}};
    uint64_t ticks[] = {10,20,30,30,50,49,60,65}; pass_accumulate(m, ticks, 4);
    assert(g_pass_timed == 3 && g_pass_invalid == 1 && g_pass_unranked == 1);
    assert(g_pcnt[2] == 1 && fabs(g_pacc[2] - .00002) < 1e-12 && g_pcnt[FPASSQ-1] == 1);
    vk_pass_report(); assert(g_pass_timed == 0 && g_pass_invalid == 0 && g_pmax == 0);
    reported = 0; pthread_t thread; assert(!pthread_create(&thread, NULL, samples, NULL));
    for (int i = 0; i < 100; ++i) vk_pass_report();
    pthread_join(thread, NULL); vk_pass_report(); assert(reported == 10000);

    g_pq_on = 2; unsetenv("DM_VK_PASS_CAPTURE"); vk_pass_frame(); assert(!pass_times_on());
    setenv("DM_VK_PASS_CAPTURE", argv[1], 1); setenv("DM_VK_PASS_CAPTURE_FRAMES", "4", 1);
    vk_pass_frame();
    for (unsigned i = 0; i < 4; ++i) {
        assert(pass_times_on()); g_cur = (int)i; g_npass[g_cur] = 0;
        vk_pass_ts(0,0,0,0,0); vk_pass_ts(1,640,360,37,1u | (640u<<16));
        uint64_t t[] = {10000+i*20,10005+i*20};
        pass_capture(100+i, 1000+i, g_pmeta[g_cur], t, 1); vk_pass_frame();
    }
    assert(!pass_times_on());
    pass_capture(104,1004,g_pmeta[3],ticks,1);
    FILE *check = fopen(argv[1], "r"); assert(check); char row[2048]; int lines = 0;
    while (fgets(row,sizeof row,check)) lines++;
    fclose(check); assert(lines == 5);
    char next[1024]; snprintf(next,sizeof next,"%s.next",argv[1]);
    setenv("DM_VK_PASS_CAPTURE",next,1); setenv("DM_VK_PASS_CAPTURE_FRAMES","999",1);
    for (int i = 0; i < 16; ++i) { vk_pass_frame(); assert(pass_times_on()); }
    vk_pass_frame(); assert(!pass_times_on());
    setenv("DM_VK_PASS_CAPTURE_FRAMES","0",1); assert(pass_capture_frames() == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-pass-timing-') as temp:
            c = Path(temp) / 'pass.c'
            exe = Path(temp) / 'pass-check'
            capture = Path(temp) / 'capture.tsv'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe), str(capture)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with capture.open(encoding='utf-8') as stream:
                rows = list(csv.DictReader(stream, delimiter='\t'))
            self.assertEqual([int(row['frame']) for row in rows], [100, 101, 102, 103])
            self.assertTrue(all(int(row['pass']) == 0 and int(row['draws']) == 1 and int(row['bbox_width']) == 640
                                and int(row['end_ticks']) - int(row['begin_ticks']) == 5 and float(row['tick_ns']) == 2
                                for row in rows))


if __name__ == '__main__':
    unittest.main()
