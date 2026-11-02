"""Only one recurring non-presenting sync caller may acquire the opt-in affinity."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SyncThreadPolicy(unittest.TestCase):
    def test_single_caller_selection_and_disabled_or_unavailable_policy(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('host C compiler is required')
        source = (ROOT / 'nxnative/dm/android/perf_android.c').read_text(encoding='utf8')
        begin = source.index('static _Atomic int g_perf_frame_tid, g_sync_pin_tid;')
        end = source.index('\nstatic int64_t g_work_start;', begin)
        prefix, suffix = (ROOT / 'tests/native/sync_thread_policy.c').read_text(encoding='utf8').split('/* PRODUCTION POLICY */')
        with tempfile.TemporaryDirectory(prefix='sync-thread-') as temp:
            directory = Path(temp)
            path, exe = directory / 'check.c', directory / 'check.exe'
            path.write_text(prefix + source[begin:end] + suffix, encoding='utf8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-pthread', str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for mode in ('off', 'prime', 'quad', 'uniform', 'unknown', 'refused'):
                result = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, mode + ': ' + result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
