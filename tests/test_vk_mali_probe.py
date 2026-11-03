"""Exercise opt-in gates and the real proxy's two conversion directions."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


class MaliProbe(unittest.TestCase):
    def test_proxy_device_gate_roundtrip_and_release(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and Vulkan headers required')
        android = ROOT / 'nxnative/dm/android'
        source = (android / 'gfx_vk_exec.c').read_text(encoding='utf8')
        start = source.index('static struct { vkimg_t image, *original;')
        proxy = source[start:source.index('\n#endif', start)]
        code = (ROOT / 'tests/native/mali_probe.c').read_text(encoding='utf8').replace('/* PROXY */', proxy)
        with tempfile.TemporaryDirectory(prefix='mali-probe-') as directory:
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
                for mode, sampler in (('0', 'log'), ('1', 'nearest'), ('invalid', 'invalid')):
                    result = subprocess.run([str(exe), mode, sampler], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
