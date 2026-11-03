"""Synthetic blocks only: desktop direct conversion, fallback and image gates."""
import ctypes as C
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import Mock

from porter import astc_tool, paths
from porter.textures import Codec, encode_image, native, ptr

ROOT = Path(__file__).resolve().parents[1]


class TextureCodec(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: raise unittest.SkipTest('host C compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='texture-codec-'); cls.root = Path(cls.temp.name)
        lib = cls.root / ('codec.dll' if os.name == 'nt' else 'codec.so')
        source = ROOT / 'porter/native'
        r = subprocess.run([cc, '-std=c11', '-shared', '-O2', '-ffreestanding', '-fno-builtin', '-fPIC',
            '-I' + str(source / 'texture_include'), '-I' + str(ROOT / 'nxnative/dm/gfx/nvn'),
            str(source / 'texture_codec.c'), str(ROOT / 'nxnative/dm/gfx/nvn/nvn_tex.c'),
            str(ROOT / 'nxnative/dm/gfx/tex/nvn_deswizzle.c'), '-o', str(lib)], capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stdout + r.stderr)
        cls.lib = native(lib)

    @classmethod
    def tearDownClass(cls):
        # Windows cannot remove the helper until ctypes releases it.
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(cls.lib._handle)
        cls.temp.cleanup()

    def test_bc1_known_blocks_guard_and_reference_decoder_error_bound(self):
        import numpy as np
        name = astc_tool.archive_name(); host = name.removeprefix(f'astcenc-{astc_tool.VERSION}-').removesuffix('.zip')
        archive = paths.player_tools() / f'astcenc-{astc_tool.VERSION}-{host}' / name
        # test_porter isolates NX2A_HOME for the full suite. Reuse an already
        # downloaded developer archive there; get() still enforces its pin.
        local = ROOT / 'tools/player/toolchains' / archive.parent.name / name
        if not archive.is_file() and local.is_file():
            archive.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(local, archive)
        if not archive.is_file(): self.skipTest('pinned astcenc tool is not cached; tests never download tools')
        exe = astc_tool.get(bool(self.lib.texture_avx2())); codec = Codec(exe, self.root)
        blocks = [struct.pack('<HHI', a, b, indices) for a, b in [(0xffff, 0), (0xf800, 0x07e0), (0x8021, 0x1020)]
                  for indices in (0, 0xffffffff, 0xe4e4e4e4, 0x01234567)]
        bc = np.frombuffer(b''.join(blocks), np.uint8).copy().reshape(-1, 8)
        reference = np.empty((len(bc), 4, 4, 4), np.uint8)
        for fmt in (0x1a01, 0x1a06):
            out = np.empty((len(bc), 16), np.uint8); status = np.empty(len(bc), np.uint8)
            self.lib.transcode_bc(fmt, ptr(bc), ptr(out), ptr(status), len(bc), 1)
            self.assertTrue((status == 0).all())
            self.lib.decode_bc(fmt, ptr(bc), ptr(reference), len(bc))
            decoded = codec.decode(out, srgb=fmt == 0x1a06)
            self.assertLessEqual(int(np.abs(decoded.astype(np.int16) - reference.astype(np.int16)).max()), 1)
        bc = np.frombuffer(struct.pack('<HHI', 0, 0xffff, 0xffffffff), np.uint8).copy()
        self.lib.transcode_bc(0x1a01, ptr(bc), ptr(out), ptr(status), 1, 1)
        self.assertEqual(int(status[0]), 1)  # punch-through is always an encoder fallback

    def test_fallback_goes_through_encoder_and_failed_image_is_a_lossless_exception(self):
        import numpy as np
        work = self.root / 'cache'; (work / 'bc').mkdir(parents=True, exist_ok=True); (work / 'astc').mkdir(exist_ok=True)
        for fmt, data, bad in [(0x1a01, struct.pack('<HHI', 0, 0xffff, 0xffffffff), False),
                              (0x1c01, b'\xff\0' + bytes(6) + struct.pack('<HHI', 0xffff, 0, 0), True)]:
            key = hashlib.sha256(data).hexdigest(); (work / 'bc' / (key + '.bin')).write_bytes(data)
            bc = np.frombuffer(data, np.uint8).copy(); reference = np.empty((1, 4, 4, 4), np.uint8)
            self.lib.decode_bc(fmt, ptr(bc), ptr(reference), 1)
            codec = Mock(); codec.encode.return_value = np.zeros((1, 16), np.uint8)
            codec.decode.return_value = np.zeros_like(reference) if bad else reference
            row = dict(key=key, format=fmt, blocks=1, subresources=[dict(width=4, height=4, blocks=1, offset=0)])
            result = encode_image(row, work, self.lib, codec, 'synthetic-test')
            codec.encode.assert_called_once(); self.assertEqual(result['exception'], bad)
            self.assertEqual((work / 'astc' / (key + '.bin')).exists(), not bad)
            if not bad: self.assertEqual(result['fallback_blocks'], 1)
            again = encode_image(row, work, self.lib, codec, 'synthetic-test')
            self.assertEqual(result, again); codec.encode.assert_called_once()
