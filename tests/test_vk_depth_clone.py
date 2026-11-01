"""Compile actual depth-clone helpers with resource failures and clear transitions."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


def function(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}', start) + 3]


class DepthClone(unittest.TestCase):
    def test_copy_contract_failures_and_original_depth_writes(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        start = header.index('struct vkimg {')
        image_type = 'typedef struct vkimg vkimg_t;\n' + header[start:header.index('\n};', start) + 4]
        obj = (ROOT / 'nxnative/dm/gfx/nvn/nvn_obj.h').read_text(encoding='utf-8')
        clear_type = next(line for line in obj.splitlines() if line.endswith(' p_clear_ds;'))
        whole = next(line for line in src.splitlines() if line.startswith('static int pass_whole('))
        prefix = r'''
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define HANDLE(t,n) ((t)(uintptr_t)(n))
''' + image_type + clear_type + r'''
typedef struct { int depth_repl; } vkshader_t;
static struct { int pass_open, rast_discard; uint32_t daspect, pass_w, pass_h;
    struct { int dt, dw; } ds; struct { unsigned offset_en; } poly; vkshader_t *sh[2]; } S;
static struct { vkimg_t *im; uint32_t fmt,w,h; } P_att[9];
static VkRenderingAttachmentInfo P_da, P_sa;
static vkimg_t original, P_depth_copy, *g_lrz_prev;
static int P_depth_copied, P_depth_clear, P_depth_tried, P_cleared, g_depth_clone, g_fdz, g_lrz_touch;
static uint64_t g_tex_frame, g_depth_clone_restarts;
static struct { unsigned folded; } g_vkst;
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static VkRect2D area;
static unsigned ended, clears, touches, forgotten;
static int fail, suballocate, created, destroyed, deferred_views, deferred_images, copied, barriers, transitions;
static char events[16]; static unsigned nevent;
static void event(char c) { assert(nevent < sizeof events-1); events[nevent++]=c; events[nevent]=0; }
static void vk_end_pass(void) { assert(S.pass_open); ended++; S.pass_open=0; P_depth_copied=0; }
static void lrz_touch(vkimg_t *im, int write) { assert(im==&P_depth_copy && write==1); touches++; }
static void vk_lrz_forget(vkimg_t *im) { assert(im==&P_depth_copy); forgotten++; if(g_lrz_prev==im) g_lrz_prev=NULL; }
static void vk_tex_written(vkimg_t *im) { assert(im==&original); }
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd,VkFormat fmt,VkFormatProperties *p) {
    assert(pd==V.pd && fmt==original.fmt); memset(p,0,sizeof *p);
    if(fail!=6) p->optimalTilingFeatures=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}
static VkResult vkCreateImage(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out) {
    assert(d==V.dev && !a && ci->flags==0 && ci->imageType==VK_IMAGE_TYPE_2D && ci->format==original.fmt);
    assert(ci->extent.width==original.w && ci->extent.height==original.h && ci->extent.depth==1);
    assert(ci->mipLevels==1 && ci->arrayLayers==1 && ci->samples==1 && ci->tiling==VK_IMAGE_TILING_OPTIMAL);
    assert(ci->usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    if(fail==1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=HANDLE(VkImage,2); created++; return VK_SUCCESS;
}
static void vkGetImageMemoryRequirements(VkDevice d,VkImage i,VkMemoryRequirements *r) {
    assert(d==V.dev && i==HANDLE(VkImage,2)); *r=(VkMemoryRequirements){8192,4096,1};
}
static uint32_t vk_mem_type(uint32_t bits,VkMemoryPropertyFlags want,VkMemoryPropertyFlags avoid) {
    assert(bits==1 && want==VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && !avoid); return fail==2 ? UINT32_MAX : 0;
}
static VkDeviceMemory vk_image_memory(const VkMemoryRequirements *r,uint32_t type,VkDeviceSize *off,void **sub) {
    assert(r->size==8192 && type==0); *off=0; *sub=NULL;
    if(fail==3) return VK_NULL_HANDLE;
    if(suballocate) { *sub=(void *)(uintptr_t)4; *off=4096; } return HANDLE(VkDeviceMemory,3);
}
static void vkDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks *a) {
    assert(d==V.dev && i==HANDLE(VkImage,2) && !a); destroyed++; event('D');
}
static VkResult vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize off) {
    assert(d==V.dev && i==HANDLE(VkImage,2) && m==HANDLE(VkDeviceMemory,3) && off==(suballocate?4096u:0u));
    return fail==4 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VkResult vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *vi,const VkAllocationCallbacks *a,VkImageView *v) {
    assert(d==V.dev && !a && vi->image==HANDLE(VkImage,2) && vi->format==original.fmt && vi->viewType==VK_IMAGE_VIEW_TYPE_2D);
    assert(vi->subresourceRange.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT && vi->subresourceRange.levelCount==1 && vi->subresourceRange.layerCount==1);
    if(fail==5) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *v=HANDLE(VkImageView,5); return VK_SUCCESS;
}
static void vk_defer_free_view(VkImageView v) { assert(v==HANDLE(VkImageView,5)); deferred_views++; event('V'); }
static void vk_defer_free_image_sub(VkImage i,void *sub) { assert(i==HANDLE(VkImage,2) && sub==(void *)(uintptr_t)4); deferred_images++; event('S'); }
static void vk_defer_free_image(VkImage i,VkDeviceMemory m) { assert(i==HANDLE(VkImage,2) && m==HANDLE(VkDeviceMemory,3)); deferred_images++; event('I'); }
static VkCommandBuffer vk_cmd(void) { return HANDLE(VkCommandBuffer,1); }
static void vk_img_barrier_all(VkCommandBuffer cb) { assert(cb==vk_cmd()); barriers++; }
static void vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags dep,
    uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *i) {
    assert(cb==vk_cmd() && s==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && d==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && !dep && !nm && !m && !nb && !b && ni==1);
    assert(i->image==HANDLE(VkImage,2) && i->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && i->newLayout==VK_IMAGE_LAYOUT_GENERAL);
    assert(i->subresourceRange.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT && i->subresourceRange.levelCount==1 && i->subresourceRange.layerCount==1); transitions++;
}
static void vkCmdCopyImage(VkCommandBuffer cb,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy *r) {
    assert(cb==vk_cmd() && s==original.img && sl==VK_IMAGE_LAYOUT_GENERAL && d==HANDLE(VkImage,2) && dl==VK_IMAGE_LAYOUT_GENERAL && n==1);
    assert(r->srcSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT && r->dstSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
    assert(!r->srcSubresource.mipLevel && !r->dstSubresource.mipLevel && !r->srcSubresource.baseArrayLayer && !r->dstSubresource.baseArrayLayer);
    assert(r->srcSubresource.layerCount==1 && r->dstSubresource.layerCount==1 && r->extent.width==original.w && r->extent.height==original.h && r->extent.depth==1);
    assert(barriers==1 && transitions==1 && deferred_views==1 && deferred_images==1); copied++;
}
'''
        code = prefix + r'''
static const char *mode_value;
static char *mode_environment(const char *name) { assert(!strcmp(name,"DM_VK_DEPTH_CLONE")); return (char *)mode_value; }
#define getenv mode_environment
''' + function(src, 'static int depth_clone_mode(') + '\n#undef getenv\n'
        for signature in ('static int depth_clone_wanted(', 'static int depth_clone_make(',
                          'static void depth_clone_before_draw(', 'static void depth_clone_finish('):
            code += function(src, signature)
        code += r'''
static void reset(void) {
    memset(&S,0,sizeof S); memset(&original,0,sizeof original); memset(P_att,0,sizeof P_att); memset(&P_da,0,sizeof P_da);
    original.img=HANDLE(VkImage,9); original.w=64; original.h=32; original.d=original.layers=original.levels=original.samples=1;
    original.fmt=VK_FORMAT_D16_UNORM; original.vtype=VK_IMAGE_VIEW_TYPE_2D; original.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    original.layout=VK_IMAGE_LAYOUT_GENERAL; original.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; original.fdz_frame=101;
    P_att[8].im=&original; P_att[8].w=64; P_att[8].h=32; P_att[8].fmt=original.fmt;
    static vkshader_t shader; shader.depth_repl=0; S.sh[1]=&shader; S.ds.dt=1; S.daspect=VK_IMAGE_ASPECT_DEPTH_BIT; S.pass_w=64; S.pass_h=32;
    P_da.imageView=HANDLE(VkImageView,8); P_da.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; P_da.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    g_depth_clone=g_fdz=g_lrz_touch=1; g_tex_frame=100; P_depth_copied=P_depth_clear=P_depth_tried=P_cleared=0;
    V.dev=HANDLE(VkDevice,1); V.pd=HANDLE(VkPhysicalDevice,6); created=destroyed=deferred_views=deferred_images=copied=barriers=transitions=0; nevent=0; events[0]=0;
}
static int begin_pass(void) { if(S.pass_open) return 1; P_depth_copied=P_depth_clear=0; P_da.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; P_da.imageView=HANDLE(VkImageView,8); S.pass_open=2; return 1; }
static int pass_commit(void) { if(S.pass_open==2) { assert(!depth_clone_wanted()); S.pass_open=1; } return S.pass_open==1; }
static VkRect2D scissor_rect(void) { return area; }
static void clear_attachments(VkCommandBuffer cb,uint32_t n,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r) {
    assert(cb==vk_cmd() && n==1 && nr==1 && r->rect.extent.width && r->rect.extent.height && (a->aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT));
    assert(!P_depth_copied && P_depth_clear && P_da.imageView==HANDLE(VkImageView,8)); clears++;
}
#define vkCmdClearAttachments clear_attachments
''' + whole + '\n' + function(src, 'static void clear_ds(') + r'''
int main(void) {
    const char *options[]={NULL,"0","1","2","-1","99","junk"};
    const int modes[]={2,0,1,2,0,2,0};
    for(unsigned i=0;i<sizeof modes/sizeof *modes;i++) { mode_value=options[i]; assert(depth_clone_mode()==modes[i]); }
    for(unsigned bits=0;bits<1024;bits++) {
        reset(); g_depth_clone=(bits>>0)&1; g_fdz=(bits>>1)&1; S.ds.dt=(bits>>2)&1; S.ds.dw=(bits>>3)&1;
        S.rast_discard=(bits>>4)&1; S.sh[1]->depth_repl=(bits>>5)&1; P_depth_clear=(bits>>6)&1;
        S.poly.offset_en=(bits>>7)&1; P_da.loadOp=(bits>>8)&1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        P_da.resolveMode=(bits>>9)&1 ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : 0;
        int wanted=(bits&7)==7 && !(bits&1016); assert(depth_clone_wanted()==wanted);
    }
    for(int field=0;field<14;field++) {
        reset(); assert(depth_clone_wanted());
        switch(field) {
        case 0: original.samples=2; break; case 1: original.layers=2; break; case 2: original.levels=2; break;
        case 3: original.d=2; break; case 4: original.vtype=VK_IMAGE_VIEW_TYPE_3D; break;
        case 5: original.aspect|=VK_IMAGE_ASPECT_STENCIL_BIT; break; case 6: S.daspect|=VK_IMAGE_ASPECT_STENCIL_BIT; break;
        case 7: P_att[8].w--; break; case 8: P_att[8].fmt=VK_FORMAT_D32_SFLOAT; break;
        case 9: original.layout=VK_IMAGE_LAYOUT_UNDEFINED; break; case 10: original.usage=0; break; case 11: S.sh[1]=NULL; break;
        case 12: original.cflags=VK_IMAGE_CREATE_PROTECTED_BIT; break; case 13: original.w=P_att[8].w=0; break;
        } assert(!depth_clone_wanted());
    }
    const uint64_t frames[]={0,98,99,100,101,102};
    for(unsigned i=0;i<sizeof frames/sizeof *frames;i++) { reset(); original.fdz_frame=frames[i]; assert(depth_clone_wanted()==(frames[i]>=99 && frames[i]<=101)); }
    reset(); P_att[8].im=NULL; assert(!depth_clone_wanted());
    reset(); g_depth_clone=2; assert(depth_clone_wanted());
    original.fmt=P_att[8].fmt=VK_FORMAT_D32_SFLOAT; assert(!depth_clone_wanted());
    g_depth_clone=1; assert(depth_clone_wanted());
    for(suballocate=0;suballocate<=1;suballocate++) for(fail=0;fail<=6;fail++) {
        reset(); vkimg_t output, untouched, source; memset(&output,0xa5,sizeof output); memcpy(&untouched,&output,sizeof output); memcpy(&source,&original,sizeof source);
        assert(depth_clone_make(&original,&output)==(fail==0)); assert(!memcmp(&original,&source,sizeof source));
        if(fail) { assert(!memcmp(&output,&untouched,sizeof output)); assert(!copied && !barriers && !transitions && !deferred_views); }
        if(fail==0) {
            assert(copied==1 && barriers==2 && transitions==1 && deferred_views==1 && deferred_images==1 && !destroyed);
            assert(!strcmp(events,suballocate?"VS":"VI")); assert(output.img==HANDLE(VkImage,2) && output.full==HANDLE(VkImageView,5));
            assert(output.fmt==original.fmt && output.w==64 && output.h==32 && output.aspect==VK_IMAGE_ASPECT_DEPTH_BIT && !output.cflags);
        } else if(fail==6) assert(!created && !destroyed && !deferred_images);
        else if(fail<=3) assert(destroyed==(fail!=1) && !deferred_images);
        else assert(!destroyed && deferred_images==1 && !strcmp(events,suballocate?"S":"I"));
    }
    reset(); S.pass_open=1; P_depth_copied=1; unsigned e=ended; depth_clone_before_draw(); assert(ended==e);
    S.ds.dw=1; depth_clone_before_draw(); assert(ended==e+1 && !P_depth_copied);
    reset(); S.pass_open=1; e=ended; depth_clone_before_draw(); assert(ended==e+1); // mid-pass first copy
    reset(); S.pass_open=1; P_depth_tried=1; e=ended; depth_clone_before_draw(); assert(ended==e); // failed resource attempt keeps the original pass
    for(int active=1;active<=2;active++) for(int touch=0;touch<=1;touch++) {
        reset(); P_depth_copied=1; S.pass_open=active; g_lrz_touch=touch; g_lrz_prev=&P_depth_copy;
        unsigned t=touches,f=forgotten; depth_clone_finish(); assert(!P_depth_copied && !g_lrz_prev && forgotten==f+1 && touches==t+(unsigned)(active==1 && touch));
        depth_clone_finish(); assert(forgotten==f+1);
    }
    for(int full=0;full<=1;full++) for(int existing=0;existing<=1;existing++) {
        reset(); S.pass_open=existing; P_depth_copied=existing; unsigned e=ended,c=clears;
        area=(VkRect2D){{0,0},{64,32}}; if(!full) area=(VkRect2D){{4,4},{8,8}};
        p_clear_ds clear={.depth=.25f,.depth_mask=1}; clear_ds(&clear);
        assert(ended==e+(unsigned)existing && !P_depth_copied && P_depth_clear);
        if(full) { assert(P_da.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && clears==c); pass_commit(); }
        else assert(clears==c+1);
    }
    assert(g_depth_clone_restarts && g_vkst.folded); return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-depth-clone-') as temp:
            c = Path(temp) / 'clone.c'; exe = Path(temp) / 'clone-check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-idirafter', str(headers), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
