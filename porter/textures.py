"""Offline complete ASTC generation. No transcoder is linked into the Android host."""
import ctypes as C
import hashlib
import json
import os
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from porter import astc_sidecar as bank, astc_tool, native_tools, texture_read
from porter.texture_quality import quality, masks_and_edges
from porter.upstream import BuildError, PROG


def native(library):
    lib = C.CDLL(str(library))
    lib.linear_bc.argtypes = [C.c_void_p, C.c_uint64, C.c_void_p] + [C.c_uint] * 7
    lib.decode_bc.argtypes = [C.c_uint, C.c_void_p, C.c_void_p, C.c_uint]
    lib.transcode_bc.argtypes = [C.c_uint, C.c_void_p, C.c_void_p, C.c_void_p, C.c_uint, C.c_int]
    return lib


def ptr(a): return C.c_void_p(a.ctypes.data)


def atlas(pixels):
    import numpy as np
    n = len(pixels); cols = min(128, math_ceil_sqrt(n)); rows = (n + cols - 1) // cols
    out = np.zeros((rows * cols, 4, 4, 4), np.uint8); out[:n] = pixels
    return out.reshape(rows, cols, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(rows * 4, cols * 4, 4).copy()


def math_ceil_sqrt(n):
    import math
    return max(1, math.isqrt(n - 1) + 1)


def unatlas(pixels, n):
    h, w = pixels.shape[:2]
    return pixels.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 4, 4, 4)[:n].copy()


class Codec:
    def __init__(self, executable, scratch): self.exe, self.scratch = executable, scratch

    def call(self, args):
        r = subprocess.run([str(self.exe), *map(str, args), '-j', '1', '-silent'], capture_output=True, text=True)
        if r.returncode: raise BuildError(f'astcenc failed: {r.stdout[-2000:]} {r.stderr[-2000:]}')

    def encode(self, pixels, srgb=False, normal=False):
        from PIL import Image
        import numpy as np
        # Bounded atlases also bound the external tool's temporary working set.
        parts = []
        for off in range(0, len(pixels), 65536):
            image = atlas(pixels[off:off + 65536]); h, w = image.shape[:2]
            with tempfile.TemporaryDirectory(dir=self.scratch, prefix='encode-') as tmp:
                src, dst = Path(tmp) / 'input.png', Path(tmp) / 'output.astc'
                Image.fromarray(image).save(src, compress_level=0)
                self.call(['-cs' if srgb else '-cl', src, dst, '4x4', '-thorough', '-decode_unorm8'] + (['-normal'] if normal else []))
                data = dst.read_bytes()
                if data[:7] != b'\x13\xab\xa1\x5c\x04\x04\x01' or len(data) != 16 + (w // 4) * (h // 4) * 16: raise BuildError('astcenc output geometry mismatch')
                parts.append(np.frombuffer(data[16:], np.uint8).reshape(-1, 16)[:min(65536, len(pixels) - off)].copy())
        return np.concatenate(parts) if parts else np.empty((0, 16), np.uint8)

    def decode(self, blocks, srgb=False, normal=False):
        from PIL import Image
        import numpy as np
        parts = []
        for off in range(0, len(blocks), 65536):
            b = blocks[off:off + 65536]; n = len(b); cols = min(128, math_ceil_sqrt(n)); rows = (n + cols - 1) // cols
            padded = np.zeros((rows * cols, 16), np.uint8); padded[:n] = b
            # A valid constant black block for atlas-only padding.
            if n < len(padded): padded[n:] = np.array([0xfc, 0xfd, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 255, 255], np.uint8)
            with tempfile.TemporaryDirectory(dir=self.scratch, prefix='decode-') as tmp:
                src, dst = Path(tmp) / 'input.astc', Path(tmp) / 'output.png'
                header = b'\x13\xab\xa1\x5c\x04\x04\x01' + (cols * 4).to_bytes(3, 'little') + (rows * 4).to_bytes(3, 'little') + b'\x01\0\0'
                src.write_bytes(header + padded.tobytes())
                self.call(['-ds' if srgb else '-dl', src, dst] + (['-dsw', 'ra01'] if normal else []))
                with Image.open(dst) as im: parts.append(unatlas(np.array(im.convert('RGBA')), n))
        return np.concatenate(parts)


def encode_image(row, work, lib, codec, tool_key):
    import numpy as np
    key = row['key']; source = work / 'bc' / (key + '.bin'); output = work / 'astc' / (key + '.bin'); record = output.with_suffix('.json')
    if record.is_file():
        cached = json.loads(record.read_text())
        if cached.get('tool_key') == tool_key and (cached['exception'] or (output.is_file() and hashlib.sha256(output.read_bytes()).hexdigest() == cached['astc_sha256'])):
            return cached
    start = time.perf_counter(); fmt, n = row['format'], row['blocks']; normal = fmt == 0x1e01; srgb = fmt & 255 == 6
    bc = np.frombuffer(source.read_bytes(), np.uint8).reshape(n, 8 if fmt >> 8 == 0x1a else 16).copy()
    reference = np.empty((n, 4, 4, 4), np.uint8); lib.decode_bc(fmt, ptr(bc), ptr(reference), n)
    pixels = reference.copy(); mask = masks_and_edges(pixels, row['subresources'])
    status = np.ones(n, np.uint8)
    if fmt >> 8 == 0x1a:
        encoded = np.empty((n, 16), np.uint8); lib.transcode_bc(fmt, ptr(bc), ptr(encoded), ptr(status), n, 1)
        fallback = status != 0
        encoded[fallback] = codec.encode(pixels[fallback], srgb)
    else: encoded = codec.encode(pixels, srgb, normal)
    decoded = codec.decode(encoded, srgb, normal)
    result = quality(reference, decoded, mask, normal)
    direct = status == 0
    direct_max = int(np.abs(decoded[direct].astype(np.int16) - reference[direct].astype(np.int16)).max()) if direct.any() else 0
    if direct_max > 1: raise BuildError('BC1 direct path exceeded the one-byte error bound')
    if fmt >> 8 == 0x1a and result['exception']: raise BuildError('BC1 image failed the 38 dB image gate')
    data = encoded.tobytes()
    result.update(key=key, format=fmt, tool_key=tool_key, direct_blocks=int(direct.sum()), fallback_blocks=int((status != 0).sum()) if fmt >> 8 == 0x1a else 0,
                  direct_max=direct_max, seconds=time.perf_counter() - start, astc_sha256=hashlib.sha256(data).hexdigest())
    # Reject NaN metrics; JSON infinities are written as strings for verifiable records.
    clean = {k: ('Infinity' if isinstance(v, float) and v == float('inf') else v) for k, v in result.items()}
    if not result['exception']:
        part = output.with_suffix('.partial'); part.write_bytes(data); part.replace(output)
    record.write_text(json.dumps(clean, sort_keys=True, indent=2) + '\n')
    return clean


def generate(romfs, aoc, out, work, ndk, jobs=4):
    """Final port stage; cache by source content/geometry and all generator/tool inputs."""
    from porter.paths import REPO
    start = time.perf_counter(); out, work = Path(out), Path(work)
    out.parent.mkdir(parents=True, exist_ok=True)
    for d in (work, work / 'bc', work / 'astc', work / 'scratch', work / 'logs'): d.mkdir(parents=True, exist_ok=True)
    library = native_tools.textures(ndk, work / 'logs', dict(os.environ)); lib = native(library)
    exe = astc_tool.get(bool(lib.texture_avx2()))
    sources = [p for p in (REPO / 'porter').glob('texture*.py')] + [REPO / 'porter/astc_sidecar.py', REPO / 'porter/astc_tool.py']
    tool_key = hashlib.sha256(bank.POLICY + b''.join(p.read_bytes() for p in sorted(sources)) + library.read_bytes() + exe.read_bytes()).hexdigest()
    roots = [(Path(romfs), 'RomFS')] + ([(Path(aoc), 'aoc')] if aoc else [])
    files = [(root, tag, p) for root, tag in roots for p in sorted(root.rglob('*')) if p.is_file()]
    rows = {}; catalog = []; fingerprints = []; scanned = 0
    for root, tag, p in files:
        data = p.read_bytes(); scanned += len(data); label = tag + '/' + p.relative_to(root).as_posix()
        fingerprints.append((label, hashlib.sha256(data).hexdigest()))
        try:
            for bntx, name in texture_read.archives(data, label):
                for row, bc in texture_read.images(bntx, name, lib):
                    key = row['key']; catalog.append(row)
                    if key not in rows:
                        (work / 'bc' / (key + '.bin')).write_bytes(bc); rows[key] = row
        except (ValueError, IndexError, OverflowError) as e: raise BuildError(f'texture scan failed at {label}: {e}') from e
        if len(fingerprints) % 256 == 0: PROG.set(step='finding BC textures', done=len(fingerprints), total=len(files), unit='files')
    (work / 'catalog.json').write_text(json.dumps(catalog, sort_keys=True))
    (work / 'inputs.json').write_text(json.dumps(fingerprints))
    codec = Codec(exe, work / 'scratch'); results = {}
    def task(row): return encode_image(row, work, lib, codec, tool_key)
    with ThreadPoolExecutor(max_workers=max(1, min(jobs, 4))) as pool:
        for i, r in enumerate(pool.map(task, sorted(rows.values(), key=lambda r: r['key']))):
            results[r['key']] = r
            PROG.set(step='precomputing ASTC and checking quality', done=i + 1, total=len(rows), unit='textures')
    entries = []
    for key, row in sorted(rows.items()):
        output = work / 'astc' / (key + '.bin')
        loader = None if results[key]['exception'] else lambda p=output: p.read_bytes()
        entries.append((bytes.fromhex(key), row['format'], row['width'], row['height'], row['mips'], row['layers'], loader))
    PROG.set(step='compressing and verifying per-texture frames')
    result = bank.write(out, entries)
    result.update(seconds=time.perf_counter() - start, textures=len(catalog), scanned_files=len(files), scanned_bytes=scanned,
                  input_sha256=hashlib.sha256(json.dumps(fingerprints, separators=(',', ':')).encode()).hexdigest(),
                  tool_key=tool_key, astcenc_version=astc_tool.VERSION, astcenc_commit=astc_tool.COMMIT,
                  direct_blocks=sum(r['direct_blocks'] for r in results.values()), fallback_blocks=sum(r['fallback_blocks'] for r in results.values()),
                  exceptions_list=[catalog_row for catalog_row in catalog if results[catalog_row['key']]['exception']])
    (work / 'quality.json').write_text(json.dumps(results, sort_keys=True, indent=2))
    (work / 'generation.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    return {k: v for k, v in result.items() if k != 'exceptions_list'}
