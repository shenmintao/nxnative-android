"""Stored ZIP + APK v2 output without loading game data into memory."""
import hashlib
import os
import struct
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

CHUNK = 1 << 20
ZIP_LIMIT = 0xffffffff


@dataclass
class Entry:
    name: str
    source: object
    align: int = 4

    @property
    def size(self):
        if isinstance(self.source, bytes): return len(self.source)
        if isinstance(self.source, Path): return self.source.stat().st_size
        return self.source.size

    def chunks(self):
        if isinstance(self.source, bytes):
            for i in range(0, len(self.source), CHUNK): yield self.source[i:i + CHUNK]
        elif isinstance(self.source, Path):
            with self.source.open('rb') as stream:
                while chunk := stream.read(CHUNK): yield chunk
        else:
            yield from self.source.chunks()


def padding(offset, name_length, align):
    pad = -(offset + 30 + name_length) % align
    if 0 < pad < 4: pad += align
    return struct.pack('<HH', 0xd935, pad - 4) + b'\0' * (pad - 4) if pad else b''


def layout(entries, block_size):
    """Reject ZIP64 before writing: Android's APK v2 verifier requires ZIP32."""
    if len(entries) >= 65535:
        raise ValueError('too many APK entries for ZIP32')
    sizes = []
    offset = cd_size = 0
    names = set()
    for entry in entries:
        name = entry.name.encode('utf-8')
        if not name or len(name) > 65535 or name in names or entry.align not in (4, 16384):
            raise ValueError(f'invalid or duplicate APK entry: {entry.name}')
        names.add(name)
        size = entry.size
        sizes.append(size)
        offset += 30 + len(name) + len(padding(offset, len(name), entry.align)) + size
        cd_size += 46 + len(name)
        if size >= ZIP_LIMIT or offset + cd_size + block_size + 22 >= ZIP_LIMIT:
            raise ValueError('game data exceeds the single-APK ZIP32 limit (4 GiB including APK overhead); use --data-mode external')
    return sizes


def file_digest(stream, size, cd, eocd):
    digests = []
    stream.seek(0)
    left = size
    while left:
        chunk = stream.read(min(CHUNK, left))
        if not chunk: raise OSError('incomplete APK while computing its signature')
        digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(chunk)) + chunk).digest())
        left -= len(chunk)
    for section in (cd, eocd):
        for i in range(0, len(section), CHUNK):
            chunk = section[i:i + CHUNK]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(chunk)) + chunk).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


def write_apk(entries, output, signing_block):
    """signing_block(digest) returns the v2 signing block; replace output on success only."""
    block_size = len(signing_block(b'\0' * 32))
    sizes = layout(entries, block_size)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = None
    try:
        with tempfile.NamedTemporaryFile(mode='w+b', prefix=output.name + '.', suffix='.partial', dir=output.parent, delete=False) as stream:
            partial = Path(stream.name)
            cd = bytearray()
            dos_date = (2026 - 1980) << 9 | 1 << 5 | 1
            for entry, size in zip(entries, sizes):
                name = entry.name.encode('utf-8')
                offset = stream.tell()
                extra = padding(offset, len(name), entry.align)
                # CRC is patched after streaming the entry. The file digest is computed afterwards.
                stream.write(struct.pack('<IHHHHHIIIHH', 0x04034b50, 10, 0x800, 0, 0, dos_date, 0, size, size, len(name), len(extra)))
                stream.write(name)
                stream.write(extra)
                crc = wrote = 0
                for chunk in entry.chunks():
                    stream.write(chunk)
                    crc = zlib.crc32(chunk, crc)
                    wrote += len(chunk)
                if wrote != size: raise ValueError(f'APK input changed size: {entry.name}')
                end = stream.tell()
                stream.seek(offset + 14)
                stream.write(struct.pack('<I', crc))
                stream.seek(end)
                cd.extend(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 10, 10, 0x800, 0, 0, dos_date, crc, size, size, len(name), 0, 0, 0, 0, 0, offset) + name)
            end = stream.tell()
            eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(cd), end, 0)
            stream.flush()
            block = signing_block(file_digest(stream, end, cd, eocd))
            if len(block) != block_size: raise ValueError('APK signing block changed size')
            stream.seek(end)
            stream.write(block)
            stream.write(cd)
            stream.write(eocd[:16] + struct.pack('<I', end + len(block)) + eocd[20:])
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(partial, output)
        return output.stat().st_size
    finally:
        if partial is not None: partial.unlink(missing_ok=True)
