// Optional VK_EXT_descriptor_buffer transport. Descriptor contents are immutable
// until their submission fence, including the copies made for changed UBOs.
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include <stdlib.h>
#include <string.h>

typedef struct { VkDeviceSize size; VkDeviceAddress address; } db_buffer;
typedef struct { VkBuffer buffer; VkDeviceSize offset, range; VkFormat format; } db_view;
static nvn_hmap g_db_buffers = {.klen = sizeof(VkBuffer)}, g_db_views = {.klen = sizeof(VkBufferView)};
static pthread_mutex_t g_db_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_db_buffer_gen = 1;
static PFN_vkCreateBuffer g_db_create_buffer;
static PFN_vkDestroyBuffer g_db_destroy_buffer;
static PFN_vkCreateBufferView g_db_create_view;
static PFN_vkDestroyBufferView g_db_destroy_view;

static VkResult db_create_buffer(VkDevice device, const VkBufferCreateInfo *info, const VkAllocationCallbacks *alloc, VkBuffer *buffer) {
    VkBufferCreateInfo copy = *info; copy.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkResult result = g_db_create_buffer(device, &copy, alloc, buffer);
    if (result == VK_SUCCESS) {
        db_buffer *entry = calloc(1, sizeof *entry); if (!entry) die("[vk] cannot track addressable buffer"); entry->size = copy.size;
        pthread_mutex_lock(&g_db_mu); nvn_hm_put(&g_db_buffers, buffer, entry); pthread_mutex_unlock(&g_db_mu);
    }
    return result;
}
static void db_destroy_buffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *alloc) {
    if (buffer) {
        pthread_mutex_lock(&g_db_mu); free(nvn_hm_del(&g_db_buffers, &buffer));
        atomic_fetch_add_explicit(&g_db_buffer_gen, 1, memory_order_release); pthread_mutex_unlock(&g_db_mu);
    }
    g_db_destroy_buffer(device, buffer, alloc);
}
static VkResult db_create_view(VkDevice device, const VkBufferViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkBufferView *view) {
    VkResult result = g_db_create_view(device, info, alloc, view);
    if (result == VK_SUCCESS) {
        db_view *entry = malloc(sizeof *entry); if (!entry) die("[vk] cannot track texel-buffer descriptor");
        *entry = (db_view){info->buffer, info->offset, info->range, info->format};
        pthread_mutex_lock(&g_db_mu); nvn_hm_put(&g_db_views, view, entry); pthread_mutex_unlock(&g_db_mu);
    }
    return result;
}
static void db_destroy_view(VkDevice device, VkBufferView view, const VkAllocationCallbacks *alloc) {
    if (view) { pthread_mutex_lock(&g_db_mu); free(nvn_hm_del(&g_db_views, &view)); pthread_mutex_unlock(&g_db_mu); }
    g_db_destroy_view(device, view, alloc);
}
void vk_db_init(void) {
    if (!V.descriptor_buffer) return;
    if (!vkGetBufferDeviceAddress || !vkGetDescriptorEXT || !vkGetDescriptorSetLayoutSizeEXT ||
        !vkGetDescriptorSetLayoutBindingOffsetEXT || !vkCmdBindDescriptorBuffersEXT || !vkCmdSetDescriptorBufferOffsetsEXT)
        die("[vk] descriptor-buffer entry point unavailable");
    g_db_create_buffer = vkCreateBuffer; vkCreateBuffer = db_create_buffer;
    g_db_destroy_buffer = vkDestroyBuffer; vkDestroyBuffer = db_destroy_buffer;
    g_db_create_view = vkCreateBufferView; vkCreateBufferView = db_create_view;
    g_db_destroy_view = vkDestroyBufferView; vkDestroyBufferView = db_destroy_view;
}
VkResult vk_buffer_memory_allocate(const VkMemoryAllocateInfo *info, VkDeviceMemory *memory) {
    if (!V.descriptor_buffer) return vkAllocateMemory(V.dev, info, NULL, memory);
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
        if (!(((const VkMemoryAllocateFlagsInfo *)p)->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        return vkAllocateMemory(V.dev, info, NULL, memory);
    }
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, info->pNext, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo copy = *info; copy.pNext = &flags;
    return vkAllocateMemory(V.dev, &copy, NULL, memory);
}
static db_buffer db_buffer_info(VkBuffer buffer) {
    static __thread struct { VkBuffer buffer; uint64_t gen; db_buffer info; } cache[256];
    uint64_t gen = atomic_load_explicit(&g_db_buffer_gen, memory_order_acquire);
    unsigned k = (unsigned)(((uint64_t)(uintptr_t)buffer >> 4) ^ ((uint64_t)(uintptr_t)buffer >> 19)) % 256;
    if (cache[k].buffer == buffer && cache[k].gen == gen) return cache[k].info;
    pthread_mutex_lock(&g_db_mu); db_buffer *entry = nvn_hm_get(&g_db_buffers, &buffer);
    if (!entry) die("[vk] untracked descriptor buffer resource");
    if (!entry->address) {
        VkBufferDeviceAddressInfo info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, buffer};
        entry->address = vkGetBufferDeviceAddress(V.dev, &info);
        if (!entry->address) die("[vk] buffer has no device address");
    }
    db_buffer result = *entry; pthread_mutex_unlock(&g_db_mu);
    cache[k].buffer = buffer; cache[k].gen = gen; cache[k].info = result; return result;
}

typedef struct { VkBuffer buffer; VkDeviceSize offset, size; VkDeviceAddress address; uint8_t *data; } db_set;
static db_set *db_new_set(VkDeviceSize size) {
    VkDeviceSize align = V.dbprops.descriptorBufferOffsetAlignment;
    VkDeviceSize prefix = (sizeof(db_set) + align - 1) / align * align;
    if (size > UINT64_MAX - prefix) die("[vk] descriptor size overflow");
    VkBuffer buffer; VkDeviceSize offset;
    db_set *set = vk_db_scratch(prefix + size, &buffer, &offset);
    *set = (db_set){buffer, offset + prefix, size, db_buffer_info(buffer).address, (uint8_t *)set + prefix};
    if (!size) set->offset = 0; // an empty layout still uses an in-bounds binding offset
    return set;
}
static size_t db_descriptor_size(VkDescriptorType type) {
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT *p = &V.dbprops;
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: return p->samplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return p->combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return p->sampledImageDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return p->storageImageDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return p->uniformTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return p->storageTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return p->uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return p->storageBufferDescriptorSize;
    default: die("[vk] unsupported descriptor-buffer descriptor type %d", type);
    }
}
static void db_write(db_set *set, uint32_t count, const VkWriteDescriptorSet *writes, const VkDeviceSize *offsets) {
    for (uint32_t i = 0; i < count; i++) {
        const VkWriteDescriptorSet *w = &writes[i]; size_t stride = db_descriptor_size(w->descriptorType);
        if (!stride || offsets[i] > set->size || w->dstArrayElement > (set->size - offsets[i]) / stride)
            die("[vk] descriptor array offset outside its native layout");
        uint64_t start = offsets[i] + (uint64_t)w->dstArrayElement * stride;
        if (w->descriptorCount > (set->size - start) / stride)
            die("[vk] descriptor write outside its native layout");
        for (uint32_t j = 0; j < w->descriptorCount; j++) {
            VkDescriptorGetInfoEXT info = {VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, NULL, w->descriptorType};
            VkDescriptorAddressInfoEXT address = {VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            VkDescriptorImageInfo image = {0}; VkSampler sampler = VK_NULL_HANDLE;
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const VkDescriptorBufferInfo *b = &w->pBufferInfo[j]; db_buffer buffer = db_buffer_info(b->buffer);
                if (b->offset > buffer.size) die("[vk] descriptor offset outside its buffer");
                address.address = buffer.address + b->offset; address.range = b->range == VK_WHOLE_SIZE ? buffer.size - b->offset : b->range;
                if (!address.range || address.range > buffer.size - b->offset) die("[vk] descriptor range outside its buffer");
                if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) info.data.pUniformBuffer = &address; else info.data.pStorageBuffer = &address;
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                VkBufferView handle = w->pTexelBufferView[j];
                pthread_mutex_lock(&g_db_mu); db_view *entry = nvn_hm_get(&g_db_views, &handle);
                if (!entry) die("[vk] untracked texel-buffer view");
                db_view view = *entry; pthread_mutex_unlock(&g_db_mu);
                db_buffer buffer = db_buffer_info(view.buffer);
                if (view.offset > buffer.size) die("[vk] texel descriptor outside its buffer");
                address.address = buffer.address + view.offset; address.range = view.range == VK_WHOLE_SIZE ? buffer.size - view.offset : view.range; address.format = view.format;
                if (!address.range || address.range > buffer.size - view.offset) die("[vk] texel descriptor range outside its buffer");
                if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) info.data.pUniformTexelBuffer = &address; else info.data.pStorageTexelBuffer = &address;
            } else {
                image = w->pImageInfo[j]; sampler = image.sampler;
                switch (w->descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER: info.data.pSampler = &sampler; break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: info.data.pCombinedImageSampler = &image; break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: info.data.pSampledImage = &image; break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: info.data.pStorageImage = &image; break;
                default: die("[vk] unsupported image descriptor type");
                }
            }
            vkGetDescriptorEXT(V.dev, &info, stride, set->data + start + (uint64_t)j * stride);
        }
    }
}
VkDescriptorSet vk_db_encode(VkDeviceSize size, uint32_t count, const VkWriteDescriptorSet *writes, const VkDeviceSize *offsets) {
    db_set *set = db_new_set(size); memset(set->data, 0, size); db_write(set, count, writes, offsets); return (VkDescriptorSet)set;
}
VkDescriptorSet vk_db_patch(VkDescriptorSet source, uint32_t count, const VkWriteDescriptorSet *writes, const VkDeviceSize *offsets) {
    const db_set *old = (const db_set *)source; db_set *set = db_new_set(old->size);
    memcpy(set->data, old->data, old->size); db_write(set, count, writes, offsets); return (VkDescriptorSet)set;
}
int vk_db_equal(VkDescriptorSet a, VkDescriptorSet b) {
    if (!a || !b) return a == b;
    const db_set *x = (const db_set *)a, *y = (const db_set *)b;
    return x->size == y->size && !memcmp(x->data, y->data, x->size);
}

extern uint64_t g_cmd_gen;
static __thread struct { uint64_t command_gen, bind_gen; VkCommandBuffer command; uint32_t count; VkDeviceAddress addresses[5]; } g_db_bound;
static __thread struct { uint64_t bind_gen; VkPipelineLayout layout; uint32_t count, indices[5]; VkDeviceSize offsets[5]; } g_db_point[2];
void vk_db_invalidate(void) { g_db_bound.bind_gen++; memset(g_db_point, 0, sizeof g_db_point); }
void vk_db_bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t count, const VkDescriptorSet *sets) {
    if (!count) return;
    if (count > 5) die("[vk] too many descriptor-buffer sets");
    VkDeviceAddress addresses[5] = {0}; VkDeviceSize offsets[5] = {0}; uint32_t indices[5] = {0}, n = 0;
    for (uint32_t i = 0; i < count; i++) if (sets[i]) {
        const db_set *set = (const db_set *)sets[i]; uint32_t k = 0;
        while (k < n && addresses[k] != set->address) k++;
        if (k == n) addresses[n++] = set->address;
        indices[i] = k; offsets[i] = set->offset;
    }
    if (!n) return;
    int fresh = g_db_bound.command_gen != g_cmd_gen || g_db_bound.command != command;
    if (fresh || g_db_bound.count != n || memcmp(g_db_bound.addresses, addresses, n * sizeof *addresses)) {
        VkDescriptorBufferBindingInfoEXT bindings[5];
        for (uint32_t i = 0; i < n; i++) bindings[i] = (VkDescriptorBufferBindingInfoEXT){VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, NULL,
            addresses[i], VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
        vkCmdBindDescriptorBuffersEXT(command, n, bindings); g_db_bound.command_gen = g_cmd_gen; g_db_bound.command = command; g_db_bound.count = n;
        memcpy(g_db_bound.addresses, addresses, n * sizeof *addresses); g_db_bound.bind_gen++;
    }
    unsigned p = point == VK_PIPELINE_BIND_POINT_COMPUTE; // buffer addresses are shared by both bind points; offsets are not
    if (fresh || g_db_point[p].bind_gen != g_db_bound.bind_gen || g_db_point[p].layout != layout || g_db_point[p].count != count ||
        memcmp(g_db_point[p].indices, indices, count * sizeof *indices) || memcmp(g_db_point[p].offsets, offsets, count * sizeof *offsets)) {
        vkCmdSetDescriptorBufferOffsetsEXT(command, point, layout, 0, count, indices, offsets);
        g_db_point[p].bind_gen = g_db_bound.bind_gen; g_db_point[p].layout = layout; g_db_point[p].count = count;
        memcpy(g_db_point[p].indices, indices, count * sizeof *indices); memcpy(g_db_point[p].offsets, offsets, count * sizeof *offsets);
    }
}
