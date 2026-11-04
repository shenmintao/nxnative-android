#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "mali_probe.h"
typedef struct { uint32_t w,h,d,levels,layers,samples; VkFormat fmt; VkImageViewType vtype; VkImageAspectFlags aspect; VkImageUsageFlags usage; VkImageLayout layout; VkImage img; VkDeviceMemory mem; VkImageView full; void *sub; uint64_t pool,off; } vkimg_t;
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static struct { int pass_open,ncol; struct { uint32_t blend_en; } color; VkFormat cfmt[8]; } S;
static struct { vkimg_t *im; uint32_t fmt,layers; VkImageUsageFlags usage; } P_att[8];
static uint64_t g_tex_frame;
static int allocations,blits,ended,blend32;
static VkFormat allocated_format;
static VkImageBlit regions[2];
#define VKLOG(...) ((void)0)
static uint32_t vk_mem_type(uint32_t a,uint32_t b,uint32_t c) { return 0; }
static VkDeviceMemory vk_image_memory(const VkMemoryRequirements *r,uint32_t mt,VkDeviceSize *offset,void **sub) { return (VkDeviceMemory)(uintptr_t)3; }
static void vk_defer_free_view(VkImageView v) { }
static void vk_defer_free_image_sub(VkImage i,void *s) { }
static void vk_defer_free_image(VkImage i,VkDeviceMemory m) { }
static VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void vk_img_barrier_all(VkCommandBuffer b) { }
static void vk_tex_written(vkimg_t *im) { }
static void vk_end_pass(void) { ended++;S.pass_open=0; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice d,VkFormat f,VkFormatProperties *p) { memset(p,0,sizeof *p);p->optimalTilingFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;if(f==VK_FORMAT_R16G16B16A16_SFLOAT||blend32)p->optimalTilingFeatures|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d,const VkImageCreateInfo *c,const VkAllocationCallbacks *a,VkImage *i) { allocations++;allocated_format=c->format;assert(c->extent.width==8&&c->extent.height==4);*i=(VkImage)(uintptr_t)2;return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks *a) { }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d,VkImage i,VkMemoryRequirements *r) { memset(r,0,sizeof *r);r->size=256;r->memoryTypeBits=1; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *v,const VkAllocationCallbacks *a,VkImageView *o) { assert(v->format==VK_FORMAT_R16G16B16A16_SFLOAT);*o=(VkImageView)(uintptr_t)4;return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer b,VkPipelineStageFlags a,VkPipelineStageFlags c,VkDependencyFlags d,uint32_t n,const VkMemoryBarrier *m,uint32_t o,const VkBufferMemoryBarrier *p,uint32_t q,const VkImageMemoryBarrier *r) { }
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageBlit *b,VkFilter f) { assert(blits<2&&n==1&&f==VK_FILTER_NEAREST);assert((src==(VkImage)(uintptr_t)1)==(blits==0));assert((dst==(VkImage)(uintptr_t)2)==(blits==0));regions[blits++]=*b; }
#ifndef DM_RELEASE
/* PROXY */
#endif
int main(int argc,char **argv) {
    assert(argc==3);(void)g_tex_frame;
#ifdef _WIN32
    _putenv_s("DM_VK_X_MALI_A",argv[1]);_putenv_s("DM_VK_X_SAMPLE_CAPS",argv[2]);_putenv_s("DM_VK_X_ROBUST",argv[1]);
#else
    setenv("DM_VK_X_MALI_A",argv[1],1);setenv("DM_VK_X_SAMPLE_CAPS",argv[2],1);setenv("DM_VK_X_ROBUST",argv[1],1);
#endif
    int mode=vk_mali_a_mode();
    assert(!vk_mali_robust(0x5143,1)&&!vk_mali_robust(0x13B5,0));   // never on Adreno, never unreported
#ifdef DM_RELEASE
    assert(!mode&&!vk_mali_u_mode()&&!vk_mali_robust(0x13B5,1));(void)V;(void)S;(void)P_att;(void)g_tex_frame;(void)ended;return 0;
#else
    assert(mode==(!strcmp(argv[1],"1")));assert(vk_mali_robust(0x13B5,1)==(!strcmp(argv[1],"1")));assert(vk_mali_u_mode()==(!strcmp(argv[2],"nearest")?2:!strcmp(argv[2],"log")?1:0));
    assert(!vk_mali_a_eligible(0,109,0,1,1,1));assert(!vk_mali_a_eligible(1,97,0,1,1,1));assert(!vk_mali_a_eligible(1,109,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,1,1,1));
    assert(!vk_mali_a_eligible(1,109,0,4,1,1));assert(!vk_mali_a_eligible(1,109,0,1,2,1));assert(!vk_mali_a_eligible(1,109,0,1,1,2));assert(vk_mali_a_eligible(1,109,0,1,1,1));
    vkimg_t src={.w=16,.h=8,.d=1,.samples=1,.fmt=VK_FORMAT_R32G32B32A32_SFLOAT,.img=(VkImage)(uintptr_t)1};
    blend32=1;assert(!mali_a_make(&src,1,0,1,0));assert(!allocations);blend32=0;
    S.pass_open=1;S.ncol=1;S.color.blend_en=1;S.cfmt[0]=109;P_att[0].im=&src;P_att[0].layers=1;mali_a_before_draw();assert(ended==mode);
    int made=mali_a_make(&src,1,0,1,0);assert(made==mode);assert(allocations==mode);
    if(mode){assert(allocated_format==97&&blits==1);assert(regions[0].srcSubresource.mipLevel==1&&regions[0].dstSubresource.mipLevel==0);P_mali_a[0].fmt=109;P_mali_a[0].usage=0x17;P_att[0].im=&P_mali_a[0].image;S.cfmt[0]=97;mali_a_finish();assert(blits==2);assert(regions[1].dstSubresource.mipLevel==1);assert(P_att[0].im==&src&&P_att[0].fmt==109&&S.cfmt[0]==109&&!P_mali_a[0].original&&src.fmt==109);}
    return 0;
#endif
}
