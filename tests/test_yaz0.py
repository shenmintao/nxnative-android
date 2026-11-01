"""The optional native decoder must preserve literal and overlapping Yaz0 semantics."""
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'nxnative/dm/gfx/tools'))
from yaz0_accel import decode


def header(size): return b'Yaz0' + size.to_bytes(4, 'big') + b'\0' * 8


@unittest.skipUnless(os.environ.get('DM_YAZ0_LIB'), 'set DM_YAZ0_LIB to test the compiled decoder')
class NativeYaz0(unittest.TestCase):
    def test_literal_groups_and_empty_stream(self):
        for size in (0, 1, 7, 8, 9, 255, 65537):
            with self.subTest(size=size):
                plain = bytes(i & 255 for i in range(size))
                encoded = header(size) + b''.join(b'\xff' + plain[i:i + 8] for i in range(0, size, 8))
                self.assertEqual(decode(encoded), plain)

    def test_back_references(self):
        self.assertEqual(decode(header(16) + b'\xffABCDEFGH\0\x60\x07'), b'ABCDEFGHABCDEFGH')
        self.assertEqual(decode(header(300) + b'\x80A\0\0\xff\0\0\x08'), b'A' * 300)

    def test_damaged_streams_fail(self):
        for encoded in (b'bad', header(1), header(8) + b'\0\x60\x07', header(20) + b'\x80A\0'):
            with self.subTest(encoded=encoded):
                with self.assertRaises(ValueError): decode(encoded)


if __name__ == '__main__': unittest.main()
