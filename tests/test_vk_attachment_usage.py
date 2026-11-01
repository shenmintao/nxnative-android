"""Imageless attachment creation must match the view's inherited usage."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT=Path(__file__).resolve().parents[1]
DM=ROOT/'nxnative/dm'


class AttachmentUsage(unittest.TestCase):
    def test_real_framebuffer_key_preserves_color_depth_and_clone_view_contracts(self):
        cc=shutil.which('cc') or shutil.which('clang')
        ndk,_=find_ndk()
        headers=next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'),None) if ndk else None
        if not cc or not headers:self.skipTest('C compiler and Vulkan headers required')
        source=(DM/'android/gfx_vk_exec.c').read_text(encoding='utf8')
        key=next(line for line in source.splitlines() if line.startswith('typedef struct { VkRenderPass rp;'))
        start=source.index('static VkFramebuffer fb_get(')
        helper=source[start:source.index('\n}',start)+3]
        start=source.index('        rpkey k; memset(&k, 0, sizeof k);')
        fragment=source[start:source.index('        VkRenderPass rp = rp_get(&k);',start)]
        prefix=r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "nvn_hmap.h"
#define VKRED_ONCE(...) ((void)0)
static struct {VkDevice dev;} V;
typedef struct {VkImageUsageFlags usage;VkImageCreateFlags cflags;VkFormat fmt,fmt2;} vkimg_t;
static struct {vkimg_t*im;VkImageView v;uint32_t fmt,layers,w,h;VkImageUsageFlags usage;} P_att[9];
static VkRenderingAttachmentInfo P_ca[8],P_da,P_sa;
static VkRenderingInfo P_ri;
static vkimg_t P_depth_copy;static int P_depth_copied;
static struct {uint32_t samples;} S;
typedef struct {uint32_t ncol,samples,cfmt[8],cload[8],dfmt,dload,sload,dstore;} rpkey;
static pthread_mutex_t g_rp_mu=PTHREAD_MUTEX_INITIALIZER;
static VkImageUsageFlags expected[9];static unsigned created;
static VkResult make_framebuffer(VkDevice d,const VkFramebufferCreateInfo*ci,const VkAllocationCallbacks*a,VkFramebuffer*out) {
    (void)d;(void)a;assert(ci->flags==VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT&&ci->pAttachments==NULL);
    const VkFramebufferAttachmentsCreateInfo*attachments=ci->pNext;
    for(unsigned i=0;i<ci->attachmentCount;i++)
        if(attachments->pAttachmentImageInfos[i].usage!=expected[i])return VK_ERROR_VALIDATION_FAILED_EXT;
    *out=(VkFramebuffer)(uintptr_t)++created;return VK_SUCCESS;
}
#define vkCreateFramebuffer make_framebuffer
'''+key+r'''
static nvn_hmap g_fbs={.klen=sizeof(fbkey)};
'''+helper+r'''
static VkFramebuffer construct(void) {
'''+fragment+r'''
    (void)cv;(void)views;fk.rp=(VkRenderPass)(uintptr_t)1;fk.w=fk.h=128;fk.layers=1;fk.n=na;
    return fb_get(&fk);
}
int main(void) {
    vkimg_t images[9];memset(images,0,sizeof images);S.samples=1;
    for(unsigned colors=1;colors<=8;colors++)for(int depth=0;depth<3;depth++){
        memset(P_att,0,sizeof P_att);memset(P_ca,0,sizeof P_ca);memset(&P_da,0,sizeof P_da);memset(&P_sa,0,sizeof P_sa);
        P_ri.colorAttachmentCount=colors;P_ri.pDepthAttachment=depth?&P_da:NULL;P_ri.pStencilAttachment=NULL;
        P_depth_copied=depth==2;
        for(unsigned i=0;i<colors;i++){
            images[i].usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            images[i].fmt=VK_FORMAT_R8G8B8A8_UNORM;
            P_att[i].im=&images[i];P_att[i].usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            P_att[i].fmt=images[i].fmt;P_att[i].layers=1;P_att[i].w=P_att[i].h=128;
            P_ca[i].imageView=(VkImageView)(uintptr_t)(i+1);expected[i]=P_att[i].usage;
        }
        if(depth){
            images[8].usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            P_att[8].im=&images[8];P_att[8].usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            P_att[8].fmt=VK_FORMAT_D32_SFLOAT;P_att[8].layers=1;P_att[8].w=P_att[8].h=128;
            P_da.imageView=(VkImageView)(uintptr_t)20;
            P_depth_copy.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            expected[colors]=depth==2?P_depth_copy.usage:P_att[8].usage;
        }
        VkFramebuffer first=construct();if(!first)return 77;
        assert(construct()==first);
        // Parent storage can support additional roles without changing its view.
        for(unsigned i=0;i<colors;i++)images[i].usage|=VK_IMAGE_USAGE_STORAGE_BIT;
        assert(construct()==first);
    }
    for(unsigned i=0;i<g_fbs.cap;i++)if(g_fbs.vals[i])g_fbs.vals[i]=NULL;
    free(g_fbs.keys);free(g_fbs.vals);puts("PASS");return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)
            def run(text,label):
                c=path/(label+'.c');exe=path/(label+'.exe');c.write_text(text,encoding='utf8')
                subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',
                    '-idirafter',str(headers),'-I'+str(DM/'gfx/nvn'),str(c),'-o',str(exe)],check=True)
                return subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run(prefix,'current').returncode,0)
            old=prefix.replace('fk.a[na].usage = P_att[i].usage;', 'fk.a[na].usage = P_att[i].im->usage;')
            self.assertNotEqual(old,prefix)
            self.assertEqual(run(old,'old-parent-usage').returncode,77)
