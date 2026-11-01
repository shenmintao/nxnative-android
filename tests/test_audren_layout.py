"""Compile the production audio parser against upstream's startup/layout cases."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}', start) + 3]


class AudioLayout(unittest.TestCase):
    def test_headers_startup_recovery_and_stereo_pcm(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        src = (ROOT / 'nxnative/dm/host/hle_audren.c').read_text(encoding='utf-8')
        helpers = src[src.index('static int g_sink_id'):src.index('// A renderer at 32 kHz')]
        code = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
static _Atomic uint64_t g_frames, g_nonsilent;
'''
        frame_max = int(re.search(r'FRAME_MAX = (\d+)', src)[1])
        taps = int(re.search(r'RS_T = (\d+)', src)[1])
        code += f'enum {{ FRAME_MAX = {frame_max}, RS_T = {taps} }};\n'
        code += helpers + function(src, 'static uint32_t up_3_2(') + function(src, 'static int capture(')
        code += (ROOT / 'tests/native/audren_layout.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='nx2a-audren-') as temp:
            c = Path(temp) / 'audren.c'
            exe = Path(temp) / 'audren-check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for sdk in (4, 5, 6, 16, 21):
                for incomplete in (0, 1):
                    with self.subTest(sdk=sdk, incomplete=incomplete):
                        result = subprocess.run([str(exe), str(sdk), str(incomplete)],
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
