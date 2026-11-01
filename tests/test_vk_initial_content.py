"""First-use NVN storage uploads: actual policy, deswizzle and Vulkan copy records."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^static (?:int|uint64_t) ' + name + r'\([^\n]*\) \{', source, re.MULTILINE)
    start = match.start()
    end = source.index('\n}', start) + 3
    return source[start:end]


class InitialTextureContent(unittest.TestCase):
    def test_published_storage_subresources_and_gpu_ownership(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        functions = '\n'.join(function(source, name) for name in ('linear_pitch', 'pkg_upload_', 'pkg_upload', 'initial_content'))
        code = (ROOT / 'tests/native/vk_initial_content.c').read_text(encoding='utf-8')
        code = code.replace('/* PRODUCTION_FUNCTIONS */', functions)
        with tempfile.TemporaryDirectory(prefix='vk-initial-content-') as temp:
            path, exe = Path(temp) / 'initial.c', Path(temp) / 'initial'
            path.write_text(code, encoding='utf-8')
            result = subprocess.run([
                cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-idirafter', str(headers),
                '-I', str(ROOT / 'nxnative/dm/gfx/nvn/include'),
                '-I', str(ROOT / 'nxnative/dm/gfx/nvn'),
                '-I', str(ROOT / 'nxnative/dm/gfx/tex'), str(path),
                str(ROOT / 'nxnative/dm/gfx/nvn/nvn_tex.c'),
                str(ROOT / 'nxnative/dm/gfx/tex/nvn_deswizzle.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
