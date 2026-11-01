"""Decode synthetic MonoGame 5/8 effects through the actual extraction functions."""
import ast
from pathlib import Path
import re
import struct
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools/shader_split.py'
NAMES = {'_Rd', 'sph_stage', 'ctl_len', 'check', 'glslc_stage', 'glslc_section',
         'glslc_program', 'glslc_shaders', 'mgfx_effect', 'mgfx_shaders'}
tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
scope = dict(struct=struct, re=re, MGFX=b'MGFX', CTL_MAGIC=bytes.fromhex('34127698'),
             GFX_MAGIC=bytes.fromhex('78563412'), CS_MAGIC=bytes.fromhex('79563412'),
             SPH_STAGE={1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'})


def red(message):
    raise ValueError(message)


scope['red'] = red
nodes = [n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.ClassDef)) and n.name in NAMES]
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), scope)


def effect(version, vertex):
    ctl = bytearray(0x880)
    ctl[:4] = scope['CTL_MAGIC']
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    code = bytearray(0x50)
    code[:4] = scope['GFX_MAGIC']
    struct.pack_into('<I', code, 0x30, (1 if vertex else 5) << 10 | 0x61)
    struct.pack_into('<I', ctl, 0x704, len(code))
    if version == 5:
        header = bytearray(0x48)
        header[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', header, 0x44, len(header) + len(ctl) + len(code))
        bytecode = header + ctl + code
    else:
        bytecode = struct.pack('<I', len(ctl)) + ctl + struct.pack('<I', len(code)) + code
    data = b'MGFX' + bytes([version, 0]) + (b'\0' * 4 if version == 8 else b'')
    data += bytes([0, 1, int(vertex)]) + struct.pack('<I', len(bytecode)) + bytecode
    data += b'\0\0' + (b'\0' if version == 8 else b'') + b'\0\0'
    return data, bytes(ctl), bytes(code)


class MonoGameEffects(unittest.TestCase):
    def test_versions_stages_offsets_and_zero_padding(self):
        for version in (5, 8):
            for vertex in (False, True):
                with self.subTest(version=version, vertex=vertex):
                    data, ctl, code = effect(version, vertex)
                    out = scope['mgfx_effect'](b'prefix' + data + b'\0' * 31, 6, 'effect.xnb')
                    self.assertEqual([(st, c, b) for _, st, c, b in out], [('vs' if vertex else 'ps', ctl, code)])
                    with self.assertRaisesRegex(ValueError, 'effect ends'):
                        scope['mgfx_effect'](data + b'nonzero', 0, 'effect.xnb')

    def test_false_headers_and_oversized_shader_length(self):
        for version in (5, 8):
            data, _, _ = effect(version, True)
            bad = bytearray(data)
            struct.pack_into('<I', bad, (6 if version == 5 else 10) + 3, len(data) * 4)
            self.assertIsNone(scope['mgfx_effect'](bad, 0, 'main'))
            with self.assertRaisesRegex(ValueError, 'past the end'):
                scope['mgfx_effect'](bad, 0, 'effect.xnb')
        self.assertIsNone(scope['mgfx_effect'](b'MGFX\x07' + b'\0' * 20, 0, 'main'))
        self.assertIsNone(scope['mgfx_effect'](b'MGFX\x05\0', 0, 'main'))


if __name__ == '__main__':
    unittest.main()
