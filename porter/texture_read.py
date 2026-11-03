"""Bounded BNTX reader over the existing Yaz0/SARC readers (owned input only)."""
import ast
import ctypes as C
from functools import lru_cache
import hashlib
import struct
import sys

from porter.paths import DM
from porter.astc_sidecar import FORMATS, blocks, source_key


@lru_cache(maxsize=1)
def readers():
    # shader_split's module initialization selects a shader profile. These two
    # independent archive readers need none; compile their unchanged definitions.
    sys.path.insert(0, str(DM / 'gfx/tools')) if str(DM / 'gfx/tools') not in sys.path else None
    source = DM / 'gfx/tools/shader_split.py'; tree = ast.parse(source.read_text(encoding='utf-8'))
    definitions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('yaz0', 'sarc_members')]
    if len(definitions) != 2: raise ValueError('archive reader definitions changed')
    scope = dict(struct=struct)
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(source), 'exec'), scope)
    return scope['yaz0'], scope['sarc_members']


def archives(data, label, depth=0):
    if depth > 12: raise ValueError('texture archive nesting exceeds 12')
    yaz0, sarc_members = readers()
    if data[:4] == b'Yaz0': data = yaz0(data)
    if data[:4] == b'SARC':
        for name, content in sarc_members(data): yield from archives(content, label + '::' + name, depth + 1)
        return
    pos = 0
    while (pos := data.find(b'BNTX\0\0\0\0', pos)) >= 0:
        if len(data) - pos < 0x40 or data[pos + 0x20:pos + 0x24] not in (b'NX  ', b'Gen '):
            pos += 8; continue
        size = struct.unpack_from('<I', data, pos + 0x1c)[0]
        if size < 0x40 or size > len(data) - pos: raise ValueError('BNTX bounds')
        yield data[pos:pos + size], label + f'::BNTX@{pos:x}'
        pos += size


def images(data, label, native):
    def get(fmt, p):
        if p < 0 or struct.calcsize(fmt) > len(data) - p: raise ValueError('BNTX field bounds')
        return struct.unpack_from(fmt, data, p)
    def u16(p): return get('<H', p)[0]
    def u32(p): return get('<I', p)[0]
    def u64(p): return get('<Q', p)[0]
    if data[12:14] != b'\xff\xfe': raise ValueError('unsupported BNTX byte order')
    count, table = u32(0x24), u64(0x28)
    if count > 100000 or table + count * 8 > len(data): raise ValueError('BNTX table bounds')
    for i in range(count):
        t = u64(table + 8 * i)
        if data[t:t + 4] != b'BRTI': raise ValueError('BNTX texture header')
        fmt = u32(t + 0x1c)
        if fmt not in FORMATS: continue   # BC4/SNORM and unsupported formats keep the lossless runtime path
        w, h, z, layers = get('<4I', t + 0x24)
        mips, tile, samples = u16(t + 0x16), u16(t + 0x12), u16(t + 0x18)
        if z != 1 or samples != 1 or tile not in (0, 1): continue
        n = blocks(w, h, mips, layers)
        size, layout, pointers = u32(t + 0x50), u32(t + 0x34), u64(t + 0x70)
        offsets = [u64(pointers + m * 8) for m in range(mips)]
        if size % layers or offsets[0] + size > len(data): raise ValueError('BNTX image bounds')
        bpb = 8 if fmt >> 8 == 0x1a else 16
        parts = []; subresources = []; off = 0
        for layer in range(layers):
            for mip, p in enumerate(offsets):
                mw, mh = max(1, w >> mip), max(1, h >> mip)
                by = (mh + 3) // 4; th = layout & 7
                while th and by <= 8 * (1 << (th - 1)): th -= 1
                start, end = p + layer * (size // layers), offsets[0] + (layer + 1) * (size // layers)
                if start < offsets[0] or start > end or end > len(data): raise ValueError('BNTX mip bounds')
                length = ((mw + 3) // 4) * by * bpb
                out = C.create_string_buffer(length)
                if native.linear_bc(C.c_char_p(data[start:end]), end - start, out, mw, mh, 1, bpb, th, 0, tile): raise ValueError('BNTX block layout bounds')
                parts.append(out.raw); subresources.append(dict(width=mw, height=mh, blocks=length // bpb, offset=off)); off += length // bpb
        payload = b''.join(parts)
        if len(payload) != n * bpb: raise ValueError('BNTX block count')
        key = source_key(hashlib.sha256(payload).digest(), fmt, w, h, mips, layers).hex()
        yield dict(key=key, format=fmt, width=w, height=h, mips=mips, layers=layers, blocks=n,
                   subresources=subresources, source_sha256=hashlib.sha256(payload).hexdigest(), label=label, texture=i), payload
