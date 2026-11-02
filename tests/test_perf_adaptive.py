"""Exercise the Android adaptive controller's recovery using its actual C code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/perf_android.c'


class AdaptiveFrameRate(unittest.TestCase):
    def test_downshift_veto_allows_median_recovery(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is required')
        src = SOURCE.read_text(encoding='utf-8')
        deadline = src[src.index('static _Atomic uint64_t g_frame_deadline'):src.index('// ---- adaptive frame rate')]
        controller = src[src.index('static const int64_t STEPS[]'):src.index('// hle_nvn.c, the first acquire')]
        controller = controller.replace(
            '__attribute__((weak)) double dm_nvn_present_hold_ms(void) { return 0; }',
            'static double test_present_hold; double dm_nvn_present_hold_ms(void) { return test_present_hold; }')
        hint, shown = controller.index('// The frame\'s work to the CPU hint'), controller.index('// The frames\' time on screen')
        controller = controller[:hint] + controller[shown:]   # without hint_report: the Android hint API (H, g_hint), called only from dm_perf_frame
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
static int setenv(const char *name, const char *value, int overwrite) {
    return !overwrite && getenv(name) ? 0 : _putenv_s(name, value);
}
static int unsetenv(const char *name) { return _putenv_s(name, ""); }
#endif
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
''' + deadline + controller + r'''
static int64_t mixed_work(int i) { return i % 64 < 48 ? 15000000 : 27000000; }
int main(int argc, char **argv) {
    (void)argv;
    int median = argc > 1;
    unsetenv("DM_FPS_PIN"); unsetenv("DM_ADAPTIVE_FPS");
    // A deliberate two-vsync hold is on time; missing that hold is still late.
    g_step = 0; test_present_hold = 33.333333;
    dm_perf_present(33.333333); assert(g_nshown == 1 && !g_late[0]);
    dm_perf_present(51); assert(g_late[1]);
    dm_perf_present(-1); assert(g_late[2]);
    test_present_hold = 0;
    dm_perf_present(25.1); assert(g_late[3]);
    dm_perf_present(0); assert(g_nshown == 4);
    g_nshown = 0; memset(g_late, 0, sizeof g_late);
    if (median) setenv("DM_ADAPT_UP_PCT", "50", 1); else unsetenv("DM_ADAPT_UP_PCT");
    dm_perf_deadline(16666667, 1);
    assert(dm_present_period_adapt(33333333) == 33333333);
    adapt_frame(99000000, 1000000000ll);
    assert(g_step == 0 && g_nwork == 0);  // frame-step owns pacing and keeps its CPU deadline
    assert(atomic_load(&g_frame_deadline) == 16666667);
    dm_perf_deadline(16666667, 0);
    assert(dm_present_period_adapt(16666667) == 16666667);
    g_step = 1; g_last_change = 1000000000ll; g_up_hold = 20000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) g_work_hist[i] = mixed_work(i);
    assert(work_pct(50) == 15000000 && work_pct(90) == 27000000);
    adapt_frame(mixed_work(0), 2000000000ll);  // four-second settling hold
    assert(g_step == 1 && g_ok_frames == 0);
    for (int i = 1; i <= 121; i++) adapt_frame(mixed_work(i), 6000000000ll + i * 25000000ll);
    assert(g_step == 1);  // the longer recovery hold still applies
    assert(g_held_back == 121);
    assert(g_ok_frames == (median ? 121 : 0));
    adapt_frame(mixed_work(122), 21000000000ll);
    assert(g_step == (median ? 0 : 1));  // default p90 policy stays at 40

    g_step = 0; g_last_change = g_last_up = 21000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) { g_work_hist[i] = 19000000; g_late[i] = i < 7; }
    adapt_frame(19000000, 24000000000ll);
    assert(g_step == 0);  // no early downshift within four seconds
    adapt_frame(19000000, 26000000000ll);
    assert(g_step == 1 && g_last_change == 26000000000ll);
    assert(g_up_hold == 40000000000ll);  // a failed recovery doubles the hold
    assert(g_nwork == 0 && g_nshown == 0 && g_ok_frames == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_adaptive_') as tmp:
            c = Path(tmp) / 'check.c'
            exe = Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for args in ([], ['median']):
                with self.subTest(policy=args or 'default'):
                    result = subprocess.run([str(exe), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
