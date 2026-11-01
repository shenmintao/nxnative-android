"""Stream the data pack into one APK asset, with a content-keyed file index.

Wire format (little endian): NXPACK01, file count u32, index length u32,
data length u64, content key [32]; then (name length u32, size u64, CRC32 u32,
UTF-8 relative name) per file, followed by those files' bytes in index order.
The Android reader is game_assets.c. One asset avoids ZIP's file-count limit.
"""
import hashlib
import io
import json
import struct
import zlib
from pathlib import Path

ASSET = 'assets/nxnative/game.pack'
MANIFEST_ASSET = 'assets/nxnative/manifest.json'
MAGIC = b'NXPACK01'
HEADER = struct.Struct('<8sIIQ32s')
RECORD = struct.Struct('<IQI')
CHUNK = 1 << 20


class GamePack:
    def __init__(self, folder, package):
        self.root = Path(folder)
        meta = json.loads((self.root / 'nx2a-data.json').read_bytes())
        if meta.get('package') != package:
            raise ValueError('the game data package does not match the APK package')
        # Build timestamps do not change game content or require another full extraction.
        meta.pop('made', None)
        self.manifest = json.dumps(meta, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
        self.files = []
        records = bytearray()
        key = hashlib.sha256(MAGIC)
        total = 0
        for path in sorted(self.root.rglob('*'), key=lambda p: p.relative_to(self.root).as_posix().encode('utf-8')):
            if path.is_symlink():
                raise ValueError(f'game data cannot contain symbolic links: {path}')
            if not path.is_file():
                continue
            name = path.relative_to(self.root).as_posix().encode('utf-8')
            if b'\\' in name or b'\0' in name or name.startswith(b'.nxnative'):
                raise ValueError(f'unsupported game data path: {path}')
            size = 0
            crc = 0
            content = hashlib.sha256()
            source = self.manifest if name == b'nx2a-data.json' else path
            with (io.BytesIO(source) if isinstance(source, bytes) else source.open('rb')) as stream:
                while chunk := stream.read(CHUNK):
                    size += len(chunk)
                    crc = zlib.crc32(chunk, crc)
                    content.update(chunk)
            record = RECORD.pack(len(name), size, crc) + name
            records.extend(record)
            key.update(record)
            key.update(content.digest())
            self.files.append((path, source, size, crc))
            total += size
        if not self.files:
            raise ValueError('the game data pack is empty')
        self.key = key.digest()
        self.header = HEADER.pack(MAGIC, len(self.files), len(records), total, self.key) + records
        self.size = len(self.header) + total

    def chunks(self):
        yield self.header
        for path, source, expected_size, expected_crc in self.files:
            size = 0
            crc = 0
            with (io.BytesIO(source) if isinstance(source, bytes) else source.open('rb')) as stream:
                while chunk := stream.read(CHUNK):
                    size += len(chunk)
                    crc = zlib.crc32(chunk, crc)
                    yield chunk
            if size != expected_size or crc != expected_crc:
                raise ValueError(f'game data changed while making the APK: {path}')
