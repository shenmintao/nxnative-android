"""Container metadata must drive versions/DLC, not the repacked XCI filename."""
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from porter import containers
from porter.upstream import BuildError, nsp


class CombinedContainers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nxnative_container_')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.keys = self.folder / 'fixture.keys'; self.keys.write_text('synthetic test input')
        self.base = self.folder / 'Combined [010043B013C5C000].xci'; self.base.write_bytes(b'\0' * 16)
        self.info = dict(title_id='010043B013C5C000', base_version=0, updates=[dict(title_id='010043B013C5C800', version=196608)], dlc=[dict(title_id='010043B013C5D001', version=0)])
        self.calls = []
        self.builds = []
        def build(repo, work, logs, env):
            self.builds.append(work)
            output = work / 'tools/nx_extract' / ('nx_extract.exe' if os.name == 'nt' else 'nx_extract')
            output.parent.mkdir(parents=True, exist_ok=True); output.write_bytes(b'synthetic tool')
            return output
        def run(command, log, **kwargs):
            self.calls.append(command)
            Path(log).write_text(json.dumps(self.info))
        for mock in (patch.object(containers.toolchain, 'find_dotnet', return_value=self.folder / 'dotnet'),
                     patch.object(containers.nsp, 'build_nx_extract', side_effect=build), patch.object(containers.nsp, 'run', side_effect=run)):
            mock.start(); self.addCleanup(mock.stop)

    def scan(self): return containers.scan(self.folder, keys=self.keys, cfg={})

    def test_embedded_update_and_dlc_are_selected(self):
        app, family = self.scan()
        self.assertEqual(app, int(self.info['title_id'], 16))
        self.assertEqual(family['updates'], [(196608, self.base)])
        self.assertEqual(family['dlc'], [(int('010043B013C5D001', 16), 0, self.base)])
        self.assertEqual(nsp.nsp_versions(family, {}), {'v0': None, 'v196608': self.base})

    def test_container_scan_reuses_metadata_and_invalidates_changed_keys(self):
        self.scan(); self.scan()
        self.assertEqual((len(self.calls), len(self.builds)), (1, 1))
        stat = self.keys.stat(); os.utime(self.keys, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000))
        self.scan()
        self.assertEqual((len(self.calls), len(self.builds)), (2, 1))

    def test_full_rebased_container_is_not_misreported_as_v0(self):
        self.info.update(base_version=196608, updates=[])
        _, family = self.scan()
        self.assertEqual(nsp.nsp_versions(family, {}), {'v196608': None})

    def test_metadata_title_mismatch_stops_the_port(self):
        self.info['title_id'] = '0100000000000000'
        with self.assertRaisesRegex(BuildError, 'filename/ticket identified'):
            self.scan()

    def test_external_update_is_preserved_alongside_embedded_versions(self):
        external = self.folder / 'Update [010043B013C5C800][v262144].nsp'
        ticket = b'010043B013C5C8000000000000000000.tik\0'
        external.write_bytes(b'PFS0' + struct.pack('<III', 1, len(ticket), 0) + struct.pack('<QQII', 0, 0, 0, 0) + ticket)
        _, family = self.scan()
        self.assertEqual(nsp.nsp_versions(family, {})['v262144'], external)


if __name__ == '__main__': unittest.main()
