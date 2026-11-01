"""Non-mipmapped filters retain level zero and distinct min/mag filtering."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class SamplerLod(unittest.TestCase):
    def test_production_sampler_ignores_lod_clamps_only_without_mipmaps(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        source = (DM / 'android/gfx_vk_tex.c').read_text(encoding='utf8')
        start = source.index('static VkSamplerAddressMode wrap(')
        functions = source[start:source.index('void mtl_reg_samp(', start)]
        objects = (DM / 'gfx/nvn/nvn_obj.h').read_text(encoding='utf8')
        params = re.search(r'typedef struct \{[^\n]+\} samp_par;', objects).group()
        prefix = r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "nvn_hmap.h"
static struct { VkDevice dev; float max_aniso; int has_custom_border; } V;
static VkSamplerCreateInfo captured;
static uintptr_t next_sampler;
static VkResult record_sampler(VkDevice device, const VkSamplerCreateInfo *info,
                              const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    (void)device; (void)allocator;
    // The original positive guest minimum exceeded the non-mipmapped maximum.
    assert(info->minLod <= info->maxLod);
    captured = *info;
    *sampler = (VkSampler)++next_sampler;
    return VK_SUCCESS;
}
#define vkCreateSampler record_sampler
'''
        suffix = r'''
int main(void) {
    const float minima[] = {-1000.0f, -0.5f, 0.0f, 4.0f, 12.0f};
    const float maxima[] = {32.0f, 1000.0f};
    V.max_aniso = 16.0f;
    for (unsigned minf = 0; minf < 6; ++minf)
        for (unsigned magf = 0; magf < 2; ++magf)
            for (unsigned low = 0; low < sizeof minima / sizeof *minima; ++low)
                for (unsigned high = 0; high < sizeof maxima / sizeof *maxima; ++high) {
                    samp_par p = {0};
                    p.minf = (uint8_t)minf; p.magf = (uint8_t)magf;
                    p.lod_min = minima[low]; p.lod_max = maxima[high];
                    p.lod_bias = -0.125f; p.aniso = 1.0f;
                    samp_par before = p;
                    assert(make_sampler(&p) != VK_NULL_HANDLE);
                    assert(!memcmp(&p, &before, sizeof p));
                    assert(captured.magFilter == (magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
                    assert(captured.minFilter == (minf & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
                    assert(captured.mipmapMode == (minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST));
                    assert(captured.mipLodBias == p.lod_bias);
                    if (minf < 2) {
                        // Vulkan's GL_NEAREST/GL_LINEAR emulation: lambda can
                        // select minFilter but always rounds to mip level zero.
                        assert(captured.minLod == 0.0f);
                        assert(captured.maxLod > 0.0f && captured.maxLod < 0.5f);
                    } else {
                        assert(captured.minLod == p.lod_min);
                        assert(captured.maxLod == p.lod_max);
                    }
                }
    assert(!vk_samp_compare_constant(VK_NULL_HANDLE));
    for (unsigned mode=0;mode<2;mode++)for(unsigned function=1;function<=8;function++) {
        samp_par p={0};p.cmp_mode=(uint8_t)mode;p.cmp_func=(uint8_t)function;p.aniso=1.0f;
        VkSampler sampler=make_sampler(&p);
        assert(vk_samp_compare_constant(sampler)==(mode==1&&(function==1||function==8)));
    }
    assert(!vk_samp_compare_constant((VkSampler)(uintptr_t)0xdeadbeef));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-sampler-lod-') as temp:
            path = Path(temp)
            c = path / 'sampler.c'
            exe = path / 'sampler.exe'
            c.write_text(prefix + params + '\n' + functions + suffix, encoding='utf8')
            subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-missing-field-initializers', '-pthread', '-I', str(DM / 'gfx/nvn'), '-idirafter', str(headers),
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
