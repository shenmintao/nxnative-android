"""The Adreno LRZ rules (cuts, depth-pass splits, depth touches) stay on Qualcomm; other GPUs cut and split freely by default."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXEC = ROOT / 'nxnative/dm/android/gfx_vk_exec.c'


def statement(source, start_text):
    start = source.index(start_text)
    return source[start:source.index(';', start) + 1]


class LrzVendor(unittest.TestCase):
    def setUp(self):
        self.source = EXEC.read_text(encoding='utf-8')

    def test_defaults_and_overrides(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        s = self.source
        start = s.index('static int lrz_vendor(void) {')
        vendor = s[start:s.index('\n', start)]
        reads = '\n'.join(statement(s, t) for t in ('g_x_cutany = (e = getenv("DM_VK_X_CUTANY"))', 'g_lrz_touch = (e = getenv("DM_VK_LRZ_TOUCH"))',
                                                   'g_psplit_depth = (e = getenv("DM_VK_PASS_SPLIT_DEPTH"))'))
        split = statement(s, 'int depth_split_ok =')
        code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static struct { struct { unsigned vendorID; } props; } V;
static int g_x_cutany, g_lrz_touch, g_psplit_depth, auto_result, auto_calls;
static struct { int daspect, pass_open; } S;
static int pass_split_depth_auto(void) { auto_calls++; return auto_result; }
''' + vendor + r'''
static void read_env(void) { const char *e;
''' + reads + r'''
}
static int depth_ok(void) { ''' + split + r''' return depth_split_ok; }
int main(int argc, char **argv) {
    assert(argc == 3); V.props.vendorID = (unsigned)strtoul(argv[1], NULL, 16); const char *mode = argv[2];
    read_env(); S.pass_open = 1;
    if (!strcmp(mode, "default")) {
        int adreno = V.props.vendorID == 0x5143;
        assert(g_x_cutany == !adreno && g_lrz_touch == adreno && g_psplit_depth == -1);
        S.daspect = 0; assert(depth_ok());                              // colour passes: always
        S.daspect = 2; auto_result = 0; assert(depth_ok() == !adreno);  // depth passes: Adreno only through its resource checks
        auto_result = 1; assert(depth_ok());
        if (adreno) { S.pass_open = 2; auto_result = 1; assert(!depth_ok()); }   // (the checks need the pass begun)
        else assert(auto_calls == 0);
    } else if (!strcmp(mode, "forced")) {   // DM_VK_X_CUTANY=0, DM_VK_LRZ_TOUCH=1, DM_VK_PASS_SPLIT_DEPTH=0
        assert(!g_x_cutany && g_lrz_touch && g_psplit_depth == 0);
        S.daspect = 2; auto_result = 1; assert(!depth_ok());
    } else {                                // DM_VK_X_CUTANY=1, DM_VK_LRZ_TOUCH=0, DM_VK_PASS_SPLIT_DEPTH=1
        assert(g_x_cutany && !g_lrz_touch && g_psplit_depth == 1);
        S.daspect = 2; auto_result = 0; assert(depth_ok());
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='lrz-vendor-') as directory:
            folder = Path(directory)
            (folder / 'check.c').write_text(code, encoding='utf-8')
            exe = folder / 'check.exe'
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(folder / 'check.c'), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            base = {k: v for k, v in os.environ.items() if k not in ('DM_VK_X_CUTANY', 'DM_VK_LRZ_TOUCH', 'DM_VK_PASS_SPLIT_DEPTH')}
            off = dict(base, DM_VK_X_CUTANY='0', DM_VK_LRZ_TOUCH='1', DM_VK_PASS_SPLIT_DEPTH='0')
            on = dict(base, DM_VK_X_CUTANY='1', DM_VK_LRZ_TOUCH='0', DM_VK_PASS_SPLIT_DEPTH='1')
            for vendor in ('5143', '13b5', '1010', '1002'):
                for mode, env in (('default', base), ('forced', off), ('relaxed', on)):
                    run = subprocess.run([str(exe), vendor, mode], capture_output=True, text=True, timeout=30, env=env)
                    self.assertEqual(run.returncode, 0, '%s %s: %s' % (vendor, mode, run.stdout + run.stderr))

    def test_lrz_touches_follow_the_switch(self):
        # every depth touch is behind g_lrz_touch, so a non-Adreno default skips them all
        lines = [l for l in self.source.splitlines() if 'lrz_touch(' in l and 'static void lrz_touch' not in l]
        self.assertTrue(lines)
        for line in lines:
            self.assertTrue('g_lrz_touch' in line or 'cut_touch' in line, line)
        self.assertIn('cut_touch = g_lrz_touch &&', self.source)


if __name__ == '__main__':
    unittest.main()
