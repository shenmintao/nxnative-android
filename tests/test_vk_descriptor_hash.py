"""Descriptor cache keys depend on Vulkan fields, not structure padding."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


SOURCE = ROOT / 'nxnative/dm/android/gfx_vk_exec.c'


class DescriptorHash(unittest.TestCase):
    def test_image_descriptor_padding_and_fields(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = SOURCE.read_text(encoding='utf-8')
        helper = src[src.index('static inline uint64_t fnv(uint64_t h, const void *p, size_t n) {'):src.index('static void drv_shared(')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
''' + helper + r'''
int main(void) {
    VkDescriptorImageInfo a[3], b[3];
    memset(a, 0xa5, sizeof a); memset(b, 0x5a, sizeof b);
    for (unsigned i = 0; i < 3; ++i) {
        a[i].sampler = b[i].sampler = (VkSampler)(uintptr_t)(0x10000 + i);
        a[i].imageView = b[i].imageView = (VkImageView)(uintptr_t)(0x20000 + i);
        a[i].imageLayout = b[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    uint64_t seed = 0xcbf29ce484222325ull;
    assert(image_infos_hash(seed, NULL, 0) == seed);
    uint64_t original = image_infos_hash(seed, a, 3);
    assert(image_infos_hash(seed, b, 3) == original);
    // Reproduce the bad key when this ABI has trailing padding after imageLayout.
    if (sizeof a[0] > sizeof a[0].sampler + sizeof a[0].imageView + sizeof a[0].imageLayout)
        assert(fnv(seed, a, sizeof a) != fnv(seed, b, sizeof b));
    for (unsigned index = 0; index < 3; ++index) {
        for (unsigned field = 0; field < 3; ++field) {
            memcpy(b, a, sizeof a);
            if (field == 0) b[index].sampler = (VkSampler)(uintptr_t)0x30000;
            if (field == 1) b[index].imageView = (VkImageView)(uintptr_t)0x40000;
            if (field == 2) b[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            assert(image_infos_hash(seed, b, 3) != original);
        }
    }
    b[0] = a[2]; b[1] = a[1]; b[2] = a[0];
    assert(image_infos_hash(seed, b, 3) != original);
    assert(image_infos_hash(seed, a, 2) != original);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-descriptor-hash-') as temp:
            c = Path(temp) / 'hash.c'
            exe = Path(temp) / 'hash-check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-idirafter', str(headers), str(c), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
