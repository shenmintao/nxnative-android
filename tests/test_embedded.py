"""Exercise the actual stored game-pack format and streaming APK output."""
import hashlib
import importlib.util
import json
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

APK = Path(__file__).resolve().parent.parent / 'nxnative/dm/android/apk'
sys.path.insert(0, str(APK))
from apk_stream import Entry, ZIP_LIMIT, file_digest, layout, write_apk
from game_pack import GamePack, HEADER, RECORD


def unpack(data):
    magic, count, index_size, size, key = HEADER.unpack_from(data)
    assert magic == b'NXPACK01' and len(data) == HEADER.size + index_size + size
    cursor = HEADER.size
    offset = HEADER.size + index_size
    files = {}
    for _ in range(count):
        length, size, crc = RECORD.unpack_from(data, cursor)
        cursor += RECORD.size
        name = data[cursor:cursor + length].decode('utf-8'); cursor += length
        files[name] = (data[offset:offset + size], crc); offset += size
    assert cursor == HEADER.size + index_size and offset == len(data)
    return key, files


class Embedded(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nxnative_embedded_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'; self.data.mkdir()
        (self.data / 'RomFS').mkdir()
        (self.data / 'RomFS' / 'test.bin').write_bytes(b'game data\0' * 333)
        (self.data / 'settings.txt').write_text('overlay=1\n')
        self.man = dict(package='org.nxnative.test', name='Fixture', bytes=3330, made='first')
        (self.data / 'nx2a-data.json').write_text(json.dumps(self.man))

    def pack(self): return GamePack(self.data, 'org.nxnative.test')

    def test_format_roundtrip_and_content_key(self):
        import zlib
        pack = self.pack()
        key, files = unpack(b''.join(pack.chunks()))
        self.assertEqual(key, pack.key)
        self.assertEqual(files['RomFS/test.bin'][0], (self.data / 'RomFS/test.bin').read_bytes())
        self.assertTrue(all(zlib.crc32(value) == crc for value, crc in files.values()))
        self.man['made'] = 'a later build'; (self.data / 'nx2a-data.json').write_text(json.dumps(self.man))
        self.assertEqual(self.pack().key, key)
        (self.data / 'RomFS/test.bin').write_bytes(b'changed!!\0' * 333)
        self.assertNotEqual(self.pack().key, key)

    def test_input_changes_cannot_produce_a_ready_pack(self):
        pack = self.pack()
        (self.data / 'RomFS/test.bin').write_bytes(b'other data' * 333)
        with self.assertRaisesRegex(ValueError, 'changed while making'):
            list(pack.chunks())

    def test_package_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, 'does not match'):
            GamePack(self.data, 'org.nxnative.other')

    def test_streamed_zip_roundtrip_and_library_alignment(self):
        pack = self.pack(); output = self.root / 'fixture.apk'
        lib = self.root / 'lib.so'; lib.write_bytes(b'\x7fELF' + os.urandom(512))
        entries = [Entry('AndroidManifest.xml', b'axml'), Entry('lib/arm64-v8a/libprobe.so', lib, 16384), Entry('assets/nxnative/game.pack', pack)]
        write_apk(entries, output, lambda digest: b'isolated ZIP-format test: ' + digest)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read(entries[2].name), b''.join(pack.chunks()))
            info = archive.getinfo(entries[1].name)
            with output.open('rb') as stream:
                stream.seek(info.header_offset); header = stream.read(30)
            name_size, extra_size = struct.unpack_from('<HH', header, 26)
            self.assertEqual((info.header_offset + 30 + name_size + extra_size) % 16384, 0)

    def test_oversize_rejected_before_writing_and_old_apk_preserved(self):
        class Huge:
            size = ZIP_LIMIT
            def chunks(self): raise AssertionError('oversized input must not be read')
        output = self.root / 'fixture.apk'; output.write_bytes(b'previous APK')
        with self.assertRaisesRegex(ValueError, '--data-mode external'):
            write_apk([Entry('assets/game.pack', Huge())], output, lambda _: b'signature')
        self.assertEqual(output.read_bytes(), b'previous APK')

    def test_midstream_failure_does_not_replace_previous_apk(self):
        class Broken:
            size = 100
            def chunks(self):
                yield b'a' * 10
                raise OSError('input interrupted')
        output = self.root / 'fixture.apk'; output.write_bytes(b'previous APK')
        with self.assertRaisesRegex(OSError, 'interrupted'):
            write_apk([Entry('assets/game.pack', Broken())], output, lambda _: b'signature')
        self.assertEqual(output.read_bytes(), b'previous APK')
        self.assertFalse(list(self.root.glob('*.partial')))


if __name__ == '__main__': unittest.main()
