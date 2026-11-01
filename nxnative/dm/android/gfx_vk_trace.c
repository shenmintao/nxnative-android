// Install resource/driver observations only when explicitly requested. With
// tracing off Vulkan dispatch stays unchanged, including descriptor wrappers.
#include "gfx_vk.h"
#include "sync_trace.h"
#if !defined(DM_RELEASE) && (defined(__ANDROID__) || defined(DM_SYNC_TRACE_TEST))
#define DM_TRACE_VK(X) \
    X(vkCreateBuffer, (VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *o), (d,i,a,o), "work: create buffer") \
    X(vkCreateImage, (VkDevice d, const VkImageCreateInfo *i, const VkAllocationCallbacks *a, VkImage *o), (d,i,a,o), "work: create image") \
    X(vkCreateImageView, (VkDevice d, const VkImageViewCreateInfo *i, const VkAllocationCallbacks *a, VkImageView *o), (d,i,a,o), "work: create image view") \
    X(vkAllocateMemory, (VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *o), (d,i,a,o), "work: allocate resource memory") \
    X(vkCreateGraphicsPipelines, (VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o), (d,c,n,i,a,o), "work: driver create graphics pipeline") \
    X(vkCreateComputePipelines, (VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o), (d,c,n,i,a,o), "work: driver create compute pipeline")
#define DM_TRACE_WRAP(fn,signature,args,label) \
    static PFN_##fn trace_next_##fn; \
    static VkResult VKAPI_CALL trace_##fn signature { DM_SYNC_SCOPE(label); return trace_next_##fn args; }
DM_TRACE_VK(DM_TRACE_WRAP)
void dm_sync_vulkan_install(void) {
    if (!dm_sync_requested()) return;
#define DM_TRACE_INSTALL(fn,signature,args,label) if (fn && fn != trace_##fn) { trace_next_##fn = fn; fn = trace_##fn; }
    DM_TRACE_VK(DM_TRACE_INSTALL)
#undef DM_TRACE_INSTALL
}
#endif
