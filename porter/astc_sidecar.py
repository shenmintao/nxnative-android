"""Version 1 complete mixed ASTC bank. One independent zstd frame per image.

Source bytes are tightly packed BC blocks, layer then mip, without Tegra padding.
All output is precomputed on the PC. This module has no encoder or device API.
"""
import hashlib
import os
import struct
from pathlib import Path

HEADER = struct.Struct('<8sIIQQ32s32s32x')
ENTRY = struct.Struct('<32s32s6I5Q')
MAGIC = b'NXASTC01'
POLICY = hashlib.sha256(b'complete-mixed-astc-v1;bc1-guard1;bc3-bc5-thorough;psnr38;bc5-ra;unorm8;zstd3').digest()
MAX_IMAGE = 256 << 20
FORMATS = (0x1a01, 0x1a06, 0x1c01, 0x1c06, 0x1e01)


def zstd():
    try: from compression import zstd as z
    except ImportError: from backports import zstd as z
    return z


def source_key(digest, fmt, w, h, mips, layers):
    return hashlib.sha256(digest + struct.pack('<6I', fmt, w, h, mips, layers, 1)).digest()


def blocks(w, h, mips, layers):
    if not (0 < w <= 32768 and 0 < h <= 32768 and 0 < mips <= 16 and 0 < layers <= 2048): raise ValueError('invalid texture geometry')
    n = sum(((max(1, w >> m) + 3) // 4) * ((max(1, h >> m) + 3) // 4) for m in range(mips)) * layers
    if n * 16 > MAX_IMAGE: raise ValueError('texture exceeds the 256 MiB decompression limit')
    return n


def write(path, rows):
    """rows: sorted (key, fmt, w, h, mips, layers, ASTC bytes or None) iterable.
    Atomic replacement; callers keep the previous completed bank until success.
    """
    rows = list(rows)
    if len(rows) > 262144: raise ValueError('too many textures')
    path = Path(path); tmp = path.with_suffix(path.suffix + '.partial')
    index = []; offset = HEADER.size + ENTRY.size * len(rows); previous = b''
    try:
        with tmp.open('w+b') as f:
            f.write(bytes(offset))
            for key, fmt, w, h, mips, layers, data in rows:
                if len(key) != 32 or key <= previous or fmt not in FORMATS: raise ValueError('invalid or duplicate texture key/format')
                previous = key; n = blocks(w, h, mips, layers)
                if callable(data): data = data()
                if data is not None and len(data) != n * 16: raise ValueError('ASTC byte count does not match geometry')
                raw = data or b''; compressed = zstd().compress(raw, level=3) if data is not None else b''
                # Verify the exact stored frame, not a separately encoded sample.
                if data is not None and zstd().decompress(compressed) != raw: raise ValueError('zstd round-trip failed')
                index.append(ENTRY.pack(key, hashlib.sha256(raw).digest(), 2 if data is not None else 0,
                                       fmt, w, h, mips, layers, n, len(raw), n * (8 if fmt >> 8 == 0x1a else 16), f.tell(), len(compressed)))
                f.write(compressed); f.write(bytes((-f.tell()) % 16))
            table = b''.join(index); f.seek(0)
            f.write(HEADER.pack(MAGIC, 1, ENTRY.size, len(rows), offset, hashlib.sha256(table).digest(), POLICY)); f.write(table)
            f.flush(); os.fsync(f.fileno())
        with Reader(tmp) as r: r.verify()
        tmp.replace(path)
    finally: tmp.unlink(missing_ok=True)
    return info(path)


class Reader:
    def __init__(self, path):
        self.file = open(path, 'rb')
        try:
            size = os.fstat(self.file.fileno()).st_size
            header = self.file.read(HEADER.size)
            if len(header) != HEADER.size: raise ValueError('truncated ASTC header')
            magic, version, stride, count, offset, digest, policy = HEADER.unpack(header)
            if (magic, version, stride, policy) != (MAGIC, 1, ENTRY.size, POLICY) or count > 262144 or offset != HEADER.size + count * ENTRY.size or offset > size:
                raise ValueError('invalid ASTC header/version/policy')
            table = self.file.read(count * stride)
            if hashlib.sha256(table).digest() != digest: raise ValueError('ASTC index checksum mismatch')
            self.entries = {}; previous = b''; end = offset
            for fields in ENTRY.iter_unpack(table):
                key, sha, flag, fmt, w, h, mips, layers, n, raw, source, off, length = fields
                expected = blocks(w, h, mips, layers)
                if (key <= previous or fmt not in FORMATS or flag not in (0, 2) or n != expected or
                    raw != (n * 16 if flag else 0) or source != n * (8 if fmt >> 8 == 0x1a else 16) or
                    off != end or length > MAX_IMAGE or length > size - off or bool(flag) != bool(length)):
                    raise ValueError('invalid ASTC entry')
                self.entries[key] = fields; previous = key; end = (off + length + 15) & ~15
            if end != size: raise ValueError('truncated or trailing ASTC data')
        except BaseException:
            self.file.close(); raise

    def read(self, key):
        e = self.entries[key]
        if not e[2]: return None
        self.file.seek(e[-2]); compressed = self.file.read(e[-1])
        decoder = zstd().ZstdDecompressor()
        raw = decoder.decompress(compressed, max_length=e[-4] + 1)
        if len(raw) != e[-4] or not decoder.eof or decoder.unused_data or hashlib.sha256(raw).digest() != e[1]:
            raise ValueError('ASTC frame size/checksum mismatch')
        return raw

    def verify(self):
        for key in self.entries: self.read(key)

    def __enter__(self): return self
    def __exit__(self, *args): self.file.close()


def info(path):
    sha = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(4 << 20), b''): sha.update(chunk)
    with Reader(path) as r:
        return dict(representation='astc-mixed-zstd-v1', file='textures.astc', bytes=Path(path).stat().st_size,
                    sha256=sha.hexdigest(), entries=len(r.entries), exceptions=sum(not e[2] for e in r.entries.values()),
                    raw_astc_bytes=sum(e[-4] for e in r.entries.values()), policy=POLICY.hex())
