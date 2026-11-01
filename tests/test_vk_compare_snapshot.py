"""Exercise production snapshot lifetime, subresource layout and failed allocation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


class CompareSnapshot(unittest.TestCase):
    def test_allocation_failures_copy_order_and_write_invalidation(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf8')
        start = header.index('struct vkimg {')
        image = header[start:header.index('\n};', start) + 4]
        start = source.index('struct vkcompare {')
        declaration = source[start:source.index('\n', start)]
        functions = []
        for signature in ('void vk_tex_written(', 'static void compare_drop(vkimg_t *im) {',
                          'static VkDeviceSize compare_regions(', 'static int compare_make(', 'static int compare_make_constant(',
                          'static void compare_refresh(', 'static vkimg_t *compare_depth('):
            start = source.index(signature)
            functions.append(source[start:source.index('\n}', start) + 3])
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define H(t,v) ((t)(uintptr_t)(v))
typedef struct vkimg vkimg_t;
''' + image + '\n' + declaration + r'''
static struct { VkDevice dev; } V;
static uint64_t g_tex_gen=1,g_vk_image_gen,g_compare_copies,g_compare_bytes,g_compare_hits;
static uint64_t g_const_compare_created,g_const_compare_reads,g_const_compare_clears;
static unsigned fail,types,images,buffers,memories,suballocate,restarts,barriers,recorded;
static char events[32];
static vkimg_t *current;
static void event(char c){assert(recorded<sizeof events-1);events[recorded++]=c;events[recorded]=0;}
static void vk_tex_view_drop_all(vkimg_t *im){assert(im!=current);g_tex_gen++;}
static void vk_defer_free_image(VkImage im,VkDeviceMemory mem){if(im){assert(images);images--;}if(mem){assert(memories);memories--;}}
static void vk_defer_free_image_sub(VkImage im,void *sub){assert(sub);vk_defer_free_image(im,H(VkDeviceMemory,1));}
static void vk_defer_free_buffer(VkBuffer b,VkDeviceMemory m){if(b){assert(buffers);buffers--;}if(m){assert(memories);memories--;}}
static VkResult vkCreateImage(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out){
    (void)d;assert(!a&&ci->flags==0&&ci->imageType==VK_IMAGE_TYPE_2D&&ci->format==VK_FORMAT_D16_UNORM);
    assert(ci->extent.width==11&&ci->extent.height==7&&ci->extent.depth==1&&ci->mipLevels==3&&ci->arrayLayers==3&&ci->samples==1);
    assert(ci->usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    if(fail==1)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=H(VkImage,2);images++;return VK_SUCCESS;
}
static void vkGetImageMemoryRequirements(VkDevice d,VkImage im,VkMemoryRequirements *r){(void)d;assert(im);*r=(VkMemoryRequirements){4096,4096,1};}
static uint32_t vk_mem_type(uint32_t bits,VkMemoryPropertyFlags wanted,VkMemoryPropertyFlags avoid){
    assert(bits==1&&wanted==VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT&&!avoid);types++;
    return (types==1&&fail==2)||(types==2&&fail==6)?UINT32_MAX:0;
}
static VkDeviceMemory vk_image_memory(const VkMemoryRequirements *r,uint32_t mt,VkDeviceSize *off,void **sub){
    assert(r->size==4096&&mt==0);*off=0;*sub=NULL;if(fail==3)return VK_NULL_HANDLE;
    if(suballocate)*sub=(void *)(uintptr_t)1;memories++;return H(VkDeviceMemory,3);
}
static VkResult vkBindImageMemory(VkDevice d,VkImage im,VkDeviceMemory m,VkDeviceSize off){(void)d;assert(im&&m&&!off);return fail==4?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
static VkResult vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *ci,const VkAllocationCallbacks *a,VkBuffer *out){
    (void)d;assert(!a&&ci->size==568&&ci->usage==(VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    if(fail==5)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=H(VkBuffer,4);buffers++;return VK_SUCCESS;
}
static void vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r){(void)d;assert(b);*r=(VkMemoryRequirements){1024,256,1};}
static VkResult vk_buffer_memory_allocate(const VkMemoryAllocateInfo *ai,VkDeviceMemory *out){
    assert(ai->allocationSize==1024&&ai->memoryTypeIndex==0);if(fail==7)return VK_ERROR_OUT_OF_DEVICE_MEMORY;memories++;*out=H(VkDeviceMemory,5);return VK_SUCCESS;
}
static VkResult vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize off){(void)d;assert(b&&m&&!off);return fail==8?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
static void vk_end_pass_land(void){event('E');}
static VkCommandBuffer vk_cmd(void){return H(VkCommandBuffer,1);}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im){assert(cb&&im);event(im==current?'S':'D');im->layout=VK_IMAGE_LAYOUT_GENERAL;}
static void vk_img_barrier_all(VkCommandBuffer cb){assert(cb);barriers++;event('B');}
static void vkCmdClearDepthStencilImage(VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue *value,uint32_t count,const VkImageSubresourceRange *range){
    assert(cb&&image==current->compare->depth.img&&layout==VK_IMAGE_LAYOUT_GENERAL&&count==1);
    assert(value->depth==0&&value->stencil==0&&range->aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
    assert(range->baseMipLevel==0&&range->levelCount==3&&range->baseArrayLayer==0&&range->layerCount==3);event('Z');
}
static void check_regions(uint32_t n,const VkBufferImageCopy *r,VkImageAspectFlags aspect){
    assert(n==3);const VkDeviceSize offsets[]={0,464,556};const unsigned widths[]={11,5,2},heights[]={7,3,1};
    for(unsigned i=0;i<n;i++)assert(r[i].bufferOffset==offsets[i]&&!r[i].bufferRowLength&&!r[i].bufferImageHeight&&
        r[i].imageSubresource.aspectMask==aspect&&r[i].imageSubresource.mipLevel==i&&r[i].imageSubresource.baseArrayLayer==0&&
        r[i].imageSubresource.layerCount==3&&r[i].imageExtent.width==widths[i]&&r[i].imageExtent.height==heights[i]&&r[i].imageExtent.depth==1);
}
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage src,VkImageLayout layout,VkBuffer b,uint32_t n,const VkBufferImageCopy *r){
    assert(cb&&src==current->img&&layout==VK_IMAGE_LAYOUT_GENERAL&&b==current->compare->buffer&&barriers==1);check_regions(n,r,VK_IMAGE_ASPECT_COLOR_BIT);event('I');
}
static void vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer b,VkImage dst,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *r){
    assert(cb&&dst==current->compare->depth.img&&layout==VK_IMAGE_LAYOUT_GENERAL&&b==current->compare->buffer&&barriers==2);check_regions(n,r,VK_IMAGE_ASPECT_DEPTH_BIT);event('C');
}
static void vk_sub_restart(void){assert(current->compare->valid);restarts++;event('T');}
static void compare_drop(vkimg_t *im);
''' + '\n'.join(functions) + r'''
int main(void){
    vkimg_t im={0};im.img=H(VkImage,1);im.fmt=VK_FORMAT_R16_UNORM;im.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    im.w=11;im.h=7;im.d=1;im.layers=3;im.levels=3;im.samples=1;im.vtype=VK_IMAGE_VIEW_TYPE_2D_ARRAY;im.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    current=&im;VkBufferImageCopy r[32];assert(compare_regions(&im,2,r)==568);check_regions(3,r,VK_IMAGE_ASPECT_COLOR_BIT);
    for(unsigned bad=0;bad<8;bad++){
        vkimg_t wrong=im;
        switch(bad){case 0:wrong.w=0;break;case 1:wrong.h=0;break;case 2:wrong.d=2;break;case 3:wrong.layers=0;break;
            case 4:wrong.levels=33;break;case 5:wrong.samples=4;break;case 6:wrong.vtype=VK_IMAGE_VIEW_TYPE_3D;break;
            case 7:wrong.w=wrong.h=wrong.layers=UINT32_MAX;break;}
        assert(!compare_regions(&wrong,2,r));
    }
    assert(!compare_regions(&im,1,r));vkimg_t before=im;
    for(suballocate=0;suballocate<2;suballocate++)for(fail=1;fail<=8;fail++){
        types=0;assert(!compare_make(&im,VK_FORMAT_D16_UNORM,2));assert(!images&&!buffers&&!memories&&!memcmp(&im,&before,sizeof im));
    }
    suballocate=0;fail=0;types=0;assert(compare_make(&im,VK_FORMAT_D16_UNORM,2));assert(im.compare&&!im.compare->valid&&images==1&&buffers==1&&memories==2);
    VkImage original_image=im.img,depth_image=im.compare->depth.img;VkBuffer original_bridge=im.compare->buffer;
    compare_refresh(&im);assert(!strcmp(events,"ESDBIBCBT")&&restarts==1&&g_compare_copies==1&&g_compare_bytes==568&&g_vk_image_gen==1);
    unsigned recorded_before=recorded;compare_refresh(&im);assert(recorded==recorded_before&&g_compare_hits==1&&restarts==1);
    uint64_t generation=g_tex_gen;vk_tex_written(&im);assert(!im.compare->valid&&g_tex_gen==generation+1);vk_tex_written(&im);assert(g_tex_gen==generation+1);
    recorded=barriers=0;events[0]=0;compare_refresh(&im);assert(im.compare->valid&&g_compare_copies==2&&g_compare_bytes==1136&&g_vk_image_gen==2);
    assert(im.img==original_image&&im.compare->depth.img==depth_image&&im.compare->buffer==original_bridge);
    vk_tex_written(NULL);compare_drop(&im);assert(!im.compare&&!images&&!buffers&&!memories);
    // Constant comparison preserves shape and initializes all subresources
    // without copying source contents or allocating an intermediate buffer.
    for(suballocate=0;suballocate<2;suballocate++)for(fail=1;fail<=4;fail++){
        types=0;assert(!compare_make_constant(&im,VK_FORMAT_D16_UNORM,2));assert(!im.compare&&!images&&!buffers&&!memories);
    }
    suballocate=0;fail=0;types=0;recorded=barriers=0;events[0]=0;
    vkimg_t *constant=compare_depth(&im,VK_FORMAT_D16_UNORM,2,1);
    assert(constant==&im.compare->depth&&im.compare->constant_only&&images==1&&!buffers&&memories==1);
    assert(!strcmp(events,"EDBZB")&&g_const_compare_created==1&&g_const_compare_reads==1&&g_const_compare_clears==1);
    assert(g_compare_copies==2);recorded_before=recorded;
    assert(compare_depth(&im,VK_FORMAT_D16_UNORM,2,1)==constant&&recorded==recorded_before&&g_const_compare_clears==1);
    // A later content-dependent comparison replaces the surrogate before use.
    types=0;recorded=barriers=0;events[0]=0;
    assert(compare_depth(&im,VK_FORMAT_D16_UNORM,2,0)==&im.compare->depth);
    assert(!im.compare->constant_only&&im.compare->valid&&images==1&&buffers==1&&memories==2);
    assert(!strcmp(events,"ESDBIBCBT")&&g_compare_copies==3&&g_const_compare_reads==2);
    compare_drop(&im);assert(!im.compare&&!images&&!buffers&&!memories);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-compare-') as directory:
            path = Path(directory)
            c = path / 'comparison.c'
            exe = path / 'comparison.exe'
            c.write_text(code, encoding='utf8')
            built = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-missing-field-initializers', '-Wno-misleading-indentation',
                                    '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            checked = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == '__main__':
    unittest.main()
