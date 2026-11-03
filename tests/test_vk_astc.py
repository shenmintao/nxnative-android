"""Actual Android sidecar lookup, subresource uploads and lossless demotion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk
from test_astc_sidecar import fixture

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class AstcImages(unittest.TestCase):
    def test_content_selected_storage_reuploads_mips_layers_and_partial_write_demotion(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers: self.skipTest('host compiler and NDK Vulkan headers required')
        android = ROOT / 'nxnative/dm/android'
        source = (android / 'gfx_vk_tex.c').read_text(encoding='utf-8')
        header = (android / 'gfx_vk.h').read_text(encoding='utf-8')
        a = header.index('typedef struct { VkFormat vk;')
        fmt = header[a:header.index('} vkfmt_t;', a) + len('} vkfmt_t;')]
        a = header.index('struct vkimg {')
        image = 'typedef struct vkimg vkimg_t;\n' + header[a:header.index('\n};', a) + 4]
        a = source.index('static dm_astc_bank *g_astc_bank;')
        bank_code = source[a:source.index('static vkimg_t *img_new(', a)]
        functions = '\n'.join(function(source, f) for f in (
            'static void bc_target(', 'static void bc_decode(', 'static int image_replace(',
            'static void astc_upload(', 'static int pkg_upload_(vkimg_t *im, const tex_t *t, const vkfmt_t *f, int tracked) {', 'vkimg_t *vk_tex_lossless('))
        code = (ROOT / 'tests/native/vk_astc.c').read_text(encoding='utf-8')
        code = code.replace('/* IMAGE TYPES */', fmt + '\n' + image).replace('/* BANK FUNCTIONS */', bank_code).replace('/* IMAGE FUNCTIONS */', functions)
        zstd = ROOT / 'nxnative/dm/third_party/zstd/lib'
        with tempfile.TemporaryDirectory(prefix='vk-astc-') as temp:
            temp = Path(temp); c, exe, bank = temp / 'check.c', temp / 'check.exe', temp / 'bank'
            c.write_text(code, encoding='utf-8'); fixture(bank)
            r = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-misleading-indentation', '-D_GNU_SOURCE', '-DZSTD_DISABLE_ASM=1',
                '-idirafter', str(headers), '-I' + str(android), '-I' + str(android / 'compat/include'),
                '-I' + str(ROOT / 'nxnative/dm/gfx/nvn/include'), '-I' + str(ROOT / 'nxnative/dm/gfx/nvn'),
                '-I' + str(ROOT / 'nxnative/dm/gfx/tex'), '-I' + str(ROOT / 'nxnative/dm/third_party/bcdec'),
                str(c), str(android / 'astc_bank.c'), str(android / 'compat/sha256.c'),
                str(ROOT / 'nxnative/dm/gfx/nvn/nvn_tex.c'), str(ROOT / 'nxnative/dm/gfx/tex/nvn_deswizzle.c'),
                *map(str, sorted((zstd / 'common').glob('*.c'))), *map(str, sorted((zstd / 'decompress').glob('*.c'))),
                '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe), str(bank)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
