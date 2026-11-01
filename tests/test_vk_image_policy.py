"""Check color/depth format policies without changing other image properties."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class ImagePolicy(unittest.TestCase):
    def test_fixed_depth_preserves_image_contract_and_vulkan_11_chain(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        start = src.index('static int ubwc_mode(')
        helper = src[start:src.index('static vkimg_t *img_new(', start)]
        code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
static char *no_environment(const char *name) { (void)name; return NULL; }
#define getenv no_environment
''' + helper + r'''
int main(void) {
    assert(ubwc_mode() == 3);
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageStencilUsageCreateInfo tail = {VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    const VkImageCreateFlags kept = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    for (int compressed = 0; compressed < 2; ++compressed)
        for (int color = 0; color < 2; ++color)
            for (int mode = 0; mode < 4; ++mode)
                for (unsigned count = 1; count <= 2; ++count) {
                    VkImageCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                    ci.pNext = &tail; ci.flags = kept | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
                    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = color ? formats[0] : VK_FORMAT_D32_SFLOAT;
                    ci.extent = (VkExtent3D){1280, 720, 1}; ci.mipLevels = 4; ci.arrayLayers = 6;
                    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    VkImageCreateInfo before = ci;
                    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, count, formats};
                    int changed = image_format_policy(&ci, compressed, color, mode, &list);
                    int enabled = !compressed && (color ? (mode == 1 || mode == 3) : (mode == 2 || mode == 3));
                    assert(changed == enabled);
                    if (!enabled) { assert(!memcmp(&ci, &before, sizeof ci)); assert(list.pNext == NULL); continue; }
                    assert((ci.flags & kept) == kept && !(ci.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT));
                    assert(!!(ci.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == (color && count == 2));
                    if (color) { assert(ci.pNext == &list && list.pNext == &tail); }
                    else { assert(ci.pNext == &tail && list.pNext == NULL); }
                    // Format, size, levels, layers, usage and all other properties are unchanged.
                    ci.pNext = before.pNext; ci.flags = before.flags;
                    assert(!memcmp(&ci, &before, sizeof ci));
                }
    VkImageCreateInfo plain = {0}; plain.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    plain.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkImageFormatListCreateInfo one = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 1, formats};
    assert(image_format_policy(&plain, 0, 0, 2, &one) && plain.pNext == NULL && plain.flags == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-image-policy-') as temp:
            c = Path(temp) / 'policy.c'
            exe = Path(temp) / 'policy'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
