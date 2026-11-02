"""Check selected-file imports with synthetic PFS0 containers, not game dumps."""
import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from porter import pipeline, sources
from porter.cli import main
from porter.upstream import BuildError


def write_nsp(path, title_id):
    ticket = f'{title_id}0000000000000000.tik'.encode() + b'\0'
    path.write_bytes(b'PFS0' + struct.pack('<III', 1, len(ticket), 0)
                     + struct.pack('<QQII', 0, 0, 0, 0) + ticket)
    return path


class SourceImports(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nxnative_import_')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def test_selected_file_keeps_its_updates_and_dlc_only(self):
        base = write_nsp(self.folder / 'selected.NSP', '010043B013C5C000')
        write_nsp(self.folder / 'update [v196608].nsp', '010043B013C5C800')
        write_nsp(self.folder / 'addon.nsp', '010043B013C5D001')
        write_nsp(self.folder / 'other.nsp', '0100A00019DE0000')
        info = pipeline.inspect(base)
        self.assertEqual(info['title_id'], '010043B013C5C000')
        self.assertEqual(info['base'], base.name)
        self.assertEqual(info['updates'], [{'version': 'v196608', 'file': 'update [v196608].nsp'}])
        self.assertEqual([row['title_id'] for row in info['dlc']], ['010043B013C5D001'])
        self.assertTrue(info['supported'])
        self.assertEqual(info['pack'], 'v196608')
        # The original folder workflow still rejects an ambiguous collection.
        with self.assertRaisesRegex(BuildError, 'more than one game'):
            pipeline.inspect(self.folder)

    def test_selecting_update_finds_its_base_game(self):
        write_nsp(self.folder / 'base.nsp', '0100A00019DE0000')
        update = write_nsp(self.folder / 'update [v655360].nsp', '0100A00019DE0800')
        info = pipeline.inspect(update)
        self.assertEqual(info['base'], 'base.nsp')
        self.assertEqual(info['title_id'], '0100A00019DE0000')

    def test_selected_base_wins_when_two_copies_share_a_title(self):
        chosen = write_nsp(self.folder / 'A-selected.nsp', '010043B013C5C000')
        write_nsp(self.folder / 'Z-another-copy.nsp', '010043B013C5C000')
        self.assertEqual(pipeline.inspect(chosen)['base'], chosen.name)

    def test_each_android_profile_can_be_selected_from_a_collection(self):
        from porter.profiles import all_entries
        games = [(row, write_nsp(self.folder / f'{row.id}.nsp', row.id)) for row in all_entries()]
        for row, path in games:
            with self.subTest(title_id=row.id):
                info = pipeline.inspect(path)
                self.assertEqual(info['title_id'], row.id)
                self.assertEqual(info['android']['status'], row.status)
                self.assertTrue(info['supported'])

    def test_cli_inspect_accepts_file_path_and_emits_json(self):
        path = write_nsp(self.folder / 'game.nsp', '010043B013C5C000')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main(['inspect', str(path), '--json'])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue())['base'], path.name)

    def test_bad_inputs_are_actionable_errors(self):
        bad = self.folder / 'bad.nsp'
        bad.write_bytes(b'not a game')
        with self.assertRaisesRegex(BuildError, 'not an NSP/PFS0'):
            sources.resolve(bad)
        bad.write_bytes(b'PFS0' + struct.pack('<III', 12, 100, 0))
        with self.assertRaisesRegex(BuildError, 'incomplete NSP file table'):
            sources.resolve(bad)
        with self.assertRaisesRegex(BuildError, 'no such game file'):
            sources.resolve(self.folder / 'missing.nsp')
        compressed = self.folder / 'game.nsz'
        compressed.write_bytes(b'anything')
        with self.assertRaisesRegex(BuildError, 'not an NSP/PFS0'):
            sources.resolve(compressed)
        unsupported = self.folder / 'game.iso'
        unsupported.write_bytes(b'anything')
        with self.assertRaisesRegex(BuildError, 'choose an NSP, XCI, NSZ or XCZ'):
            sources.resolve(unsupported)

    def test_ticketless_named_xci(self):
        cartridge = self.folder / 'Game [010043B013C5C000].XCI'
        cartridge.write_bytes(b'\0' * 16)
        self.assertEqual(pipeline.inspect(cartridge)['title_id'], '010043B013C5C000')

    def test_unknown_game_retains_experimental_status(self):
        path = write_nsp(self.folder / 'unknown.nsp', '0100000000AAA000')
        info = pipeline.inspect(path)
        self.assertFalse(info['supported'])
        self.assertTrue(info['experimental'])


if __name__ == '__main__':
    unittest.main()
