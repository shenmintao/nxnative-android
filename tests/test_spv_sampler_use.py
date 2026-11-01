"""Comparison follows the shader's actual image operation and descriptor root."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SamplerUse(unittest.TestCase):
    def test_sampling_operations_and_descriptor_provenance(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='spv-sampler-') as directory:
            exe = Path(directory) / 'sampling.exe'
            compiled = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                       '-I', str(ROOT / 'nxnative/dm/android'),
                                       str(ROOT / 'tests/native/spv_sampler_use.c'), '-o', str(exe)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
