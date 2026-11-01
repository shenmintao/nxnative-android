from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class UnusedDistanceInterface(unittest.TestCase):
    def test_unused_tail_only_preserves_executable_code_and_used_interfaces(self):
        compiler=shutil.which('cc') or shutil.which('clang')
        if not compiler:self.skipTest('host C compiler required')
        with tempfile.TemporaryDirectory() as temp:
            exe=Path(temp)/'distances.exe'
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'nxnative/dm/android'),str(ROOT/'tests/native/spv_unused_distances.c'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            self.assertIn('PASS',result.stdout)
