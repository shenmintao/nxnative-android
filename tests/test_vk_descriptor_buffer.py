"""Compile the actual optional transport against a deterministic Vulkan driver."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class DescriptorBuffer(unittest.TestCase):
    def test_immutable_encoding_addresses_arrays_binding_state_and_stale_handles(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and NDK Vulkan headers required')
        source = (DM / 'android/gfx_vk_descriptor.c').read_text(encoding='utf8')
        source = source.replace('#include "gfx_vk.h"', '')
        with tempfile.TemporaryDirectory(prefix='vk-descriptor-buffer-') as temp:
            path = Path(temp)
            (path/'descriptor_buffer_runtime.inc').write_text(source, encoding='utf8')
            exe = path/'check.exe'
            subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers', '-idirafter', str(headers),
                '-I'+str(DM/'gfx/nvn'), '-I'+str(path),
                str(ROOT/'tests/native/vk_descriptor_buffer.c'), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('PASS', result.stdout)
            for invalid in ('layout', 'buffer'):
                rejected = subprocess.run([str(exe), invalid], capture_output=True)
                self.assertEqual(rejected.returncode, 77, invalid)
