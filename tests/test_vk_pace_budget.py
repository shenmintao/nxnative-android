"""Exercise the scheduling estimate and one-shot replay stall without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class VulkanPaceBudget(unittest.TestCase):
    def run_case(self, case):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and NDK Vulkan headers required')
        core = (ANDROID / 'gfx_vk_core.c').read_text(encoding='utf8')
        replay = (ANDROID / 'gfx_vk_exec.c').read_text(encoding='utf8')
        present = (ANDROID / 'gfx_vk_present.c').read_text(encoding='utf8')
        spans = core[core.index('static uint64_t g_cur_frame,'):core.index('// Internal completion protects')]
        stall = '\n'.join(function(core, name) for name in ('static void split_collect(', 'int vk_debug_stall_ready('))
        stall += '\nstatic long g_x_stall_ms;\n' + function(replay, 'static void replay_stall_read(')
        stall += '\n' + function(replay, 'static void replay_stall(')
        pace = present[present.index('static _Atomic uint64_t g_refresh_ns;'):present.index('static void present_window(')]
        code = (ROOT / 'tests/native/vk_pace_budget.c').read_text(encoding='utf8')
        code = code.replace('/* FRAME SPANS */', spans).replace('/* STALL */', stall).replace('/* PACER */', pace)
        with tempfile.TemporaryDirectory(prefix='vk-pace-budget-') as temp:
            src, exe = Path(temp) / 'check.c', Path(temp) / 'check.exe'
            src.write_text(code, encoding='utf8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-missing-field-initializers', f'-DCASE={case}',
                '-idirafter', str(headers), '-I', str(ANDROID), str(src), '-o', str(exe)],
                capture_output=True, text=True, timeout=40)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_estimator_steady_outliers_consecutive_stalls_sustained_load_and_unknowns(self):
        self.run_case(0)

    def test_ring_keeps_raw_latest_lookup_and_bounds_known_history(self):
        self.run_case(1)

    def test_stall_consumes_once_rearms_and_waits_for_split_handoff(self):
        self.run_case(2)

    def test_pacer_default_live_toggle_buffered_bypass_and_counters(self):
        self.run_case(3)

    def test_injection_is_debug_only_and_ctl_reapplies_identical_values(self):
        replay = (ANDROID / 'gfx_vk_exec.c').read_text(encoding='utf8')
        core = (ANDROID / 'gfx_vk_core.c').read_text(encoding='utf8')
        self.assertIn('#ifndef DM_RELEASE\n' + function(replay, 'static void replay_stall(') + '#endif', replay)
        self.assertIn('#ifndef DM_RELEASE\n    replay_stall();\n#endif', replay)
        self.assertIn('#ifndef DM_RELEASE\nstatic long g_x_stall_ms;\n' + function(replay, 'static void replay_stall_read(') + '#endif', replay)
        self.assertIn('#ifndef DM_RELEASE\n    replay_stall_read();\n#endif', function(replay, 'static void x_read('))
        self.assertNotIn('getenv', function(replay, 'static void replay_stall('))
        self.assertIn('#ifndef DM_RELEASE\n' + function(core, 'int vk_debug_stall_ready(') + '#endif', core)
        present = (ANDROID / 'gfx_vk_present.c').read_text(encoding='utf8')
        self.assertIn('#ifndef DM_RELEASE\nuint64_t vk_debug_next_present(void) { return g_presents + 1; }\n#endif', present)
        ctl = (ROOT / 'nxnative/dm/host/hle_hid.c').read_text(encoding='utf8')
        self.assertIn('setenv(line + 4, eq + 1, 1)', ctl)
        build = (ANDROID / 'build.py').read_text(encoding='utf8')
        self.assertIn("['-DDM_RELEASE=1'] if a.release else []", build)

    def test_raw_spans_still_feed_frame_cost_and_timing(self):
        core = (ANDROID / 'gfx_vk_core.c').read_text(encoding='utf8')
        raw = function(core, 'double vk_gpu_ms_recent(')
        self.assertIn('k = 2; k < 8 && k <= f', raw)
        self.assertIn('double g = mtl_gpu_ms(f - k); if (g >= 0) return g;', raw)
        self.assertNotIn('budget', raw)
        present = (ANDROID / 'gfx_vk_present.c').read_text(encoding='utf8')
        cost = present[present.index('uint64_t mtl_present('):present.index('dm_perf_frame(period);')]
        self.assertIn('double g = vk_gpu_ms_recent();', cost)
        self.assertIn('dm_perf_frame_cost((int64_t)rp, gn);', cost)
        self.assertNotIn('vk_gpu_ms_budget', cost)
        self.assertRegex(core, r'g_frame_gpu\[k\] \+= ns;')
        self.assertIn('g_frame_gpu[f % 64] / 1e6 : -1', core)


if __name__ == '__main__':
    unittest.main()
