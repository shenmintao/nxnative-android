"""Comparison lookups on colour views: a driver's declaration first, then a defined depth snapshot, then the colour view (undefined)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / 'nxnative/dm/android/gfx_vk_tex.c'
CORE = ROOT / 'nxnative/dm/android/gfx_vk_core.c'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class CompareTiers(unittest.TestCase):
    def test_tiers(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        source = TEX.read_text(encoding='utf8')
        start = source.index('static int cmp_color_on(void) {')   # one line
        functions = [source[start:source.index('\n', start)], function(source, 'VkImageView vk_tex_compare_view_mode(')]
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define H(t,v) ((t)(uintptr_t)(v))
static int logs, reds;
#define VKLOG(...) (logs++)
#define VKRED_ONCE(id, ...) (reds++)
typedef struct { int dummy; } tex_t; typedef struct { int dummy; } view_t; typedef struct { int dummy; } pthread_mutex_t;
typedef struct vkimg { VkImage img; VkFormat fmt; VkImageAspectFlags aspect; uint32_t nvn_format, w, h, d, layers, samples; VkImageViewType vtype; } vkimg_t;
typedef struct { int compressed; unsigned bpb; } vkfmt_t;
typedef struct vview { VkImageView v; VkImageViewType type; VkFormat fmt; uint32_t lv0, nlv, ly0, nly; VkComponentMapping swz; VkImageAspectFlags aspect; int attach; VkImageUsageFlags usage; struct vview *next; } vview;
static struct { VkDevice dev; int cmp_color[2], has_depth_range_unrestricted; } V;
static VkFormat vk_view_fmt; static pthread_mutex_t g_view_mu; static int g_views;
static uint64_t g_compare_failures, g_compare_direct, g_compare_colour;
static vkimg_t img; static vkfmt_t base; static vview source; static VkFormat depth_format; static int depth_calls;
static void pthread_mutex_lock(pthread_mutex_t *m) { (void)m; }
static void pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; }
static VkImageView vk_tex_view(const tex_t *t, const view_t *vw, int dim, int arrayed, int flags, vkimg_t **out) {
    (void)t; (void)vw; (void)dim; (void)arrayed; (void)flags; *out = &img; vk_view_fmt = img.fmt; return H(VkImageView, 0x10);
}
static const vkfmt_t *vk_fmt(uint32_t f) { (void)f; return &base; }
static vview *nvn_hm_get(int *map, vkimg_t **key) { (void)map; return *key == &img ? &source : NULL; }
static void nvn_hm_put(int *map, vkimg_t **key, vview *v) { (void)map; (void)key; (void)v; }
static vkimg_t *compare_depth(vkimg_t *src, VkFormat format, unsigned bpp, int constant) {
    (void)src; (void)constant; assert(bpp == (format == VK_FORMAT_D16_UNORM ? 2u : 4u)); depth_format = format; depth_calls++; return NULL;   // stops at the snapshot
}
static VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const void *a, VkImageView *out) { (void)d; (void)ci; (void)a; *out = 0; return VK_SUCCESS; }
static void vk_defer_free_view(VkImageView v) { (void)v; }
''' + '\n'.join(functions) + r'''
static VkImageView look(VkFormat f, int constant) {
    img = (vkimg_t){H(VkImage, 1), f, VK_IMAGE_ASPECT_COLOR_BIT, 1, 64, 64, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D};
    base = (vkfmt_t){0, f == VK_FORMAT_R16_UNORM ? 2u : 4u}; source.v = H(VkImageView, 0x10);
    depth_calls = 0; depth_format = VK_FORMAT_UNDEFINED; vkimg_t *out = NULL;
    VkImageView v = vk_tex_compare_view_mode(NULL, NULL, 1, 0, &out, constant); assert(out == &img); return v;
}
int main(int argc, char **argv) {
    const VkImageView ordinary = H(VkImageView, 0x10);
    if (argc > 1 && !strcmp(argv[1], "off")) {   // DM_VK_CMP_COLOR=0: the snapshot rules alone; the undefined fallback fails closed as before
        V.cmp_color[0] = V.cmp_color[1] = 1;
        assert(!look(VK_FORMAT_R32_SFLOAT, 0) && reds == 1 && g_compare_failures == 1 && !g_compare_colour && !g_compare_direct);
        assert(!look(VK_FORMAT_R16_UNORM, 0) && depth_calls == 1 && depth_format == VK_FORMAT_D16_UNORM && !g_compare_direct);
        return 0;
    }
    // 1. declared by the driver: the colour view as it is, no snapshot
    V.cmp_color[0] = V.cmp_color[1] = 1;
    assert(look(VK_FORMAT_R32_SFLOAT, 0) == ordinary && !depth_calls && g_compare_direct == 1);
    assert(look(VK_FORMAT_R16_UNORM, 0) == ordinary && !depth_calls && g_compare_direct == 2);
    // 2. undeclared: R16_UNORM always has a defined D16 snapshot; R32_SFLOAT with the extension or a constant comparison has D32
    V.cmp_color[0] = V.cmp_color[1] = 0;
    assert(!look(VK_FORMAT_R16_UNORM, 0) && depth_calls == 1 && depth_format == VK_FORMAT_D16_UNORM);
    assert(!look(VK_FORMAT_R32_SFLOAT, 1) && depth_calls == 1 && depth_format == VK_FORMAT_D32_SFLOAT);
    V.has_depth_range_unrestricted = 1;
    assert(!look(VK_FORMAT_R32_SFLOAT, 0) && depth_calls == 1 && depth_format == VK_FORMAT_D32_SFLOAT);
    // 3. neither: R32_SFLOAT through its colour view, reported once, no failure
    V.has_depth_range_unrestricted = 0; int before = logs; uint64_t failures = g_compare_failures;
    assert(look(VK_FORMAT_R32_SFLOAT, 0) == ordinary && !depth_calls && g_compare_colour == 1);
    assert(look(VK_FORMAT_R32_SFLOAT, 0) == ordinary && g_compare_colour == 2 && logs == before + 1 && g_compare_failures == failures);
    // a declaration for one format does not leak to the other
    V.cmp_color[1] = 1;
    assert(look(VK_FORMAT_R32_SFLOAT, 0) == ordinary && g_compare_colour == 3 && g_compare_direct == 2);
    // other colour formats still have no representation
    reds = 0; assert(!look(VK_FORMAT_R8G8B8A8_UNORM, 0) && reds == 1 && !depth_calls);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-compare-tiers-') as directory:
            path = Path(directory)
            (path / 'tiers.c').write_text(code, encoding='utf8')
            exe = path / 'tiers.exe'
            built = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                                    '-Wno-unused-function', '-Wno-unused-variable', '-idirafter', str(headers), str(path / 'tiers.c'), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for mode, env in (('on', {}), ('off', {'DM_VK_CMP_COLOR': '0'})):
                import os
                checked = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=10, env={**os.environ, **env})
                self.assertEqual(checked.returncode, 0, mode + checked.stdout + checked.stderr)

    def test_driver_declaration_is_queried_and_reported(self):
        core = CORE.read_text(encoding='utf8')
        self.assertIn('VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT', core)
        self.assertIn('optimalTilingFeatures', core)
        self.assertIn('"depth comparison declared on colour views:', core)
        self.assertIn('X(vkGetPhysicalDeviceFormatProperties2)', (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf8'))


if __name__ == '__main__':
    unittest.main()
