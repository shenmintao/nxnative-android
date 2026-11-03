"""The investigation reference is opt-in, attachment-specific and absent in release."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


class FloatBlendProbe(unittest.TestCase):
    def test_default_log_reference_and_release_gates(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and Vulkan headers required')
        android = ROOT / 'nxnative/dm/android'
        source = (android / 'gfx_vk_exec.c').read_text(encoding='utf8')
        a = source.index('typedef struct {\n    uint64_t key[3]')
        key = source[a:source.index('} pkey;', a) + len('} pkey;')]
        a = source.index('static void fp32_probe_pipeline(')
        function = source[a:source.index('\n}', a) + 3]
        code = (ROOT / 'tests/native/fp32_blend_probe.c').read_text(encoding='utf8')
        code = code.replace('/* KEY */', key).replace('/* FUNCTION */', function)
        with tempfile.TemporaryDirectory(prefix='fp32-probe-') as directory:
            folder = Path(directory)
            c = folder / 'check.c'
            c.write_text(code, encoding='utf8')
            for release in (False, True):
                exe = folder / ('release.exe' if release else 'debug.exe')
                args = [cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-misleading-indentation',
                        '-idirafter', str(headers), '-I' + str(android), str(c), '-o', str(exe)]
                if release:
                    args.insert(1, '-DDM_RELEASE=1')
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for mode in ('', 'log', 'C', 'A'):
                    result = subprocess.run([str(exe), mode], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
