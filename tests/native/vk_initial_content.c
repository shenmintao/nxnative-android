// CPU execution of production upload/policy functions. Vulkan records are mocked;
// the NVN storage layout and swizzle/deswizzle operations are the real helpers.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "nvn.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; } pool_t;
typedef struct { uint64_t serial; const uint8_t *mem; pool_t *pool; int64_t off; nvn_texdesc d; nvn_texlayout L; int has_layout; } tex_t;
typedef struct { uint32_t w,h,d,layers,levels; VkImage img; VkImageAspectFlags aspect; VkImageLayout layout;
    int gpu_w,packaged,pkg_released; uint64_t pkg_stamp,pkg_seen; const void *pkg_span; const uint8_t *pkg_mem; size_t pkg_len;
    const struct dm_astc_entry *astc; uint8_t *astc_bc, *astc_pending; } vkimg_t;
typedef struct dm_astc_entry { uint64_t source_bytes; } dm_astc_entry;
typedef struct { int vk; uint32_t bw,bh,bpb; int dec,dbpp; } vkfmt_t;
static void bc_decode(const vkfmt_t *f,const uint8_t *p,size_t pitch,uint32_t w,uint32_t h,uint32_t z,uint8_t *dst) { (void)f;(void)p;(void)pitch;(void)w;(void)h;(void)z;(void)dst; assert(0 && "BC decoding has its own fixture"); }
static uint8_t storage[2<<20], arena[2<<20], expected[8][8][65536];
static size_t used, expected_bytes[8][8];
static int copies, prepares, protected_ranges, span_reads, flush_calls, was_flushed, errors, write_notices;
static const char *flush_option;
static uint64_t g_pkg_n,g_pkg_bytes,g_pkg_ns;
static uint64_t g_astc_shadow,g_astc_demotions;
static const dm_astc_entry *astc_fetch(const tex_t *t,const vkfmt_t *f,uint8_t **source,uint8_t **data) { (void)t;(void)f;(void)source;(void)data; assert(0); return NULL; }
static int image_replace(vkimg_t *im,const tex_t *t,const vkfmt_t *f) { (void)im;(void)t;(void)f; assert(0); return 0; }
static void astc_upload(vkimg_t *im) { (void)im; assert(0); }
#define VKLOG(...) ((void)0)
#define VKRED_ONCE(...) ((void)++errors)
static const char *mock_getenv(const char *name) { return strcmp(name,"DM_NVN_FLUSH_FILL") ? NULL : flush_option; }
#define getenv mock_getenv
static int mock_clock(int unused,struct timespec *out) { (void)unused; out->tv_sec=1; out->tv_nsec=0; return 0; }
#define clock_gettime mock_clock
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static VkCommandBuffer vk_upload_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) { assert(cb); if(im->layout!=VK_IMAGE_LAYOUT_GENERAL) { prepares++; im->layout=VK_IMAGE_LAYOUT_GENERAL; } }
static void vk_tex_written(vkimg_t *im) { assert(im && copies > write_notices); write_notices++; }
static void nvn_dirty_protect(const void *p,size_t n) { assert(p && n); protected_ranges++; }
static uint64_t nvn_dirty_now(void) { return 123; }
static const void *nvn_dirty_span(const void *p,size_t n) { assert(p && n); span_reads++; return p; }
int nvn_flushed(const void *p,uint64_t n) { assert(p && n); flush_calls++; return was_flushed; }
static void *vk_scratch(size_t n,size_t align,VkBuffer *buffer,VkDeviceSize *offset) {
    used=(used+align-1)&~(align-1); assert(n<=sizeof arena-used); *buffer=(VkBuffer)(uintptr_t)2; *offset=used;
    void *p=arena+used; used+=n; return p;
}
static void vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy *region) {
    assert(cb && buffer && image && layout==VK_IMAGE_LAYOUT_GENERAL && prepares==1 && count==1);
    unsigned lv=region->imageSubresource.mipLevel,ly=region->imageSubresource.baseArrayLayer;
    assert(lv<8 && ly<8 && region->imageSubresource.layerCount==1 && region->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);
    assert(region->bufferRowLength==0 && region->bufferImageHeight==0 && region->imageExtent.depth>0);
    assert(expected_bytes[lv][ly] && !memcmp(arena+region->bufferOffset,expected[lv][ly],expected_bytes[lv][ly])); copies++;
}

/* PRODUCTION_FUNCTIONS */

static tex_t make_texture(pool_t *pool,vkimg_t *im,vkfmt_t *fmt,int target,int compressed,int zeros) {
    copies=prepares=protected_ranges=span_reads=flush_calls=errors=write_notices=0; used=0; flush_option=NULL;
    g_pkg_n=g_pkg_bytes=g_pkg_ns=0; memset(arena,0xcc,sizeof arena); memset(storage,0xa7,sizeof storage);
    memset(expected_bytes,0,sizeof expected_bytes);
    tex_t t={0}; t.serial=7; t.pool=pool; t.off=64; t.mem=storage+64;
    t.d=(nvn_texdesc){.target=target,.format=compressed ? 0x42 : 0x29,.width=17,.height=9,.depth=target==2 ? 5 : target==4 ? 3 : 1,.levels=3};
    nvn_tex_layout(&t.d,&t.L); assert(t.L.size+128<sizeof storage); memset(storage+64,0,t.L.size);
    *pool=(pool_t){.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED,.mem=storage,.size=t.L.size+128};
    *fmt=(vkfmt_t){.vk=1,.bw=compressed ? 4 : 1,.bh=compressed ? 4 : 1,.bpb=8};
    *im=(vkimg_t){.w=t.d.width,.h=t.d.height,.d=target==2 ? t.d.depth : 1,.layers=t.L.layers,.levels=t.d.levels,
        .img=(VkImage)(uintptr_t)1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.layout=VK_IMAGE_LAYOUT_UNDEFINED};
    for(unsigned lv=0;lv<im->levels;lv++) for(unsigned ly=0;ly<im->layers;ly++) {
        unsigned w=im->w>>lv ? im->w>>lv : 1,h=im->h>>lv ? im->h>>lv : 1,z=im->d>>lv ? im->d>>lv : 1;
        size_t pitch=((w+fmt->bw-1)/fmt->bw)*fmt->bpb,n=pitch*((h+fmt->bh-1)/fmt->bh)*z;
        assert(n<=sizeof expected[lv][ly]); expected_bytes[lv][ly]=n;
        for(size_t i=0;i<n;i++) expected[lv][ly][i]=zeros ? 0 : (uint8_t)(17*i+31*lv+43*ly+1);
        assert(!nvn_swizzle(&t.d,lv,ly,expected[lv][ly],pitch,storage+64));
    }
    return t;
}
static void no_copy(vkimg_t *im,const tex_t *t,const vkfmt_t *f) {
    int before=copies; assert(!initial_content(im,t,f) && copies==before && protected_ranges==0);
}
int main(void) {
    pool_t pool; vkimg_t im; vkfmt_t fmt; tex_t t;
    for(int target=1;target<=4;target++) if(target!=3) for(int compressed=0;compressed<2;compressed++) {
        t=make_texture(&pool,&im,&fmt,target,compressed,0); was_flushed=1;
        assert(initial_content(&im,&t,&fmt)); assert(copies==(int)(im.levels*im.layers));
        assert(write_notices==copies);
        assert(!protected_ranges && !span_reads && !im.packaged && !im.pkg_mem && g_pkg_n==1 && !errors);
        unsigned before=copies; t.serial++; no_copy(&im,&t,&fmt); assert(copies==(int)before);
        im.gpu_w=1; no_copy(&im,&t,&fmt); // a new NVN object sharing the image must not overwrite GPU results
    }
    t=make_texture(&pool,&im,&fmt,2,0,1); was_flushed=1;
    assert(initial_content(&im,&t,&fmt) && copies==3); // explicitly published zero texels are valid too
    for(int disabled=0;disabled<2;disabled++) {
        t=make_texture(&pool,&im,&fmt,2,0,0); was_flushed=1; flush_option=disabled ? "0" : "1";
        assert(initial_content(&im,&t,&fmt)==!disabled);
    }
    for(int no_cpu=0;no_cpu<2;no_cpu++) for(int zeros=0;zeros<2;zeros++) {
        t=make_texture(&pool,&im,&fmt,2,0,zeros); was_flushed=0;
        pool.flags=no_cpu ? NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS : NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED;
        assert(initial_content(&im,&t,&fmt)==!zeros);
    }
    for(int bad=0;bad<18;bad++) {
        t=make_texture(&pool,&im,&fmt,2,0,0); was_flushed=1;
        switch(bad) {
        case 0: was_flushed=0; break;
        case 1: pool.flags|=NVN_MEMORY_POOL_FLAGS_VIRTUAL; break;
        case 2: t.d.flags|=NVN_TEXTURE_FLAGS_COMPRESSIBLE; break;
        case 3: t.d.flags|=NVN_TEXTURE_FLAGS_SPARSE; break;
        case 4: t.d.flags|=NVN_TEXTURE_FLAGS_LINEAR; break;
        case 5: t.d.flags|=NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET; break;
        case 6: t.d.samples=4; break;
        case 7: t.d.target=NVN_TEXTURE_TARGET_BUFFER; break;
        case 8: im.aspect=VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case 9: im.gpu_w=1; break;
        case 10: t.has_layout=1; break;
        case 11: t.off=-1; break;
        case 12: pool.size=t.L.size; break;
        case 13: t.mem++; break;
        case 14: t.pool=NULL; break;
        case 15: t.mem=NULL; break;
        case 16: t.L.size=0; break;
        case 17: im.layout=VK_IMAGE_LAYOUT_GENERAL; break;
        }
        no_copy(&im,&t,&fmt);
    }
    t=make_texture(&pool,&im,&fmt,4,1,0); was_flushed=1; im.packaged=1;
    assert(pkg_upload(&im,&t,&fmt,1)); assert(copies==9 && protected_ranges==1 && span_reads==1);
    assert(im.pkg_mem==t.mem && im.pkg_len==t.L.size && im.pkg_stamp==123 && im.pkg_span==t.mem && im.packaged==2);
    for(size_t i=0;i<64;i++) assert(storage[i]==0xa7);
    for(size_t i=64+t.L.size;i<sizeof storage;i++) assert(storage[i]==0xa7);
    return 0;
}
