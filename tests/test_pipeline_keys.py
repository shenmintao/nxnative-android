"""Build-scoped prewarm packaging and first-launch cache preservation."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from porter.prewarm import find_keys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'nxnative/dm/android/apk'))
from pipeline_keys import read_keys
from test_vk_guest_notify import function


class PipelineKeys(unittest.TestCase):
    def test_pack_rejects_torn_or_unrecognized_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'keys'
            good = struct.pack('<II', 0x31455050, 1000) + bytes(2000)
            path.write_bytes(good)
            self.assertEqual(read_keys(path), good)
            for bad in (good[:-1], b'bad!' + good[4:], good[:8], struct.pack('<II', 0x31455050, 0) + b'x'):
                path.write_bytes(bad)
                with self.assertRaises(ValueError): read_keys(path)

    def test_only_packed_build_is_selected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); main = root / 'main'
            header = bytearray(0x60); header[:4] = b'NSO0'; header[0x40:0x50] = bytes(range(16))
            main.write_bytes(header)
            path = root / 'pipeline-prewarm' / bytes(range(16)).hex() / 'vk_pipelines.bin'
            path.parent.mkdir(parents=True); path.write_bytes(b'fixture')
            self.assertEqual(find_keys(root, main), path)
            header[0x40] = 99; main.write_bytes(header)
            self.assertIsNone(find_keys(root, main))
            main.write_bytes(b'bad')
            with self.assertRaises(ValueError): find_keys(root, main)

    def test_runtime_seeds_only_complete_assets_and_preserves_existing_cache(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a host C compiler is required')
        source = (ROOT / 'nxnative/dm/android/game_assets.c').read_text(encoding='utf8')
        preamble, checks = (ROOT / 'tests/native/pipeline_seed.c').read_text(encoding='utf8').split('/* PRODUCTION HELPERS */')
        helpers = '\n'.join(function(source, signature) for signature in (
            'static uint32_t u32(', 'static int asset_read(', 'static int path_join(',
            'static int write_atomic(', 'int dm_pipeline_keys_seed('))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'seed.c'; exe = Path(temp) / 'seed.exe'
            path.write_text(preamble + helpers + checks, encoding='utf8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=temp, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main()
