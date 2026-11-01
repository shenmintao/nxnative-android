"""Descriptor allocation recycling preserves fence lifetime and pool budgets."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class DescriptorAllocationReuse(unittest.TestCase):
    def test_fences_layouts_growth_fallback_and_allocation_failure(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        types = source[source.index('typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size, used; } scratch_blk;'):source.index('static vksub_t g_slot[NSLOT];')]
        declarations = source[source.index('enum { DP_SETS = 4096 };'):source.index('void vk_dsl_register(')]
        implementation = declarations + '\n'.join(function(source, signature) for signature in (
            'static void descriptor_reuse_free(', 'static void descriptor_slot_recycle(',
            'void vk_descriptor_reuse_stats(', 'static void slot_recycle(',
            'void vk_dsl_register(', 'static VkDescriptorSet descriptor_alloc(',
            'VkDescriptorSet vk_alloc_set('))
        with tempfile.TemporaryDirectory(prefix='vk-descriptor-reuse-') as temp:
            temp = Path(temp)
            (temp / 'descriptor_reuse_types.inc').write_text(types, encoding='utf-8')
            for negative in (False, True):
                body = implementation
                if negative:
                    needle = 'if (g_prereset) mtl_wait(s->fence, ~0ull); else wait_fence(s->fence);'
                    self.assertEqual(body.count(needle), 1)
                    body = body.replace(needle, 'descriptor_slot_recycle(s);\n    ' + needle)
                (temp / 'descriptor_reuse_impl.inc').write_text(body, encoding='utf-8')
                exe = temp / ('negative' if negative else 'reuse')
                result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                         '-idirafter', str(headers), '-I', str(temp),
                                         '-I', str(ROOT / 'nxnative/dm/gfx/nvn'),
                                         '-I', str(ROOT / 'nxnative/dm/android'),
                                         str(ROOT / 'tests/native/vk_descriptor_reuse.c'), '-o', str(exe)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 77 if negative else 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
