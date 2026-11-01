"""Production color-image widening: ownership, complete copies, and publication order."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}', start) + 3]


class ColorPromotion(unittest.TestCase):
    def test_creation_fallback_and_absent_format_list_support(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        a = src.index('    VkImageCreateInfo unrestricted = ci;')
        b = src.index('    if (r) { VKRED_ONCE(10,', a)
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define VKLOG(...) ((void)0)
static int mode_now, fail, calls;
static struct { int has_format_list; VkDevice dev; } V;
typedef struct { VkImage img; VkImageAspectFlags aspect; int fixed_fmt; VkFormat fmt2; VkImageUsageFlags usage; VkImageCreateFlags cflags; } vkimg_t;
typedef struct { int compressed; } vkfmt_t;
static VkImageCreateInfo before, final;
static int ubwc_mode(void) { return mode_now; }
static VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a; calls++; final=*ci;
    if (calls==2) assert(!memcmp(ci,&before,sizeof before));
    if (fail==2 || (fail==1 && calls==1)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=(VkImage)(uintptr_t)1; return VK_SUCCESS;
}
''' + function(src, 'static int image_format_policy(') + r'''
static VkResult create(vkimg_t *im, const vkfmt_t *f, VkImageCreateInfo ci) {
    VkFormat fl[2]={ci.format,VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo fli={VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,NULL,2,fl};
''' + src[a:b] + r'''
    return r;
}
int main(void) {
    VkImageStencilUsageCreateInfo tail={VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,NULL,VK_IMAGE_USAGE_SAMPLED_BIT};
    for (int support=0;support<2;++support) for (mode_now=0;mode_now<4;++mode_now)
    for (int color=0;color<2;++color) for (int compressed=0;compressed<2;++compressed) for (fail=0;fail<3;++fail) {
        V.has_format_list=support; calls=0; vkfmt_t f={compressed};
        vkimg_t im={0}; im.aspect=color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageCreateInfo ci={0}; ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.pNext=&tail;
        ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.format=color ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_D32_SFLOAT;
        ci.extent=(VkExtent3D){64,32,1}; ci.mipLevels=4; ci.arrayLayers=6; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        before=ci;
        int restricted=!compressed && (color ? support && (mode_now&1) : mode_now&2);
        int retry=fail && restricted && color;
        VkResult r=create(&im,&f,ci);
        assert(calls==1+retry && !!im.fixed_fmt==(restricted && !retry));
        assert((r==VK_SUCCESS)==(!fail || (fail==1 && retry)));
        assert(im.cflags==final.flags && im.usage==final.usage);
        if (!restricted || retry) assert(!memcmp(&final,&before,sizeof before));
        if (retry) assert(im.fmt2==VK_FORMAT_UNDEFINED);
        if (restricted && !color) assert(final.pNext==&tail && final.flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-color-create-') as temp:
            c = Path(temp) / 'create.c'; exe = Path(temp) / 'create'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_resources_subresources_and_ordered_publication(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        h = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        start = h.index('struct vkimg {')
        image = 'typedef struct vkimg vkimg_t;\n' + h[start:h.index('\n};', start) + 4]
        prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define H(t,n) ((t)(uintptr_t)(n))
#define VKRED_ONCE(id,...) do { if (0) printf(__VA_ARGS__); } while (0)
''' + image + r'''
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static pthread_mutex_t g_present_mu = PTHREAD_MUTEX_INITIALIZER, g_view_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_tex_gen, g_tex_frame = 1, g_vk_image_gen, g_color_promotions, g_color_promotion_failures;
static vkimg_t *logical;
typedef struct vview { VkImageView v; struct vview *next; } vview;
static vview *cached;
static int g_views;
static void *nvn_hm_get(int *map, const void *key) { assert(map==&g_views && *(vkimg_t *const *)key==logical); return cached; }
static void nvn_hm_del(int *map, const void *key) { assert(map==&g_views && *(vkimg_t *const *)key==logical); cached=NULL; }
static unsigned creates, destroyed, allocations, binds, copies, barriers, preparations, restarts, landed, defviews, defimages;
static int fail, suballocate, active=1, publication;
static VkImageCreateInfo made;
static VkDeviceSize bound_offset;
static void vk_defer_free_view(VkImageView v) {
    assert(v && (!publication || restarts==2)); defviews++;
}
static void vk_defer_free_image(VkImage i, VkDeviceMemory m) {
    assert(i && m); if (publication) assert(restarts==2 && defviews==4); defimages++;
}
static void vk_defer_free_image_sub(VkImage i, void *sub) {
    assert(i && sub); if (publication) assert(restarts==2 && defviews==4); defimages++;
}
static VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a; creates++; made=*ci;
    if (fail==1) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=H(VkImage,10); return VK_SUCCESS;
}
static void vkGetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *r) {
    (void)d; assert(i==H(VkImage,10)); *r=(VkMemoryRequirements){4096,256,1};
}
static uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    assert(bits==1 && want==VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && avoid==VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    return fail==2 ? UINT32_MAX : 0;
}
static VkDeviceMemory vk_image_memory(const VkMemoryRequirements *r, uint32_t type, VkDeviceSize *offset, void **sub) {
    assert(r->size==4096 && type==0); allocations++;
    *offset=suballocate ? 512 : 0; *sub=suballocate ? (void *)(uintptr_t)7 : NULL;
    return fail==3 ? VK_NULL_HANDLE : H(VkDeviceMemory,20);
}
static void vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)a; assert(i==H(VkImage,10)); destroyed++; }
static VkResult vkBindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize offset) {
    (void)d; assert(i==H(VkImage,10) && m==H(VkDeviceMemory,20)); binds++; bound_offset=offset;
    return fail==4 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static int vk_sub_open(void) { return active; }
static void vk_end_pass_land(void) { assert(logical->img==H(VkImage,1)); landed++; }
static void vk_sub_restart(void) {
    assert(logical->img==H(VkImage,1) && !g_vk_image_gen);
    assert(restarts<2 && (!restarts || copies==logical->levels)); restarts++;
}
static VkCommandBuffer vk_cmd(void) { assert(restarts==1); return H(VkCommandBuffer,30); }
static uint64_t mtl_last_fence(void) { return 100; }
static void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im) {
    assert(cb==H(VkCommandBuffer,30)); preparations++; im->layout=VK_IMAGE_LAYOUT_GENERAL;
}
static void vk_img_barrier_all(VkCommandBuffer cb) { assert(cb==H(VkCommandBuffer,30)); barriers++; }
static void vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    assert(cb==H(VkCommandBuffer,30) && src==H(VkImage,1) && dst==H(VkImage,10));
    assert(sl==VK_IMAGE_LAYOUT_GENERAL && dl==VK_IMAGE_LAYOUT_GENERAL && n==1 && barriers==1);
    unsigned lv=copies++;
    assert(r->srcSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && r->srcSubresource.mipLevel==lv);
    assert(r->srcSubresource.layerCount==logical->layers && !r->srcSubresource.baseArrayLayer);
    assert(!memcmp(&r->srcSubresource,&r->dstSubresource,sizeof r->srcSubresource));
    assert(!r->srcOffset.x && !r->srcOffset.y && !r->srcOffset.z && !memcmp(&r->srcOffset,&r->dstOffset,sizeof r->srcOffset));
    unsigned w=logical->w>>lv,h=logical->h>>lv,d=logical->d>>lv;
    assert(r->extent.width==(w?w:1) && r->extent.height==(h?h:1) && r->extent.depth==(d?d:1));
}
'''
        code = function(src, 'static void vk_tex_view_drop_all(vkimg_t *im) {')
        for signature in ('static int color_image_make_wide(', 'static void color_image_copy(', 'static int color_image_require(', 'static int color_image_widen('):
            code += function(src, signature)
        main = r'''
static vkimg_t original(void) {
    vkimg_t im={0}; im.img=H(VkImage,1); im.mem=H(VkDeviceMemory,2); im.full=H(VkImageView,9);
    im.fmt=VK_FORMAT_R8G8B8A8_UNORM; im.vtype=VK_IMAGE_VIEW_TYPE_2D; im.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    im.w=64; im.h=32; im.d=1; im.layers=1; im.levels=4; im.samples=1; im.fixed_fmt=1;
    im.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    im.packaged=3; im.pkg_stamp=77; im.pool=123; im.off=456; im.size=8192; im.gpu_w=1; im.wseq=987;
    im.pkg_mem=(const uint8_t *)(uintptr_t)44; im.lin_mem=(const uint8_t *)(uintptr_t)55;
    im.ovl[0]=(vkimg_t *)(uintptr_t)66; im.novl=1; im.last_use=200;
    return im;
}
static void reset(void) {
    creates=destroyed=allocations=binds=copies=barriers=preparations=restarts=landed=defviews=defimages=0;
    g_vk_image_gen=g_color_promotions=g_color_promotion_failures=0; fail=suballocate=publication=0; active=1;
}
int main(void) {
    for (int sub=0;sub<2;sub++) for(int error=1;error<=4;error++) {
        reset(); suballocate=sub; fail=error; vkimg_t src=original(), before=src, out;
        memset(&out,0xa5,sizeof out); vkimg_t poison=out;
        assert(!color_image_make_wide(&src,&out)); assert(!memcmp(&out,&poison,sizeof out)); assert(!memcmp(&src,&before,sizeof src));
        assert(!copies && !barriers && !preparations);
        assert(destroyed==(unsigned)(error==2 || error==3)); assert(defimages==(unsigned)(error==4));
    }
    for(int bad=0;bad<7;bad++) {
        reset(); vkimg_t src=original(), out;
        if(bad==0)src.w=0; if(bad==1)src.layers=0; if(bad==2)src.levels=33;
        if(bad==3)src.cflags=VK_IMAGE_CREATE_PROTECTED_BIT; if(bad==4)src.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
        if(bad==5)src.usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; if(bad==6)src.samples=0;
        assert(!color_image_make_wide(&src,&out) && !creates);
    }
    const VkImageViewType shapes[]={VK_IMAGE_VIEW_TYPE_1D,VK_IMAGE_VIEW_TYPE_1D_ARRAY,VK_IMAGE_VIEW_TYPE_2D,
        VK_IMAGE_VIEW_TYPE_2D_ARRAY,VK_IMAGE_VIEW_TYPE_3D,VK_IMAGE_VIEW_TYPE_CUBE,VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
    for(unsigned shape=0;shape<sizeof shapes/sizeof *shapes;shape++) for(int sub=0;sub<2;sub++) {
        reset(); suballocate=sub; vkimg_t src=original(), dst; logical=&src; src.vtype=shapes[shape];
        if(shape<2)src.h=1; if(shape==1 || shape==3 || shape>=5)src.layers=shape==6?12:6;
        if(shape==4) {src.d=8;src.cflags=VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;}
        if(shape>=5)src.cflags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        assert(color_image_make_wide(&src,&dst)); assert(bound_offset==(sub?512u:0u));
        assert(made.flags==(src.cflags|VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT));
        assert(made.pNext==NULL && made.format==src.fmt && made.usage==src.usage && made.arrayLayers==src.layers && made.mipLevels==src.levels);
        assert(made.imageType==(shape==4?VK_IMAGE_TYPE_3D:shape<2?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D));
        color_image_copy(H(VkCommandBuffer,30),&src,&dst); assert(copies==src.levels && barriers==2 && preparations==2);
    }
    for(int sub=0;sub<2;sub++) {
        reset(); publication=1; suballocate=sub; vkimg_t im=original(), before=im; logical=&im;
        if(sub) im.sub=before.sub=(void *)(uintptr_t)8;
        for(unsigned i=0;i<3;i++) {vview *v=calloc(1,sizeof *v);assert(v);v->v=H(VkImageView,40+i);v->next=cached;cached=v;}
        assert(color_image_widen(&im)); assert(restarts==2 && landed==1 && copies==before.levels && defviews==4 && defimages==1 && !cached);
        assert(im.img==H(VkImage,10) && im.mem==H(VkDeviceMemory,20) && !im.fixed_fmt && !im.full && !im.fmt2);
        assert(g_vk_image_gen==1 && g_color_promotions==1 && !g_color_promotion_failures && im.last_use==before.last_use);
        // All logical storage, dirty-page, alias and write-history fields survive.
        im.img=before.img;im.mem=before.mem;im.sub=before.sub;im.full=before.full;
        im.usage=before.usage;im.cflags=before.cflags;im.layout=before.layout;im.fixed_fmt=before.fixed_fmt;
        assert(!memcmp(&im,&before,sizeof im));
    }
    const VkImageUsageFlags extra[]={VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_STORAGE_BIT};
    for(unsigned use=0;use<sizeof extra/sizeof *extra;use++) {
        reset(); publication=1; vkimg_t im=original(); im.fixed_fmt=0;
        im.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        im.cflags|=VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT; im.fmt=VK_FORMAT_BC4_UNORM_BLOCK;
        logical=&im; VkImageUsageFlags before_usage=im.usage;
        for(unsigned i=0;i<3;i++){vview*v=calloc(1,sizeof *v);assert(v);v->v=H(VkImageView,40+i);v->next=cached;cached=v;}
        assert(color_image_require(&im,extra[use]));
        assert(made.format==VK_FORMAT_BC4_UNORM_BLOCK && made.usage==(before_usage|extra[use]));
        assert((made.flags&VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && im.usage==made.usage);
        assert(restarts==2 && landed==1 && copies==im.levels && defviews==4 && defimages==1 && !cached);
        assert(color_image_require(&im,extra[use]) && creates==1 && restarts==2);
    }
    reset(); vkimg_t im=original(); logical=&im; fail=1;
    assert(!color_image_widen(&im) && creates==1 && g_color_promotion_failures==1);
    assert(!color_image_widen(&im) && creates==1 && !restarts); g_tex_frame++;
    assert(!color_image_widen(&im) && creates==2 && g_color_promotion_failures==2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-color-promotion-') as temp:
            c = Path(temp) / 'check.c'; exe = Path(temp) / 'check'
            c.write_text(prefix + code + main, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-misleading-indentation', '-pthread', '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
