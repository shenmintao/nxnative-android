"""Compressed storage may expose each block through a size-compatible view."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


ROOT = Path(__file__).resolve().parents[1]


class BlockViews(unittest.TestCase):
    def test_production_format_extent_and_usage_selection(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf8')
        start = source.index('static VkImageUsageFlags image_view_usage(')
        helper = source[start:source.index('\n}', start)+3]
        start = source.index('    VkFormat fmt = f->vk;', source.index('VkImageView vk_tex_view('))
        selection = source[start:source.index('    pthread_mutex_lock(&g_view_mu);', start)]
        prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define NVN_TEXTURE_FLAGS_IMAGE 4
#define VKRED_ONCE(...) (++errors)
typedef struct { struct {uint32_t flags;} d; } tex_t;
typedef struct { VkFormat vk; uint32_t bpb,bw,bh; int compressed,depth; } vkfmt_t;
typedef struct { VkFormat fmt,fmt2; VkImageAspectFlags aspect; uint32_t w,h,nvn_format; VkImageCreateFlags cflags; VkImageUsageFlags usage; int fixed_fmt; } vkimg_t;
static struct {VkPhysicalDevice pd;} V;
static unsigned errors,queries,promotions;
static int refuse;
static VkFormatFeatureFlags features;
static vkfmt_t source_format;
static uint32_t vk_view_width,vk_view_height;
static VkFormat selected_format;
static const vkfmt_t *vk_fmt(uint32_t n){(void)n;return &source_format;}
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,VkFormat f,VkFormatProperties *out){
    (void)p;(void)f;queries++;memset(out,0,sizeof *out);out->optimalTilingFeatures=features;
}
static int color_wide_forced(void){return 0;}
static int color_image_widen(vkimg_t *im){im->fixed_fmt=0;return 1;}
static int color_image_require(vkimg_t *im,VkImageUsageFlags required){
    promotions++;if(refuse)return 0;im->usage|=required;return 1;
}
'''
        body = r'''
static VkImageView select_view(vkimg_t *im,const vkfmt_t *f,const tex_t *t,
                               unsigned lv0,unsigned nlv,unsigned nly,int as_attachment){
'''
        suffix = r'''
    selected_format=fmt;return (VkImageView)(uintptr_t)1;
}
int main(void){
    const unsigned sizes[]={1,2,3,4,7,11,32,63};
    const unsigned blocks[][2]={{4,4},{5,4},{12,12}};
    const VkImageUsageFlags initial=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageCreateFlags flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    features=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    for(unsigned bytes=8;bytes<=16;bytes+=8)for(unsigned b=0;b<3;b++)
        for(unsigned x=0;x<8;x++)for(unsigned y=0;y<8;y++)for(unsigned lv=0;lv<6;lv++){
            source_format=(vkfmt_t){VK_FORMAT_BC4_UNORM_BLOCK,bytes,blocks[b][0],blocks[b][1],1,0};
            vkfmt_t view={bytes==8?VK_FORMAT_R16G16B16A16_UINT:VK_FORMAT_R32G32B32A32_UINT,bytes,1,1,0,0};
            vkimg_t im={.fmt=source_format.vk,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.w=sizes[x],.h=sizes[y],.cflags=flags,.usage=initial};
            tex_t t={{0}};unsigned before=promotions;
            assert(select_view(&im,&view,&t,lv,1,1,1));
            unsigned w=im.w>>lv,h=im.h>>lv;if(!w)w=1;if(!h)h=1;
            assert(selected_format==view.vk&&vk_view_width==(w+blocks[b][0]-1)/blocks[b][0]);
            assert(vk_view_height==(h+blocks[b][1]-1)/blocks[b][1]);
            assert(im.w==sizes[x]&&im.h==sizes[y]&&im.fmt==source_format.vk);
            assert(im.usage==(initial|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)&&promotions==before+1);
            assert(select_view(&im,&view,&t,lv,1,1,1)&&promotions==before+1);
            t.d.flags=NVN_TEXTURE_FLAGS_IMAGE;
            assert(select_view(&im,&view,&t,lv,1,1,0));
            assert(im.usage==(initial|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_STORAGE_BIT));
        }
    vkfmt_t view={VK_FORMAT_R16G16B16A16_UINT,8,1,1,0,0};
    source_format=(vkfmt_t){VK_FORMAT_BC4_UNORM_BLOCK,8,4,4,1,0};
    tex_t t={{0}};vkimg_t im={.fmt=source_format.vk,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.w=32,.h=32,.cflags=flags,.usage=initial};
    unsigned before=promotions;
    assert(!select_view(&im,&view,&t,0,2,1,1));
    assert(!select_view(&im,&view,&t,0,1,2,1));
    view.bpb=16;assert(!select_view(&im,&view,&t,0,1,1,1));view.bpb=8;
    im.cflags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    assert(!select_view(&im,&view,&t,0,1,1,1));im.cflags=flags;
    assert(promotions==before&&errors==4);
    features=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    assert(!select_view(&im,&view,&t,0,1,1,1)&&promotions==before);
    features=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;refuse=1;
    assert(!select_view(&im,&view,&t,0,1,1,1)&&im.usage==initial);refuse=0;
    // Ordinary same-format cached views do not acquire a per-lookup format query.
    source_format=(vkfmt_t){VK_FORMAT_R8G8B8A8_UNORM,4,1,1,0,0};view=source_format;
    im.fmt=source_format.vk;im.cflags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;unsigned q=queries;
    assert(select_view(&im,&view,&t,0,3,6,1)&&queries==q&&vk_view_width==32&&vk_view_height==32);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-block-view-') as temp:
            path = Path(temp)
            c = path / 'block.c'
            exe = path / 'block.exe'
            c.write_text(prefix + helper + body + selection + suffix, encoding='utf8')
            subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
