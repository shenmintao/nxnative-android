"""Production AAudio control/callback lifecycle against a threaded fake device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AAudioReconnect(unittest.TestCase):
    def test_callback_thread_reopen_repeat_backoff_hold_and_dispose(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('host C compiler required')
        with tempfile.TemporaryDirectory(prefix='aaudio-reconnect-') as temp:
            exe = Path(temp) / 'audio.exe'
            r = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                                '-I' + str(ROOT / 'tests/native/aaudio_mock'),
                                '-I' + str(ROOT / 'nxnative/dm/android/compat/include'),
                                str(ROOT / 'tests/native/aaudio_reconnect.c'), '-pthread', '-o', str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('PASS', r.stdout)
            self.assertIn('[WARN] AAudio', r.stderr)
            self.assertIn('reconnect failed', r.stderr)
            self.assertIn('retry in 25ms', r.stderr)
            self.assertIn('retry in 50ms', r.stderr)
            self.assertIn('callback gap exceeds 100ms', r.stderr)
