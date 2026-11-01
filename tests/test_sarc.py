"""Archive filenames must not copy the entire remaining game archive per member."""
import ast
import struct
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / 'nxnative/dm/gfx/tools/shader_split.py'
tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'sarc_members')
scope = {'struct': struct}
exec(compile(ast.Module(body=[function], type_ignores=[]), str(SOURCE), 'exec'), scope)
sarc_members = scope['sarc_members']


def archive(entries):
    names = bytearray(); nodes = bytearray(); data = bytearray()
    for name, content in entries:
        offset = len(names) // 4
        names.extend(name.encode('utf-8') + b'\0')
        names.extend(b'\0' * (-len(names) % 4))
        nodes.extend(struct.pack('<IIII', 0, 0x01000000 | offset, len(data), len(data) + len(content)))
        data.extend(content)
    data_offset = 20 + 12 + len(nodes) + 8 + len(names)
    header = struct.pack('<4sHHIIHH', b'SARC', 20, 0xfeff, data_offset + len(data), data_offset, 0x100, 0)
    result = header + struct.pack('<4sHHI', b'SFAT', 12, len(entries), 0x65) + nodes + struct.pack('<4sHH', b'SFNT', 8, 0) + names + data
    return result, data_offset


class SarcNames(unittest.TestCase):
    def test_members_and_nested_names_are_unchanged(self):
        expected = [('Shader/test.bin', b'abc\0def'), ('Dir/second', b'xyz'), ('empty', b'')]
        packed, _ = archive(expected)
        self.assertEqual(list(sarc_members(packed)), expected)

    def test_name_reads_do_not_slice_archive_payload(self):
        packed, data_offset = archive([('a', b'x' * 1000), ('b', b'y' * 1000)])
        class Guarded(bytes):
            def __getitem__(self, key):
                if isinstance(key, slice) and key.start and key.start < data_offset:
                    if key.stop is None or key.stop > data_offset:
                        raise AssertionError('filename lookup copied the archive payload')
                return super().__getitem__(key)
        self.assertEqual([name for name, _ in sarc_members(Guarded(packed))], ['a', 'b'])

    def test_missing_name_terminator_is_reported(self):
        packed, data_offset = archive([('file', b'payload')])
        broken = bytearray(packed)
        start = 20 + 12 + 16 + 8
        broken[start:data_offset] = b'x' * (data_offset - start)
        with self.assertRaisesRegex(ValueError, 'filename outside'):
            list(sarc_members(broken))


if __name__ == '__main__': unittest.main()
