"""Storage writes are derived from shader instructions, not optional rw metadata."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StorageWrites(unittest.TestCase):
    def test_stores_atomics_images_pointer_aliases_and_unknown_provenance(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'writes.exe'
            compiled = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                       '-I', str(ROOT / 'nxnative/dm/android'), str(ROOT / 'tests/native/spv_buffer_writes.c'), '-o', str(exe)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
