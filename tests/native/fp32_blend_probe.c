#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "fp32_blend_probe.h"
/* KEY */
static struct { VkPhysicalDevice pd; } V;
static unsigned queries, messages;
static void note(const char *fmt, ...) { messages++; }
#define VKLOG note
static uint64_t fnv(uint64_t h, const void *p, size_t n) { return h; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out) {
    queries++;memset(out,0,sizeof *out);out->optimalTilingFeatures=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if(f==VK_FORMAT_R16G16B16A16_SFLOAT)out->optimalTilingFeatures|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
}
#ifndef DM_RELEASE
/* FUNCTION */
#endif
int main(int argc,char **argv) {
    assert(argc==2);
#ifdef _WIN32
    _putenv_s("DM_VK_X_FP32_BLEND",argv[1]);
#else
    setenv("DM_VK_X_FP32_BLEND",argv[1],1);
#endif
#ifdef DM_RELEASE
    (void)V;assert(vk_fp32_probe_mode()==0);return 0;
#else
    int mode=vk_fp32_probe_mode();assert(mode==(!strcmp(argv[1],"C")?3:!strcmp(argv[1],"log")?1:0));
    pkey k={0};k.ncol=3;k.cfmt[0]=VK_FORMAT_R32G32B32A32_SFLOAT;k.cfmt[1]=VK_FORMAT_R16G16B16A16_SFLOAT;k.cfmt[2]=VK_FORMAT_R32_SFLOAT;
    k.blend_en=3;k.wmask[0]=k.wmask[1]=k.wmask[2]=15;
    VkPipelineColorBlendAttachmentState blend[3]={{.blendEnable=1,.colorWriteMask=15},{.blendEnable=1,.colorWriteMask=15},{.blendEnable=0,.colorWriteMask=15}};
    VkPipelineColorBlendAttachmentState before[3];memcpy(before,blend,sizeof blend);
    fp32_probe_pipeline(&k,blend);
    assert(queries==(mode?2u:0u));assert((messages>0)==(mode>0));
    if(mode==3){assert(!blend[0].blendEnable);blend[0].blendEnable=1;}
    assert(!memcmp(before,blend,sizeof blend));assert(k.blend_en==3); // no key/default mutation, only C's unsupported attachment
    return 0;
#endif
}
