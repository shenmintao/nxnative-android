"""Architecture preflight after extraction, before build-ID checks or rewriting."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from porter.pipeline import Options, Port
from porter.upstream import BuildError, nsp


class Profile:
    id = '0100AAAA00000000'

    def get(self, key, default=None):
        return 'Test Game' if key == 'name' else default

    def builds(self):
        return {'v0': {'display': '1.0.0'}, 'v65536': {'display': '1.1.0'}}


class Aarch64Preflight(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nx2a-architecture-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = self.root / 'logs'
        self.logs.mkdir()

    def extracted(self, version, flags):
        root = self.root / version
        ex = root / 'ExeFS'
        ex.mkdir(parents=True)
        if flags is not None:
            (ex / 'main.npdm').write_bytes(b'META' + bytes(8) + bytes([flags]) + bytes(0x30))
        return root

    def test_names_bad_version_and_supported_alternative(self):
        d = self.extracted('v0', 0x04)
        (self.logs / 'extract_v0.log').write_text('display version 1.0.0\n')
        with self.assertRaises(BuildError) as e:
            nsp.check_versions_aarch64({'v0': d}, self.logs, Profile(), 'Test Game')
        message = str(e.exception)
        self.assertTrue(message.startswith('Test Game v0 (1.0.0) is a 32-bit ARM program'))
        self.assertIn('AArch64', message)
        self.assertIn('v65536 1.1.0', message)
        self.assertNotIn('\n', message)

    def test_checks_all_versions_and_without_display_log(self):
        dirs = {'v0': self.extracted('v0', 1), 'v65536': self.extracted('v65536', 0)}
        with self.assertRaisesRegex(BuildError, 'Test Game v65536 is a 32-bit ARM program'):
            nsp.check_versions_aarch64(dirs, self.logs, Profile(), 'Test Game')

    def test_64bit_and_missing_or_unknown_metadata_pass(self):
        for flags in (0x01, 0x3f, None):
            d = self.extracted(str(flags), flags)
            nsp.check_aarch64(d / 'ExeFS', 'Test Game')
        p = self.extracted('unknown', None) / 'ExeFS'
        (p / 'main.npdm').write_bytes(b'unknown')
        nsp.check_aarch64(p, 'Test Game')

    def test_truncated_metadata_is_reported(self):
        p = self.extracted('truncated', None) / 'ExeFS'
        (p / 'main.npdm').write_bytes(b'META')
        with self.assertRaisesRegex(BuildError, 'truncated main.npdm'):
            nsp.check_aarch64(p, 'Test Game')

    def test_android_pipeline_stops_before_build_checks_and_aoc(self):
        d = self.extracted('v0', 0x04)
        base = self.root / 'test.xci'
        base.write_bytes(b'placeholder container')
        port = Port(Options(self.root))
        port.g, port.fam, port.have = Profile(), {}, {}
        port.work, port.logs, port.env = self.root, self.logs, {}
        port.versions, port.derived, port.provisional = ['v0'], False, False
        with patch.object(nsp, 'pick_nsps', return_value={'v0': (base, None)}), \
             patch.object(nsp, 'free_gb', return_value=100), \
             patch.object(nsp, 'build_nx_extract', return_value='unused'), \
             patch.object(nsp, 'extract', return_value=d), \
             patch.object(nsp, 'build_mismatch') as builds, \
             patch.object(nsp, 'extract_aoc') as aoc:
            with self.assertRaisesRegex(BuildError, '32-bit ARM program'):
                port.extract()
            builds.assert_not_called()
            aoc.assert_not_called()


if __name__ == '__main__':
    unittest.main()
