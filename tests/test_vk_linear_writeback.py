"""Linear image writes must reach the CPU backing rows without copying padding."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class LinearWriteback(unittest.TestCase):
    def test_real_writeback_records_visible_texels_and_preserves_cpu_padding(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        source = (DM / 'android/gfx_vk_tex.c').read_text(encoding='utf8')
        def extract(signature):
            start = source.index(signature)
            return source[start:source.index('\n}', start)+3]
        functions = extract('static uint64_t linear_pitch(') + extract('void vk_linear_writeback(')
        prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pool_write_ranges.h"
#define NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET 0x40
#define VKLOG(...) ((void)0)
static unsigned errors;
#define VKRED_ONCE(...) (++errors)
typedef struct {struct {uint32_t width,stride,flags;}d;} tex_t;
typedef struct {unsigned bpb,bw;} vkfmt_t;
typedef struct {uint64_t linear_addr,linear_pitch;uint32_t linear_bpb,w,h;int nvn_format;VkImage img;} vkimg_t;
static uint8_t cpu[65536],gpu[65536];static uint64_t address=0x100000,limit;
static VkDeviceSize returned_offset;
static vk_pool_writes writes;
static unsigned copies,barriers,phase;
static const char *writeback_mode;
static const char *writeback_getenv(const char *name){return !strcmp(name,"DM_VK_LINEAR_WRITEBACK")?writeback_mode:NULL;}
#define getenv writeback_getenv
static int vk_buf_at(uint64_t a,uint64_t n,VkBuffer*b,VkDeviceSize*o){
    if(a!=address||n>limit)return 0;*b=(VkBuffer)(uintptr_t)1;*o=returned_offset;return 1;
}
static void vk_img_barrier_all(VkCommandBuffer cb){(void)cb;assert(!phase);phase=1;}
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage im,VkImageLayout layout,VkBuffer b,uint32_t n,const VkBufferImageCopy*c){
    (void)cb;(void)im;(void)b;assert(phase==1&&layout==VK_IMAGE_LAYOUT_GENERAL&&n==1);phase=2;copies++;
    assert(c->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&c->imageSubresource.layerCount==1);
    assert(!c->imageSubresource.mipLevel&&!c->imageOffset.x&&!c->imageOffset.y&&!c->imageOffset.z&&c->imageExtent.depth==1);
    unsigned bpb=2,pitch=c->bufferRowLength*bpb;
    for(unsigned y=0;y<c->imageExtent.height;y++)for(unsigned x=0;x<c->imageExtent.width*bpb;x++)gpu[y*pitch+x]=(uint8_t)(x+7*y);
}
static void vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags d,
    uint32_t n,const VkMemoryBarrier*b,uint32_t nb,const VkBufferMemoryBarrier*bb,uint32_t ni,const VkImageMemoryBarrier*ib){
    (void)cb;(void)d;(void)bb;(void)ib;assert(phase==2&&n==1&&!nb&&!ni);phase=0;barriers++;
    assert(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&(dst&VK_PIPELINE_STAGE_HOST_BIT)&&(dst&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
    assert(b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&(b->dstAccessMask&VK_ACCESS_HOST_READ_BIT));
}
void vk_gpu_wrote(uint64_t a,uint64_t n){assert(vk_pool_writes_add(&writes,a,n));}
'''
        suffix = r'''
static void run(unsigned width,unsigned height,unsigned pitch){
    memset(cpu,0xa5,sizeof cpu);memset(gpu,0x3c,sizeof gpu);writes.count=0;
    vkimg_t im={address,pitch,2,width,height,6,(VkImage)(uintptr_t)1};
    limit=(uint64_t)pitch*(height-1)+2*width;returned_offset=0;
    unsigned before=copies;vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&im);assert(copies==before+1&&barriers==copies);
    // Fence completion: apply only the real recorded write spans to CPU memory.
    vk_pool_writes_copy_back(cpu,gpu,address,address+sizeof cpu,&writes);
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<pitch;x++)
        assert(cpu[y*pitch+x]==(x<width*2?(uint8_t)(x+7*y):0xa5));
    for(unsigned i=pitch*height;i<sizeof cpu;i++)assert(cpu[i]==0xa5);
    assert(writes.count==(pitch==2*width?1:height));
    // A backing range one byte too short must never record an out-of-bounds copy.
    limit--;before=copies;vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&im);assert(copies==before);limit++;
    returned_offset=2;vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&im);assert(copies==before);returned_offset=0;
}
int main(void){
    tex_t t={{33,0,0x40}};vkfmt_t f={2,1};assert(linear_pitch(&t,&f)==128);
    t.d.flags=0x10;assert(linear_pitch(&t,&f)==96);t.d.stride=256;assert(linear_pitch(&t,&f)==256);
    t.d.width=256;t.d.stride=0;t.d.flags=0x40;assert(linear_pitch(&t,&f)==512);
    t.d.width=33;vkfmt_t bc={16,4};assert(linear_pitch(&t,&bc)==256);
    t.d.flags=0x10;assert(linear_pitch(&t,&bc)==160);
    t.d.width=0;assert(linear_pitch(&t,&f)==32);
    run(32,1,64);run(33,19,128);run(256,32,512);
    vkimg_t invalid={address,65,2,32,4,6,(VkImage)(uintptr_t)1};unsigned before=copies;
    vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&invalid);assert(copies==before);
    invalid.linear_pitch=UINT64_MAX-1;vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&invalid);assert(copies==before);
    invalid.linear_addr=0;vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&invalid);vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,NULL);assert(copies==before);
    invalid=(vkimg_t){address,64,2,32,1,6,(VkImage)(uintptr_t)1};
    writeback_mode="0";vk_linear_writeback((VkCommandBuffer)(uintptr_t)1,&invalid);assert(copies==before);writeback_mode=NULL;
    assert(errors==8);free(writes.spans);puts("PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            c = path/'linear.c'
            exe = path/'linear.exe'
            c.write_text(prefix+functions+suffix, encoding='utf8')
            subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                            '-idirafter',str(headers),'-I'+str(DM/'android'),str(c),'-o',str(exe)], check=True)
            result = subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
