// Runtime descriptor transport with a deterministic Vulkan driver and arena.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static struct { VkDevice dev; int descriptor_buffer; VkPhysicalDeviceDescriptorBufferPropertiesEXT dbprops; } V;
static int expect_failure;
_Noreturn static void die(const char *fmt, ...) { (void)fmt; assert(expect_failure); _Exit(77); }
static unsigned next_buffer=1, next_view=1, address_queries, descriptor_writes, buffer_binds, offset_binds;
static uint64_t address_version=1;
static int want_flags;
static const void *want_next;
static VkResult make_buffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks*a,VkBuffer*b) {
    (void)d;(void)a;assert(i->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);*b=(VkBuffer)(uintptr_t)next_buffer++;return VK_SUCCESS;
}
static void destroy_buffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks*a){(void)d;(void)b;(void)a;}
static VkResult make_view(VkDevice d,const VkBufferViewCreateInfo*i,const VkAllocationCallbacks*a,VkBufferView*v){(void)d;(void)i;(void)a;*v=(VkBufferView)(uintptr_t)next_view++;return VK_SUCCESS;}
static void destroy_view(VkDevice d,VkBufferView v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;}
static VkResult allocate_memory(VkDevice d,const VkMemoryAllocateInfo*i,const VkAllocationCallbacks*a,VkDeviceMemory*m) {
    (void)d;(void)a;assert(i->allocationSize==4096);
    if(want_flags) { const VkMemoryAllocateFlagsInfo*f=i->pNext;assert(f->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        assert(f->flags&VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);assert(f->pNext==want_next);
    } else assert(i->pNext==want_next);
    *m=(VkDeviceMemory)(uintptr_t)1;return VK_SUCCESS;
}
static VkDeviceAddress buffer_address(VkDevice d,const VkBufferDeviceAddressInfo*i) {
    (void)d;address_queries++;return address_version*0x100000000ull+(uintptr_t)i->buffer*0x100000ull;
}
static void descriptor(VkDevice d,const VkDescriptorGetInfoEXT*i,size_t size,void*out) {
    (void)d;assert(size>=32);descriptor_writes++;uint64_t words[4]={(uint64_t)i->type,0,0,0};
    if(i->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || i->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
       i->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || i->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
        const VkDescriptorAddressInfoEXT*a=i->data.pUniformBuffer;words[1]=a->address;words[2]=a->range;words[3]=a->format;
    } else if(i->type==VK_DESCRIPTOR_TYPE_SAMPLER) words[1]=(uintptr_t)*i->data.pSampler;
    else {const VkDescriptorImageInfo*p=i->data.pCombinedImageSampler;words[1]=(uintptr_t)p->imageView;words[2]=(uintptr_t)p->sampler;words[3]=p->imageLayout;}
    memset(out,0xcc,size);memcpy(out,words,sizeof words);
}
static void layout_size(VkDevice d,VkDescriptorSetLayout l,VkDeviceSize*s){(void)d;(void)l;*s=4096;}
static void binding_offset(VkDevice d,VkDescriptorSetLayout l,uint32_t b,VkDeviceSize*o){(void)d;(void)l;*o=256*b;}
static uint32_t last_count,last_indices[5];static VkDeviceSize last_offsets[5];
static void bind_buffers(VkCommandBuffer c,uint32_t n,const VkDescriptorBufferBindingInfoEXT*b) {
    (void)c;assert(n>0&&n<=5);for(unsigned i=0;i<n;i++){assert(b[i].address);assert(b[i].usage==(VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT|VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT));}buffer_binds++;
}
static void bind_offsets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const uint32_t*i,const VkDeviceSize*o) {
    (void)c;(void)p;(void)l;assert(first==0&&n<=5);last_count=n;memcpy(last_indices,i,n*sizeof*i);memcpy(last_offsets,o,n*sizeof*o);offset_binds++;
}
static PFN_vkCreateBuffer vkCreateBuffer=make_buffer;
static PFN_vkDestroyBuffer vkDestroyBuffer=destroy_buffer;
static PFN_vkCreateBufferView vkCreateBufferView=make_view;
static PFN_vkDestroyBufferView vkDestroyBufferView=destroy_view;
static PFN_vkAllocateMemory vkAllocateMemory=allocate_memory;
static PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress=buffer_address;
static PFN_vkGetDescriptorEXT vkGetDescriptorEXT=descriptor;
static PFN_vkGetDescriptorSetLayoutSizeEXT vkGetDescriptorSetLayoutSizeEXT=layout_size;
static PFN_vkGetDescriptorSetLayoutBindingOffsetEXT vkGetDescriptorSetLayoutBindingOffsetEXT=binding_offset;
static PFN_vkCmdBindDescriptorBuffersEXT vkCmdBindDescriptorBuffersEXT=bind_buffers;
static PFN_vkCmdSetDescriptorBufferOffsetsEXT vkCmdSetDescriptorBufferOffsetsEXT=bind_offsets;
static _Alignas(256) unsigned char arena[1024*1024];static size_t used;
static VkBuffer arena_buffer;
void *vk_db_scratch(VkDeviceSize size,VkBuffer*b,VkDeviceSize*o) {
    used=(used+255)&~(size_t)255;assert(size<=sizeof arena-used);*b=arena_buffer;*o=used;void*p=arena+used;used+=(size_t)size;return p;
}
uint64_t g_cmd_gen;
#include "descriptor_buffer_runtime.inc"
static void *presentation_invalidate(void *unused) { (void)unused; vk_db_invalidate(); return NULL; }

static VkBuffer buffer(VkDeviceSize size) {
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};VkBuffer b;
    assert(vkCreateBuffer(V.dev,&ci,NULL,&b)==VK_SUCCESS);return b;
}
static uint64_t word(VkDescriptorSet handle,size_t at) {uint64_t v;memcpy(&v,((db_set*)handle)->data+at,8);return v;}
int main(int argc, char **argv) {
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=4096};VkDeviceMemory memory;
    assert(vk_buffer_memory_allocate(&ai,&memory)==VK_SUCCESS);
    V.descriptor_buffer=1;V.dbprops.descriptorBufferOffsetAlignment=256;
    V.dbprops.uniformBufferDescriptorSize=V.dbprops.storageBufferDescriptorSize=64;
    V.dbprops.uniformTexelBufferDescriptorSize=V.dbprops.storageTexelBufferDescriptorSize=64;
    V.dbprops.samplerDescriptorSize=V.dbprops.sampledImageDescriptorSize=V.dbprops.storageImageDescriptorSize=64;
    V.dbprops.combinedImageSamplerDescriptorSize=128;
    vk_db_init();want_flags=1;
    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ai.pNext=&dedicated;want_next=&dedicated;assert(vk_buffer_memory_allocate(&ai,&memory)==VK_SUCCESS);
    assert(ai.pNext==&dedicated);
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.pNext=&dedicated,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    ai.pNext=&flags;assert(vk_buffer_memory_allocate(&ai,&memory)==VK_SUCCESS);
    flags.flags=VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT;assert(vk_buffer_memory_allocate(&ai,&memory)==VK_ERROR_FEATURE_NOT_PRESENT);
    arena_buffer=buffer(sizeof arena);VkBuffer ub=buffer(65536),sb=buffer(4096),tb=buffer(16384);
    VkBufferViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=tb,.format=VK_FORMAT_R32_UINT,.offset=256,.range=VK_WHOLE_SIZE};VkBufferView view;
    assert(vkCreateBufferView(V.dev,&view_info,NULL,&view)==VK_SUCCESS);
    VkDescriptorBufferInfo buffers[2]={{ub,512,64},{sb,128,VK_WHOLE_SIZE}};
    VkDescriptorImageInfo images[2]={{(VkSampler)(uintptr_t)20,(VkImageView)(uintptr_t)21,VK_IMAGE_LAYOUT_GENERAL},{(VkSampler)(uintptr_t)22,(VkImageView)(uintptr_t)23,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4]={
        {.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.pBufferInfo=&buffers[0]},
        {.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=2,.pImageInfo=images},
        {.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.descriptorCount=1,.pTexelBufferView=&view},
        {.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.pBufferInfo=&buffers[1]}};
    VkDeviceSize offsets[4]={512,0,2048,1024};
    VkDescriptorSet original=vk_db_encode(4096,4,writes,offsets);assert(descriptor_writes==5);
    assert(word(original,0)==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER&&word(original,8)==21&&word(original,128+8)==23);
    assert(word(original,512+8)==db_buffer_info(ub).address+512&&word(original,512+16)==64);
    assert(word(original,1024+16)==4096-128&&word(original,2048+16)==16384-256&&word(original,2048+24)==VK_FORMAT_R32_UINT);
    assert(((db_set*)original)->offset%256==0);
    unsigned queried=address_queries;assert(db_buffer_info(ub).address);assert(queried==address_queries);
    unsigned char saved[4096];memcpy(saved,((db_set*)original)->data,sizeof saved);
    buffers[0].offset=768;VkDescriptorSet changed=vk_db_patch(original,1,writes,offsets);
    assert(!vk_db_equal(original,changed)&&descriptor_writes==6);assert(word(changed,520)==db_buffer_info(ub).address+768);
    assert(!memcmp(saved,((db_set*)original)->data,sizeof saved));
    VkDescriptorSet rebuilt=vk_db_encode(4096,4,writes,offsets);assert(vk_db_equal(changed,rebuilt));
    VkDescriptorSet empty=vk_db_encode(0,0,NULL,NULL);assert(((db_set*)empty)->offset==0&&((db_set*)empty)->size==0);
    VkDescriptorSet subarray;writes[1].dstArrayElement=1;writes[1].descriptorCount=1;
    subarray=vk_db_patch(original,1,writes+1,offsets+1);assert(word(subarray,8)==21&&word(subarray,136)==21);
    g_cmd_gen=1;VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)1;VkPipelineLayout layout=(VkPipelineLayout)(uintptr_t)1;
    VkDescriptorSet sets[3]={original,NULL,changed};vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);
    assert(buffer_binds==1&&offset_binds==1&&last_count==3&&last_indices[0]==0&&last_indices[1]==0&&last_indices[2]==0);
    assert(last_offsets[0]==((db_set*)original)->offset&&last_offsets[1]==0&&last_offsets[2]==((db_set*)changed)->offset);
    vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(buffer_binds==1&&offset_binds==1);
    sets[0]=rebuilt;vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(buffer_binds==1&&offset_binds==2);
    vk_db_bind(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,3,sets);assert(buffer_binds==1&&offset_binds==3);
    vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(offset_binds==3);
    pthread_t presentation;assert(!pthread_create(&presentation,NULL,presentation_invalidate,NULL));assert(!pthread_join(presentation,NULL));
    vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(offset_binds==3); // unrelated command-recording thread
    vk_db_invalidate();vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(buffer_binds==1&&offset_binds==4);
    g_cmd_gen++;vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(buffer_binds==2&&offset_binds==5);
    db_set other=*(db_set*)changed;other.address+=0x1000000;sets[2]=(VkDescriptorSet)&other;
    vk_db_bind(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,3,sets);assert(buffer_binds==3&&offset_binds==6&&last_indices[2]==1);
    sets[2]=changed;vk_db_bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);assert(buffer_binds==4&&offset_binds==7);
    // A Vulkan handle reused after destruction must acquire its new address.
    uint64_t old_address=db_buffer_info(ub).address;unsigned old_id=(unsigned)(uintptr_t)ub;
    vkDestroyBuffer(V.dev,ub,NULL);next_buffer=old_id;address_version++;
    ub=buffer(65536);assert(db_buffer_info(ub).address!=old_address);
    unsigned bindings_before=buffer_binds,offsets_before=offset_binds;
    vk_db_bind((VkCommandBuffer)(uintptr_t)2,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,sets);
    assert(buffer_binds==bindings_before+1&&offset_binds==offsets_before+1); // a distinct command buffer, even in one generation
    // Native layouts bound every write; an invalid range fails instead of overwriting the next set.
    if(argc>1) {
        expect_failure=1;
        if(!strcmp(argv[1],"layout")) {VkDeviceSize bad=4096;vk_db_encode(4096,1,writes,&bad);assert(0);}
        assert(!strcmp(argv[1],"buffer"));buffers[0].offset=65535;buffers[0].range=64;
        vk_db_encode(4096,1,writes,offsets);assert(0);
    }
    vkDestroyBufferView(V.dev,view,NULL);
    nvn_hm_clear(&g_db_views,free);nvn_hm_clear(&g_db_buffers,free);
    free(g_db_views.keys);free(g_db_views.vals);free(g_db_buffers.keys);free(g_db_buffers.vals);
    puts("PASS");return 0;
}
