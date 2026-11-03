// Real bank/deswizzle/BC decoder and production image functions; mock Vulkan
// records to verify byte order and preserve untouched texels on a partial write.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "nvn.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "astc_bank.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#define H(t,n) ((t)(uintptr_t)(n))
typedef struct { unsigned flags; uint8_t *mem; uint64_t size; } pool_t;
typedef struct { nvn_texdesc d; nvn_texlayout L; pool_t *pool; uint8_t *mem; int64_t off; int has_layout; uint64_t serial; } tex_t;
/* IMAGE TYPES */
static struct { int has_bc,has_astc,has_astc_decode; } V;
static int errors, copies, prepares, written, landed, restarts, dropped_views, dropped_images, fail_create, active=1, flushed=1;
static uint64_t g_vk_image_gen;
pthread_mutex_t g_present_mu=PTHREAD_MUTEX_INITIALIZER;
static vkimg_t logical;
static vkfmt_t lossless;
static uint8_t arena[65536], expected[65536];
static size_t used, checked;
static int decoding,legacy_order;
static const char *bank_path;
static const char *mock_getenv(const char *name) { return strcmp(name,"DM_VK_ASTC_BANK") ? NULL : bank_path; }
#define getenv mock_getenv
#define VKLOG(...) ((void)0)
#define VKRED_ONCE(...) ((void)++errors)
static int vk_sub_open(void) { return active; }
static unsigned layers_of(const nvn_texdesc *d) { return d->target==4 || d->target==9 ? d->depth : d->target==8 ? 6 : 1; }
static int nvn_flushed(const void *p,uint64_t n) { assert(p && n); return flushed; }
/* BANK FUNCTIONS */
static vkimg_t *img_new(const tex_t *t,const vkfmt_t *f) {
    (void)t; if(fail_create) return NULL;
    vkimg_t *im=calloc(1,sizeof *im); im->img=H(VkImage,100+g_vk_image_gen); im->mem=H(VkDeviceMemory,200);
    im->fmt=f->vk; im->layout=VK_IMAGE_LAYOUT_UNDEFINED; im->usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->cflags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; return im;
}
static void vk_end_pass_land(void) { landed++; }
static void vk_sub_restart(void) { assert(landed>restarts); restarts++; }
static void vk_lrz_forget(vkimg_t *im) { assert(im==&logical && restarts==landed); }
static void compare_drop(vkimg_t *im) { assert(im==&logical); }
static void vk_tex_view_drop_all(vkimg_t *im) { assert(im==&logical && restarts==landed); dropped_views++; }
static void vk_defer_free_view(VkImageView view) { assert(view); dropped_views++; }
static void vk_defer_free_image(VkImage image,VkDeviceMemory memory) { assert(image && memory && restarts==landed); dropped_images++; }
static void vk_defer_free_image_sub(VkImage image,void *sub) { assert(sub); vk_defer_free_image(image,H(VkDeviceMemory,2)); }
static VkCommandBuffer vk_upload_cmd(void) { return H(VkCommandBuffer,1); }
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) { assert(cb); if(im->layout!=VK_IMAGE_LAYOUT_GENERAL) prepares++; im->layout=VK_IMAGE_LAYOUT_GENERAL; }
static void vk_tex_written(vkimg_t *im) { assert(im==&logical); written++; }
static void *vk_scratch(size_t n,size_t align,VkBuffer *b,VkDeviceSize *o) {
    used=(used+align-1)&~(align-1); assert(used+n<=sizeof arena); *b=H(VkBuffer,2); *o=used;
    void *p=arena+used; used+=n; return p;
}
static void vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer b,VkImage im,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *r) {
    assert(cb && b && im==logical.img && layout==VK_IMAGE_LAYOUT_GENERAL && n==1);
    unsigned ly=(unsigned)copies/logical.levels,lv=(unsigned)copies%logical.levels;
    if(legacy_order) { lv=(unsigned)copies/logical.layers;ly=(unsigned)copies%logical.layers; }
    assert(r->imageSubresource.mipLevel==lv && r->imageSubresource.baseArrayLayer==ly && r->imageSubresource.layerCount==1);
    unsigned w=logical.w>>lv,h=logical.h>>lv; if(!w)w=1;if(!h)h=1;
    assert(r->imageExtent.width==w && r->imageExtent.height==h && r->imageExtent.depth==1);
    size_t bytes=decoding ? (size_t)w*h*lossless.dbpp : (size_t)((w+3)/4)*((h+3)/4)*16;
    assert(!r->bufferRowLength && !r->bufferImageHeight && !memcmp(arena+r->bufferOffset,expected+checked,bytes));
    checked+=bytes; copies++;
}
static void nvn_dirty_protect(const void *p,size_t n) { assert(p && n); }
static uint64_t nvn_dirty_now(void) { return 1; }
static const void *nvn_dirty_span(const void *p,size_t n) { assert(n); return p; }
static uint64_t linear_pitch(const tex_t *t,const vkfmt_t *f) { (void)t;(void)f; assert(0); return 0; }
static vkimg_t *vk_tex(const tex_t *t) { assert(t); return &logical; }
static const vkfmt_t *vk_fmt(int format) { assert(format==0x42); return &lossless; }
static void bc_decode(const vkfmt_t *,const uint8_t *,size_t,uint32_t,uint32_t,uint32_t,uint8_t *);
/* IMAGE FUNCTIONS */

static tex_t texture(pool_t *pool,int format,unsigned w,unsigned h,unsigned levels,unsigned layers) {
    tex_t t={0}; t.serial=7; t.d=(nvn_texdesc){.target=layers>1 ? 4 : 1,.format=format,.width=w,.height=h,.depth=layers,.levels=levels};
    nvn_tex_layout(&t.d,&t.L); pool->mem=calloc(1,t.L.size); pool->size=t.L.size; pool->flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
    t.pool=pool;t.mem=pool->mem;t.has_layout=1;
    for(unsigned ly=0;ly<layers;ly++)for(unsigned lv=0;lv<levels;lv++) {
        unsigned x=w>>lv,y=h>>lv;if(!x)x=1;if(!y)y=1;
        unsigned pitch=((x+3)/4)*(format==0x42 ? 8 : 16),bytes=pitch*((y+3)/4);
        uint8_t bc[4096];assert(bytes<=sizeof bc);memset(bc,format==0x42 ? 1 : 6,bytes);
        assert(!nvn_swizzle(&t.d,lv,ly,bc,pitch,t.mem));
    }
    return t;
}
static void reset_records(int decode) { copies=0;used=checked=0;decoding=decode;legacy_order=0; }
int main(int argc,char **argv) {
    assert(argc==2); bank_path=argv[1]; V.has_astc=V.has_astc_decode=1;
    pool_t pool; tex_t t=texture(&pool,0x42,7,9,3,2);
    lossless=(vkfmt_t){.vk=VK_FORMAT_BC1_RGBA_UNORM_BLOCK,.bpb=8,.bw=4,.bh=4}; bc_target(&lossless);
    uint8_t *bc,*data; const dm_astc_entry *e;
    V.has_bc=1;assert(!astc_fetch(&t,&lossless,&bc,&data));V.has_bc=0;
    V.has_astc_decode=0;assert(!astc_fetch(&t,&lossless,&bc,&data));V.has_astc_decode=1;
    active=0;assert(!astc_fetch(&t,&lossless,&bc,&data));active=1;
    for(int bad=0;bad<8;bad++) {
        tex_t other=t; pool_t p=pool;other.pool=&p;
        switch(bad) {case 0:other.d.target=2;break;case 1:other.d.samples=4;break;case 2:other.d.flags=NVN_TEXTURE_FLAGS_IMAGE;break;
        case 3:p.flags|=NVN_MEMORY_POOL_FLAGS_VIRTUAL;break;case 4:other.off=-1;break;case 5:other.mem++;break;
        case 6:other.has_layout=0;p.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED;flushed=0;break;case 7:other.d.format=0x4c;break;}
        assert(!astc_fetch(&other,&lossless,&bc,&data));flushed=1;
    }
    e=astc_fetch(&t,&lossless,&bc,&data);assert(e && e->source_bytes==128 && e->bytes==256);
    assert(!memcmp(bc,(uint8_t[128]){[0 ... 127]=1},128));memcpy(expected,data,e->bytes);
    logical=(vkimg_t){.w=7,.h=9,.d=1,.levels=3,.layers=2,.img=H(VkImage,1),.mem=H(VkDeviceMemory,2),
        .fmt=VK_FORMAT_ASTC_4x4_UNORM_BLOCK,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.astc=e,.astc_bc=bc,.astc_pending=data,.sidecar_tracked=1};
    g_astc_shadow=e->source_bytes; reset_records(0); astc_upload(&logical);
    assert(copies==6 && checked==e->bytes && !logical.astc_pending && written==1);
    // A known CPU re-upload stays compressed and uses every mip/layer again.
    reset_records(0);assert(pkg_upload_(&logical,&t,&lossless,1));assert(copies==6 && logical.astc==e && !logical.astc_pending);
    // Compare demotion to original BC, after the guest bytes have been changed.
    size_t offset=0,out=0;
    for(unsigned ly=0;ly<2;ly++)for(unsigned lv=0;lv<3;lv++) {
        unsigned w=7>>lv,h=9>>lv;if(!w)w=1;if(!h)h=1;size_t pitch=((w+3)/4)*8;
        bc_decode(&lossless,logical.astc_bc+offset,pitch,w,h,1,expected+out);offset+=pitch*((h+3)/4);out+=(size_t)w*h*4;
    }
    memset(t.mem,0,t.L.size);logical.full=H(VkImageView,3);logical.pkg_used=123; logical.pool=77;logical.last_use=99;
    fail_create=1;assert(!vk_tex_lossless(&t) && logical.astc==e && !dropped_images); fail_create=0;
    reset_records(1);assert(vk_tex_lossless(&t)==&logical);
    assert(copies==6 && checked==out && !logical.astc && !logical.astc_bc && !g_astc_shadow);
    assert(logical.fmt==VK_FORMAT_R8G8B8A8_UNORM && logical.pool==77 && logical.last_use==99 && logical.pkg_used==123);
    assert(g_vk_image_gen==1 && dropped_images==1 && dropped_views==2 && restarts==1 && landed==1 && g_astc_demotions==1);
    assert(vk_tex_lossless(&t)==&logical && dropped_images==1);free(pool.mem);
    // A complete CPU re-upload of unknown content must replace ASTC storage,
    // upload the new BC decode, and discard the stale compressed snapshot.
    t=texture(&pool,0x42,7,9,3,2);e=astc_fetch(&t,&lossless,&bc,&data);assert(e);
    logical=(vkimg_t){.w=7,.h=9,.d=1,.levels=3,.layers=2,.img=H(VkImage,1),.mem=H(VkDeviceMemory,2),
        .fmt=VK_FORMAT_ASTC_4x4_UNORM_BLOCK,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.astc=e,.astc_bc=bc,.sidecar_tracked=1};
    free(data);g_astc_shadow=e->source_bytes;memset(t.mem,0,t.L.size);out=0;
    for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<2;ly++) {
        unsigned w=7>>lv,h=9>>lv;if(!w)w=1;if(!h)h=1;
        for(unsigned i=0;i<w*h;i++) { memset(expected+out,0,3);expected[out+3]=255;out+=4; }
    }
    reset_records(1);legacy_order=1;assert(pkg_upload_(&logical,&t,&lossless,1));
    assert(copies==6 && checked==out && !logical.astc && !logical.astc_bc && !g_astc_shadow && g_astc_misses==1);
    assert(g_vk_image_gen==2 && dropped_images==2 && g_astc_demotions==2);free(pool.mem);
    // Whole-image quality exceptions retain the original format.
    t=texture(&pool,0x48,16,16,1,1);vkfmt_t bc3=lossless;bc3.bpb=16;
    assert(!astc_fetch(&t,&bc3,&bc,&data) && !bc && !data && g_astc_exceptions==1);free(pool.mem);
    vkfmt_t a=astc_format(&lossless,0x41);assert(a.vk==VK_FORMAT_ASTC_4x4_UNORM_BLOCK && a.swz.a==VK_COMPONENT_SWIZZLE_ONE && a.bpb==16 && !a.dec);
    a=astc_format(&lossless,0x46);assert(a.vk==VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    a=astc_format(&lossless,0x4b);assert(a.swz.r==VK_COMPONENT_SWIZZLE_R && a.swz.g==VK_COMPONENT_SWIZZLE_A && a.swz.b==VK_COMPONENT_SWIZZLE_ZERO && a.swz.a==VK_COMPONENT_SWIZZLE_ONE);
    assert(!errors);dm_astc_close(g_astc_bank);return 0;
}
