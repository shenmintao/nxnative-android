"""Offline bank, target selection, codec gates and the Android reader; no adb."""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

from porter import astc_sidecar as bank
from porter.texture_target import select
from porter.upstream import BuildError

ROOT = Path(__file__).resolve().parents[1]


def fixture(path):
    rows = []
    for fmt, w, h, levels, layers, exception in [(0x1a01, 7, 9, 3, 2, False), (0x1c06, 16, 16, 1, 1, True), (0x1e01, 4, 4, 1, 1, False)]:
        n = bank.blocks(w, h, levels, layers)
        digest = hashlib.sha256(bytes([fmt & 255]) * (n * (8 if fmt >> 8 == 0x1a else 16))).digest()
        key = bank.source_key(digest, fmt, w, h, levels, layers)
        raw = bytes.fromhex('fcfdffffffffffff000000000000ffff') * n
        rows.append((key, fmt, w, h, levels, layers, None if exception else raw))
    bank.write(path, sorted(rows))
    return rows


class TextureTargets(unittest.TestCase):
    def test_offline_models_families_unknown_and_no_device_calls(self):
        adb = Mock()
        for target, expected in [('adreno', 'bc'), ('tb323fu', 'bc'), ('xclipse', 'bc'), ('mali', 'astc'), ('v2405a', 'astc'), ('powervr', 'astc')]:
            self.assertEqual(select(target, adb=adb), expected)
        adb.pick.assert_not_called()
        with self.assertRaises(BuildError): select('unknown-phone')
        with self.assertRaises(BuildError): select()

    def test_connected_capability_and_unknown_refusal(self):
        adb = Mock(); adb.bc_textures.return_value = True
        self.assertEqual(select(adb=adb), 'bc')
        adb.bc_textures.return_value = False; adb.astc_textures.return_value = True
        self.assertEqual(select(adb=adb), 'astc')
        adb.astc_textures.return_value = None
        with self.assertRaises(BuildError): select(adb=adb)

    def test_vulkan_json_feature_and_decode_extension(self):
        from porter.adb import Adb
        a = Adb('unused')
        for feature, extension, expected in [(1, True, True), (0, True, False), (1, False, False)]:
            d = dict(devices=[dict(features=dict(textureCompressionASTC_LDR=feature), extensions=[dict(extensionName='VK_EXT_astc_decode_mode')] if extension else [])])
            with patch.object(a, 'shell', return_value=json.dumps(d)): self.assertEqual(a.astc_textures(), expected)

    def test_cli_and_gui_both_supply_target_and_serial(self):
        from porter.cli import _parser
        a = _parser().parse_args(['port', '.', '--target-gpu', 'mali', '--device', 'selected'])
        self.assertEqual((a.target_gpu, a.device), ('mali', 'selected'))
        source = (ROOT / 'gui/app.py').read_text(encoding='utf-8')
        self.assertIn("['--target-gpu', self.target_gpu.currentData()]", source)
        self.assertIn("['--device', self.device.currentData()]", source)

    def test_gui_target_is_forwarded_without_install_or_device_access(self):
        os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
        try:
            from PySide6.QtWidgets import QApplication
            from gui.app import PortTab
        except ImportError: self.skipTest('PySide6 required for the GUI test')
        app = QApplication.instance() or QApplication([])
        with patch('gui.app.call_json'), patch('gui.app.config.load', return_value={'keys': 'unused'}):
            tab = PortTab(Mock()); tab.info = {'supported': True}
            tab.target_gpu.setCurrentIndex(tab.target_gpu.findData('mali'))
            tab.folder.setText('owned-input'); tab.install.setChecked(False)
            with patch.object(tab, 'run') as run: tab.start()
            args = run.call_args.args[0]
            self.assertEqual(args[args.index('--target-gpu') + 1], 'mali')
            self.assertNotIn('--install', args)
            tab.tick.stop(); tab.close(); tab.deleteLater(); app.processEvents()


class AstcSidecar(unittest.TestCase):
    def test_port_data_supplies_its_native_archive_reader_and_restores_environment(self):
        from types import SimpleNamespace
        from porter import pipeline
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            port = SimpleNamespace(texture_target='astc', aw=root, game=root, aoc=None, tools={'ndk': 'mock'},
                env={'DM_YAZ0_LIB': 'native-reader'}, o=SimpleNamespace(jobs=4, data_mode='external', out=root, touch=False),
                g=Mock(), app_name='Mock', ap=Mock(), pkg_ver='v0', display='1', patched=root, dmsv=root, lang='en-US', package='org.example.test',
                fps_env=[], fps_files={}, result={}, button_font=lambda: ([], {}))
            def generate(*args):
                self.assertEqual(os.environ.get('DM_YAZ0_LIB'), 'native-reader')
                return {'representation': 'astc-mixed-zstd-v1'}
            with patch.dict(os.environ, {'DM_YAZ0_LIB': 'previous'}), patch('porter.textures.generate', side_effect=generate), \
                 patch('porter.pipeline.datapack.build', return_value={'files': 1, 'bytes': 1}) as build, patch('porter.pipeline.PROG'), \
                 patch('porter.pipeline.app_stem', return_value='Mock'):
                pipeline.Port.data(port)
                self.assertEqual(os.environ['DM_YAZ0_LIB'], 'previous')
                self.assertEqual(build.call_args.kwargs['files'], {'textures.astc': root / 'textures.astc'})
                self.assertEqual(build.call_args.kwargs['textures']['representation'], 'astc-mixed-zstd-v1')

    def test_manifest_native_unchanged_astc_requires_and_install_rejects_missing_capability(self):
        from types import SimpleNamespace
        from porter import datapack, install
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root / 'mods').mkdir(); (root / 'rom').mkdir(); (root / 'shaders').write_bytes(b'shader')
            source = root / 'textures.astc'; fixture(source)
            profile = SimpleNamespace(id='test', name='Test', package='org.example.test', env=[], settings={}, requires=['bcn', 'other'])
            common = dict(profile=profile, version='v0', display='1', patched=root / 'mods', romfs=root / 'rom', shaders=root / 'shaders')
            native = datapack.build(root / 'native', **common)
            self.assertEqual(native['requires'], ['bcn', 'other']); self.assertNotIn('textures', native)
            info = bank.info(source)
            astc = datapack.build(root / 'astc', **common, textures=info, files={'textures.astc': source})
            self.assertEqual(astc['requires'], ['other', 'astc-ldr', 'astc-unorm8'])
            self.assertEqual(profile.requires, ['bcn', 'other'])
            (root / 'fake.apk').write_bytes(b'fake')
            adb = Mock(); adb.astc_textures.return_value = False
            with patch('porter.install.Adb', return_value=adb):
                with self.assertRaises(BuildError): install.install(root / 'fake.apk', root / 'astc', 'mock-adb')
            adb.install.assert_not_called()
    def test_roundtrip_complete_mips_layers_exceptions_and_determinism(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'a'; rows = fixture(p)
            with bank.Reader(p) as r:
                self.assertEqual(len(r.entries), 3)
                for row in rows: self.assertEqual(r.read(row[0]), row[-1])
            q = Path(d) / 'b'; bank.write(q, sorted(rows))
            self.assertEqual(p.read_bytes(), q.read_bytes())
            self.assertEqual(bank.info(p)['exceptions'], 1)

    def test_corrupt_header_index_frames_truncation_and_concatenated_frame(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'a'; fixture(p); original = p.read_bytes()
            for position in (0, 8, 12, 16, 24, 32, 64, 128, len(original) - 16):
                damaged = bytearray(original); damaged[position] ^= 128; p.write_bytes(damaged)
                with self.assertRaises((ValueError, bank.zstd().ZstdError)):
                    with bank.Reader(p) as r: r.verify()
            for damaged in (original[:127], original[:-1], original + b'extra'):
                p.write_bytes(damaged)
                with self.assertRaises(ValueError): bank.Reader(p)

    def test_host_quality_gates_rgb_alpha_meaningful_rg_and_angles(self):
        import numpy as np
        from porter.texture_quality import quality
        a = np.full((1, 4, 4, 4), 128, np.uint8); mask = np.ones((1, 4, 4), bool)
        self.assertFalse(quality(a, a, mask, True)['exception'])
        b = a.copy(); b[..., 0] += 5
        self.assertGreater(quality(a, b, mask)['rgb'], 38)
        self.assertLess(quality(a, b, mask, True)['rg'], 38)
        self.assertTrue(quality(a, b, mask, True)['exception'])
        b = a.copy(); b[..., 3] += 4
        self.assertTrue(quality(a, b, mask)['exception'])
        self.assertGreater(quality(a, a, mask, True)['angular_p95'], -1)

    def test_geometry_limits_and_unapproved_formats(self):
        for shape in ((0, 4, 1, 1), (4, 4, 17, 1), (32768, 32768, 1, 1), (4, 4, 1, 2049)):
            with self.assertRaises(ValueError): bank.blocks(*shape)
        self.assertNotIn(0x1d01, bank.FORMATS)
        self.assertNotIn(0x1e02, bank.FORMATS)


class AndroidAstcBank(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: raise unittest.SkipTest('host C compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='astc-bank-'); cls.exe = Path(cls.temp.name) / 'reader.exe'
        android = ROOT / 'nxnative/dm/android'; zstd = ROOT / 'nxnative/dm/third_party/zstd/lib'
        sources = [ROOT / 'tests/native/astc_bank.c', android / 'astc_bank.c', android / 'compat/sha256.c', *sorted((zstd / 'common').glob('*.c')), *sorted((zstd / 'decompress').glob('*.c'))]
        command = [cc, '-O2', '-std=c11', '-D_GNU_SOURCE', '-DZSTD_DISABLE_ASM=1', '-I' + str(android), '-I' + str(android / 'compat/include'), *map(str, sources), '-pthread', '-o', str(cls.exe)]
        r = subprocess.run(command, capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_real_android_reader_roundtrip_and_rejects_corruption(self):
        p = Path(self.temp.name) / 'bank'; fixture(p); original = p.read_bytes()
        r = subprocess.run([str(self.exe), str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for position in (8, 64, 128, len(original) - 16):
            broken = bytearray(original); broken[position] ^= 128; p.write_bytes(broken)
            self.assertNotEqual(subprocess.run([str(self.exe), str(p)], capture_output=True).returncode, 0)

    def test_runtime_uses_physical_astc_views_and_lossless_write_path_in_release(self):
        a = ROOT / 'nxnative/dm/android'
        source = (a / 'gfx_vk_tex.c').read_text(encoding='utf-8')
        self.assertIn('V.has_bc || !V.has_astc_decode', source)
        self.assertIn('im = img_new(t, e ? &a : f)', source)
        self.assertIn('if (im->astc) { astc_upload(im); return 1; }', source)
        self.assertIn('VK_FORMAT_R8G8B8A8_UNORM', source[source.index('VkImageViewASTCDecodeModeEXT decode'):])
        replay = (a / 'gfx_vk_exec.c').read_text(encoding='utf-8')
        self.assertIn('vk_tex_lossless(c->tex)', replay)
        self.assertIn('vk_tex_lossless(c->src), *d = vk_tex_lossless(c->dst)', replay)
        # The bank is ordinary runtime code, present in debug and release alike.
        self.assertNotIn('DM_RELEASE', (a / 'astc_bank.c').read_text())
        self.assertNotIn('DM_RELEASE', source[source.index('static void astc_init('):source.index('static vkimg_t *img_new(')])


if __name__ == '__main__': unittest.main()
