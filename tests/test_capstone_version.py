"""Reject incompatible disassembler APIs before extraction or ARM64-specific imports."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from porter import pipeline, toolchain
from porter.upstream import BuildError


class CapstoneVersion(unittest.TestCase):
    def checks(self, major):
        capstone = SimpleNamespace(__version__=f'{major}.0.0-test', cs_version=lambda: (major, 0, 0))
        with patch.dict(sys.modules, capstone=capstone), \
             patch.object(toolchain, '_importable', return_value=True), \
             patch.object(toolchain, 'find_ndk', return_value=(Path('.'), toolchain.NDK_REVISION)), \
             patch.object(toolchain, 'find_dotnet', return_value=Path('dotnet')), \
             patch.object(toolchain, 'dotnet_sdks', return_value=['10.0.100']), \
             patch.object(toolchain, 'find_adb', return_value=None):
            return toolchain.checks(cfg={})

    def test_setup_checks_the_loaded_api(self):
        for major in (4, 5, 6):
            with self.subTest(major=major):
                row = next(r for r in self.checks(major) if r.name == 'Python modules')
                self.assertEqual(row.ok, major == 5)
                if major != 5:
                    self.assertIn('requires Capstone 5.x', row.detail)
                    self.assertIn('capstone>=5,<6', row.fix)
                    self.assertIn(sys.executable, row.fix)
                    if os.name == 'nt': self.assertIn('"capstone>=5,<6"', row.fix)

    def test_incompatible_api_stops_before_game_processing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            keys = root / 'prod.keys'; keys.touch()
            opts = pipeline.Options(folder=root, keys=keys)
            port = pipeline.Port(opts)
            rows = self.checks(6)
            for row in rows:
                if row.name != 'Python modules': row.ok = True
            with patch.object(toolchain, 'checks', return_value=rows), \
                 patch.object(port, 'inputs') as inputs, patch.object(port, 'extract') as extract:
                with self.assertRaisesRegex(BuildError, 'requires Capstone 5.x'): port.run()
                inputs.assert_not_called()
                extract.assert_not_called()

    def test_rewriter_rejects_api_before_renamed_arm64_import(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            # This module deliberately has no capstone.arm64: the version guard must run first.
            (root / 'capstone.py').write_text("__version__ = '6.0.0-pre'\ndef cs_version(): return (6, 0, 0)\n")
            rewriter = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools/patch_nso.py'
            env = dict(os.environ, PYTHONPATH=str(root), PYTHONUTF8='1')
            result = subprocess.run([sys.executable, str(rewriter), str(root / 'missing.nso'), str(root), 'main'],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('[RED] Capstone 6.0.0-pre is unsupported', result.stderr)
            self.assertNotIn('Traceback', result.stderr)
            self.assertFalse((root / 'main.patched.bin').exists())


if __name__ == '__main__': unittest.main()
