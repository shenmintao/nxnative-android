"""Original-driver texture shapes plus bounded mip/layer swizzle round trips."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TextureLayout(unittest.TestCase):
    def test_original_driver_shapes_and_storage_bounds(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='nvn-layout-') as temp:
            exe = Path(temp) / 'layout-check'
            result = subprocess.run([
                cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'nxnative/dm/gfx/nvn'),
                '-I', str(ROOT / 'nxnative/dm/gfx/tex'),
                str(ROOT / 'tests/native/nvn_texture_layout.c'),
                str(ROOT / 'nxnative/dm/gfx/nvn/nvn_tex.c'),
                str(ROOT / 'nxnative/dm/gfx/tex/nvn_deswizzle.c'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
