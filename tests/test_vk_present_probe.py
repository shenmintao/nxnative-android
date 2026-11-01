"""Compile the actual optional presentation-query lifecycle and timestamp decoder."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


ROOT = Path(__file__).resolve().parents[1]


class PresentProbe(unittest.TestCase):
    def test_availability_wraparound_slots_and_opt_in(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_present.c').read_text(encoding='utf-8')
        code = src[src.index('enum { PRES_PROBE_POINTS'):src.index('// Where the game\'s picture lies')]
        code = code.replace('static uint64_t mono_ns(void);', '')
        prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { VkDevice dev; VkPhysicalDevice pd; uint32_t qf; VkPhysicalDeviceProperties props; } V;
static VkExtent2D g_sext = {100,100};
static unsigned creates, destroys, reads, resets, writes, logs;
static unsigned valid_bits = 36;
static int fail;
static VkResult result;
static uint64_t fixture[5][2];
static const char *env;
static const char *probe_getenv(const char *key) { assert(!strcmp(key,"DM_VK_PRESENT_TIMES")); return env; }
#define getenv probe_getenv
#define VKLOG(...) do { logs++; if (0) printf(__VA_ARGS__); } while (0)
static void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    (void)pd;
    if (p) { assert(*n >= 1); memset(p,0,sizeof *p); p->timestampValidBits = valid_bits; }
    *n = 1;
}
static VkResult vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)d; (void)a; creates++;
    assert(ci->queryType == VK_QUERY_TYPE_TIMESTAMP && ci->queryCount == 40);
    if (fail) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkQueryPool)(uintptr_t)1; return VK_SUCCESS;
}
static void vkDestroyQueryPool(VkDevice d, VkQueryPool pool, const VkAllocationCallbacks *a) {
    (void)d; (void)a; assert(pool == (VkQueryPool)(uintptr_t)1); destroys++;
}
static VkResult vkGetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t n, size_t bytes, void *out, VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)d; assert(pool == (VkQueryPool)(uintptr_t)1 && first % 5 == 0 && first <= 35 && n == 5);
    assert(bytes == sizeof fixture && stride == sizeof fixture[0]);
    assert(flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    reads++; memcpy(out,fixture,sizeof fixture); return result;
}
static void vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n) {
    (void)cb; assert(pool == (VkQueryPool)(uintptr_t)1 && first % 5 == 0 && first <= 35 && n == 5); resets++;
}
static void vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t index) {
    (void)cb; assert(pool == (VkQueryPool)(uintptr_t)1 && index < 40);
    assert(stage == (index % 5 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)); writes++;
}
'''
        main = r'''
static void samples(void) {
    for (int i = 0; i < 5; ++i) { fixture[i][0] = 1000 + (uint64_t)i * 100; fixture[i][1] = 1; }
}
int main(void) {
    V.props.limits.timestampPeriod = 1;
    pres_probe_init(); assert(g_pres_probe_on == 0 && !g_pres_probe_pool && creates == 0);
    env = "1"; g_pres_probe_on = -1;
    pres_probe_init(); assert(!g_pres_probe_pool && !creates);
    V.props.limits.timestampComputeAndGraphics = 1; valid_bits = 0;
    pres_probe_init(); assert(!g_pres_probe_pool && !creates);
    valid_bits = 36; fail = 1;
    pres_probe_init(); assert(!g_pres_probe_pool && creates == 1);
    fail = 0; pres_probe_init(); assert(g_pres_probe_pool && creates == 2);
    pres_probe_init(); assert(creates == 2);
    samples(); double ms[4] = {42,42,42,42};
    assert(pres_probe_durations(fixture,1,36,ms));
    for (int i = 0; i < 4; ++i) assert(fabs(ms[i] - .0001) < 1e-12);
    const double bad_periods[] = {0,-1,NAN,INFINITY};
    for (unsigned i = 0; i < sizeof bad_periods / sizeof *bad_periods; ++i)
        assert(!pres_probe_durations(fixture,bad_periods[i],36,ms));
    assert(!pres_probe_durations(fixture,1,0,ms)); assert(!pres_probe_durations(fixture,1,65,ms));
    for (int i = 0; i < 5; ++i) {
        fixture[i][1] = 0;
        assert(!pres_probe_durations(fixture,1,36,ms)); fixture[i][1] = 1;
    }
    uint64_t wrapped[] = {UINT64_MAX-2,UINT64_MAX,2,4,6};
    for (int i = 0; i < 5; ++i) fixture[i][0] = wrapped[i];
    assert(pres_probe_durations(fixture,1000000,64,ms));
    assert(ms[0] == 2 && ms[1] == 3 && ms[2] == 2 && ms[3] == 2);
    uint64_t short_counter[] = {250,254,2,3,8};
    for (int i = 0; i < 5; ++i) fixture[i][0] = short_counter[i] | (UINT64_C(123) << 8);
    assert(pres_probe_durations(fixture,1000000,8,ms));
    assert(ms[0] == 4 && ms[1] == 4 && ms[2] == 1 && ms[3] == 5);
    samples(); fixture[2][0] = 1;
    for (int i = 0; i < 4; ++i) ms[i] = 42;
    assert(!pres_probe_durations(fixture,1,64,ms));
    for (int i = 0; i < 4; ++i) assert(ms[i] == 42);
    samples(); pres_probe_read(3); assert(!reads);
    g_pres_probe_pending[3] = 1; pres_probe_read(3);
    assert(reads == 1 && g_pres_probe_gn == 1 && !g_pres_probe_pending[3]);
    pres_probe_read(3); assert(reads == 1);
    result = VK_NOT_READY; g_pres_probe_pending[0] = 1; pres_probe_read(0);
    assert(g_pres_probe_bad == 1 && g_pres_probe_gn == 1);
    result = VK_SUCCESS; fixture[4][1] = 0; g_pres_probe_pending[7] = 1; pres_probe_read(7);
    assert(g_pres_probe_bad == 2 && g_pres_probe_gn == 1);
    samples();
    for (unsigned i = 1; i < 600; ++i) { g_pres_probe_pending[i%8] = 1; pres_probe_read(i%8); }
    assert(!g_pres_probe_gn && !g_pres_probe_bad && g_pres_probe_gpu[0] == 0);
    pres_probe_mark(VK_NULL_HANDLE,2,0,0); assert(!resets && !writes);
    for (unsigned i = 0; i < 5; ++i) pres_probe_mark(VK_NULL_HANDLE,2,i,1);
    assert(resets == 1 && writes == 5);
    uint64_t ns[8] = {10,20,30,40,50,60,70,80};
    pres_probe_cpu(ns,0); assert(!g_pres_probe_cn);
    pres_probe_cpu(ns,1); assert(g_pres_probe_cn == 1 && g_pres_probe_cpu[0] == .00001);
    g_pres_probe_pending[0] = g_pres_probe_pending[7] = 1;
    unsigned before = reads; pres_probe_drop();
    assert(!g_pres_probe_pool && destroys == 1 && reads == before + 2);
    assert(!g_pres_probe_gn && !g_pres_probe_cn && !g_pres_probe_bad);
    for (int i = 0; i < 8; ++i) assert(!g_pres_probe_pending[i]);
    pres_probe_drop(); assert(destroys == 1);
    pres_probe_init(); assert(g_pres_probe_pool && creates == 3);
    pres_probe_drop();
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-present-probe-') as tmp:
            c = Path(tmp) / 'check.c'
            exe = Path(tmp) / 'check'
            c.write_text(prefix + code + main, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-idirafter', str(headers), str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
