// Included after the unmodified production encoder and its mock entry points.
static _Atomic unsigned draw_next, direct_calls, gate_entered, gate_open;
static _Atomic unsigned pointer_checks, cmd_busy;
static unsigned want_draws;
static int gated;
static pthread_t producer;
static const void *fallback_next, *fallback_data;
static pthread_mutex_t scope_mu = PTHREAD_MUTEX_INITIALIZER;

static void command_enter(void) {
    assert(!atomic_exchange(&cmd_busy, 1));
    assert(pthread_equal(pthread_self(), g_vke_thread));
}
static void command_leave(void) { atomic_store(&cmd_busy, 0); atomic_fetch_add(&pointer_checks, 1); }
static void VKAPI_CALL fake_draw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t instance) {
    if (cb == (VkCommandBuffer)(uintptr_t)2) {
        assert(!g_vke_owner && !pthread_equal(pthread_self(), g_vke_thread));
        assert(vertices == 3 && instances == 1 && first == 77 && instance == 0);
        atomic_fetch_add(&direct_calls, 1); return;
    }
    assert(cb == (VkCommandBuffer)(uintptr_t)1 && vertices == 3 && instances == 2 && instance == (first ^ 0x1234u));
    assert(pthread_equal(pthread_self(), g_vke_thread));
    assert(atomic_fetch_add(&draw_next, 1) == first);
    if (gated && first == 0) {
        atomic_store(&gate_entered, 1);
        while (!atomic_load(&gate_open)) sched_yield();
    }
}
static void VKAPI_CALL fake_vertex(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && first == 5 && count == 3);
    assert((uintptr_t)buffers[0] == 111 && (uintptr_t)buffers[1] == 222 && (uintptr_t)buffers[2] == 333);
    assert(offsets[0] == 7 && offsets[1] == 21 && offsets[2] == 41); command_leave();
}
static void VKAPI_CALL fake_bindsets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
        uint32_t count, const VkDescriptorSet *sets, uint32_t dynamicCount, const uint32_t *offsets) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && point == VK_PIPELINE_BIND_POINT_GRAPHICS && (uintptr_t)layout == 9 && first == 2);
    assert(count == 2 && (uintptr_t)sets[0] == 121 && (uintptr_t)sets[1] == 122 && dynamicCount == 3);
    assert(offsets[0] == 8 && offsets[1] == 40 && offsets[2] == 80); command_leave();
}
static void VKAPI_CALL fake_push(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && (uintptr_t)layout == 9);
    assert(stages == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) && offset == 16 && size == 128);
    for (unsigned i = 0; i < size; ++i) assert(((const unsigned char *)values)[i] == (unsigned char)(i ^ 0x5a));
    command_leave();
}
static void VKAPI_CALL fake_blend(VkCommandBuffer cb, const float values[4]) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1);
    const uint32_t expected[4] = {0x3f800000, 0x80000000, 0x7fc12345, 0x3e800000};
    assert(!memcmp(values, expected, sizeof expected)); command_leave();
}
static void VKAPI_CALL fake_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags deps,
        uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && src == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT && deps == 0);
    assert(nm == 1 && nb == 2 && ni == 1 && !m->pNext && !b[0].pNext && !im->pNext);
    assert(m->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT && m->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
    assert((uintptr_t)b[0].buffer == 21 && (uintptr_t)b[1].buffer == 22 && b[1].offset == 17);
    assert((uintptr_t)im->image == 31 && im->subresourceRange.levelCount == 3 && im->subresourceRange.layerCount == 5); command_leave();
}
static void VKAPI_CALL fake_renderpass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    if (info->pNext == fallback_next) {
        assert(!atomic_load(&cmd_busy)); assert(pthread_equal(pthread_self(), producer));
        assert(atomic_load(&draw_next) == want_draws); return;
    }
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && contents == VK_SUBPASS_CONTENTS_INLINE);
    assert(info->renderArea.offset.x == -3 && info->renderArea.offset.y == 7 && info->renderArea.extent.width == 17 && info->renderArea.extent.height == 19);
    assert(info->clearValueCount == 2 && info->pClearValues[0].depthStencil.depth == .375f && info->pClearValues[0].depthStencil.stencil == 4);
    assert(info->pClearValues[1].color.uint32[3] == 8);
    const VkRenderPassAttachmentBeginInfo *p = info->pNext;
    assert(p && p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO && !p->pNext && p->attachmentCount == 2);
    assert((uintptr_t)p->pAttachments[0] == 91 && (uintptr_t)p->pAttachments[1] == 92); command_leave();
}
static void VKAPI_CALL fake_rendering(VkCommandBuffer cb, const VkRenderingInfo *info) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && !info->pNext && info->colorAttachmentCount == 2);
    assert((uintptr_t)info->pColorAttachments[0].imageView == 101 && (uintptr_t)info->pColorAttachments[1].imageView == 102);
    assert((uintptr_t)info->pDepthAttachment->imageView == 103 && info->pStencilAttachment->clearValue.depthStencil.depth == .3125f); command_leave();
}
static void VKAPI_CALL fake_descriptors(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *writes, uint32_t nc, const VkCopyDescriptorSet *copies) {
    if ((uintptr_t)device == 18) {
        assert(pthread_equal(pthread_self(), producer) && nw == 1 && nc == 0 && !copies);
        const VkWriteDescriptorSetInlineUniformBlock *p = writes[0].pNext;
        assert(p == fallback_next && p->dataSize == 16 && p->pData == fallback_data && !atomic_load(&g_vke_outstanding)); return;
    }
    command_enter(); assert((uintptr_t)device == 8 && nw == 3 && nc == 1);
    assert(writes[0].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && writes[0].descriptorCount == 2);
    assert((uintptr_t)writes[0].pBufferInfo[0].buffer == 11 && writes[0].pBufferInfo[1].offset == 19 && writes[0].pBufferInfo[1].range == 31);
    assert(writes[1].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && (uintptr_t)writes[1].pImageInfo[1].sampler == 42 && (uintptr_t)writes[1].pImageInfo[0].imageView == 51);
    assert(writes[2].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && (uintptr_t)writes[2].pTexelBufferView[1] == 72);
    assert((uintptr_t)copies[0].srcSet == 81 && (uintptr_t)copies[0].dstSet == 82 && copies[0].descriptorCount == 1); command_leave();
}
static void VKAPI_CALL fake_xfb(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets) {
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && first == 1 && count == 4 && !buffers && !offsets); command_leave();
}
static void VKAPI_CALL fake_copy(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions) {
    if (count == UINT32_MAX) { assert(regions == fallback_data && !atomic_load(&cmd_busy)); return; }
    command_enter(); assert(cb == (VkCommandBuffer)(uintptr_t)1 && (uintptr_t)src == 4 && (uintptr_t)dst == 5 && count == 1);
    assert(regions[0].srcOffset == 3 && regions[0].dstOffset == 7 && regions[0].size == 21); command_leave();
}
static void VKAPI_CALL fake_update(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) {
    assert(cb == (VkCommandBuffer)(uintptr_t)1 && (uintptr_t)dst == 6 && offset == 0 && size == 65536 && data == fallback_data);
    assert(pthread_equal(pthread_self(), producer) && atomic_load(&draw_next) == want_draws && !atomic_load(&cmd_busy));
}
static VkResult VKAPI_CALL fake_end(VkCommandBuffer cb) {
    assert(cb == (VkCommandBuffer)(uintptr_t)1 && atomic_load(&draw_next) == want_draws && !atomic_load(&cmd_busy));
    return VK_ERROR_OUT_OF_HOST_MEMORY; // synchronous errors must reach their original caller
}
static VkResult VKAPI_CALL fake_allocate(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out) {
    assert((uintptr_t)device == 8 && info->commandBufferCount == 1 && !atomic_load(&cmd_busy));
    assert(!atomic_load(&g_vke_outstanding)); *out = (VkCommandBuffer)(uintptr_t)7; return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_wait(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    assert((uintptr_t)device == 8 && count == 1 && (uintptr_t)*fences == 6 && all == VK_TRUE && timeout == 17 && !atomic_load(&g_vke_outstanding));
    return VK_TIMEOUT;
}
static void *other_thread(void *unused) {
    (void)unused; vkCmdDraw((VkCommandBuffer)(uintptr_t)2, 3, 1, 77, 0); return NULL;
}
static void *release_when_full(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_vke_mu); int full = g_vke_stats.backpressure != 0; pthread_mutex_unlock(&g_vke_mu);
        if (full) { atomic_store(&gate_open, 1); return NULL; }
        sched_yield();
    }
}
static void *new_scope_owner(void *unused) {
    (void)unused;
    for (unsigned round = 0; round < 5; ++round) {
        pthread_mutex_lock(&scope_mu); vk_encode_begin(); assert(g_vke_owner);
        unsigned first = atomic_load(&draw_next);
        for (unsigned i = first; i < first + 10; ++i) vkCmdDraw((VkCommandBuffer)(uintptr_t)1, 3, 2, i, i ^ 0x1234u);
        vk_encode_end(); assert(!g_vke_owner); pthread_mutex_unlock(&scope_mu);
    }
    return NULL;
}
static void snapshots(VkCommandBuffer cb) {
    VkBuffer vb[3] = {(VkBuffer)(uintptr_t)111, (VkBuffer)(uintptr_t)222, (VkBuffer)(uintptr_t)333};
    VkDeviceSize offsets[3] = {7, 21, 41}; vkCmdBindVertexBuffers(cb, 5, 3, vb, offsets); memset(vb, 0, sizeof vb); memset(offsets, 0, sizeof offsets);
    VkDescriptorSet sets[2] = {(VkDescriptorSet)(uintptr_t)121, (VkDescriptorSet)(uintptr_t)122}; uint32_t dyn[3] = {8,40,80};
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkPipelineLayout)(uintptr_t)9,2,2,sets,3,dyn);memset(sets,0,sizeof sets);memset(dyn,0,sizeof dyn);
    unsigned char bytes[128]; for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i ^ 0x5a);
    vkCmdPushConstants(cb, (VkPipelineLayout)(uintptr_t)9, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof bytes, bytes); memset(bytes, 0, sizeof bytes);
    uint32_t color[4] = {0x3f800000, 0x80000000, 0x7fc12345, 0x3e800000}; vkCmdSetBlendConstants(cb, (const float *)color); memset(color, 0, sizeof color);
    VkMemoryBarrier m = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkBufferMemoryBarrier b[2] = {{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.buffer=(VkBuffer)(uintptr_t)21}, {.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.buffer=(VkBuffer)(uintptr_t)22,.offset=17}};
    VkImageMemoryBarrier im = {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.image=(VkImage)(uintptr_t)31,.subresourceRange={.levelCount=3,.layerCount=5}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 2, b, 1, &im); memset(&m, 0, sizeof m); memset(b, 0, sizeof b); memset(&im, 0, sizeof im);
    VkImageView views[2] = {(VkImageView)(uintptr_t)91, (VkImageView)(uintptr_t)92};
    VkClearValue clears[2] = {{.depthStencil={.375f,4}}, {.color={.uint32={5,6,7,8}}}};
    VkRenderPassAttachmentBeginInfo attachments = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,NULL,2,views};
    VkRenderPassBeginInfo rp = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.pNext=&attachments,.renderArea={{-3,7},{17,19}},.clearValueCount=2,.pClearValues=clears};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE); memset(views, 0, sizeof views); memset(clears, 0, sizeof clears); memset(&attachments, 0, sizeof attachments); memset(&rp, 0, sizeof rp);
    VkRenderingAttachmentInfo ca[2] = {{.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=(VkImageView)(uintptr_t)101}, {.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=(VkImageView)(uintptr_t)102}};
    VkRenderingAttachmentInfo ds = {.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=(VkImageView)(uintptr_t)103,.clearValue={.depthStencil={.3125f,3}}};
    VkRenderingInfo ri = {.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.colorAttachmentCount=2,.pColorAttachments=ca,.pDepthAttachment=&ds,.pStencilAttachment=&ds};
    vkCmdBeginRendering(cb, &ri); memset(ca, 0, sizeof ca); memset(&ds, 0, sizeof ds); memset(&ri, 0, sizeof ri);
    VkDescriptorBufferInfo bi[2] = {{(VkBuffer)(uintptr_t)11,17,29}, {(VkBuffer)(uintptr_t)12,19,31}};
    VkDescriptorImageInfo ii[2] = {{(VkSampler)(uintptr_t)41,(VkImageView)(uintptr_t)51,VK_IMAGE_LAYOUT_GENERAL}, {(VkSampler)(uintptr_t)42,(VkImageView)(uintptr_t)52,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkBufferView bv[2] = {(VkBufferView)(uintptr_t)71, (VkBufferView)(uintptr_t)72};
    VkWriteDescriptorSet w[3] = {{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.descriptorCount=2,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=bi}, {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.descriptorCount=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=ii}, {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.descriptorCount=2,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.pTexelBufferView=bv}};
    VkCopyDescriptorSet copy = {.sType=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,.srcSet=(VkDescriptorSet)(uintptr_t)81,.dstSet=(VkDescriptorSet)(uintptr_t)82,.descriptorCount=1};
    vkUpdateDescriptorSets((VkDevice)(uintptr_t)8,3,w,1,&copy); memset(bi,0,sizeof bi);memset(ii,0,sizeof ii);memset(bv,0,sizeof bv);memset(w,0,sizeof w);memset(&copy,0,sizeof copy);
    vkCmdBeginTransformFeedbackEXT(cb,1,4,NULL,NULL);
}
int main(void) {
    producer=pthread_self();
    vkCmdDraw=fake_draw;vkCmdBindVertexBuffers=fake_vertex;vkCmdBindDescriptorSets=fake_bindsets;vkCmdPushConstants=fake_push;vkCmdSetBlendConstants=fake_blend;
    vkCmdPipelineBarrier=fake_barrier;vkCmdBeginRenderPass=fake_renderpass;vkCmdBeginRendering=fake_rendering;
    vkUpdateDescriptorSets=fake_descriptors;vkCmdBeginTransformFeedbackEXT=fake_xfb;vkCmdCopyBuffer=fake_copy;vkCmdUpdateBuffer=fake_update;
    vkEndCommandBuffer=fake_end;vkAllocateCommandBuffers=fake_allocate;vkWaitForFences=fake_wait;
    vk_encode_init();
    env_set("DM_VK_ENCODE_THREAD","0");vk_encode_begin();vkCmdDraw((VkCommandBuffer)(uintptr_t)2,3,1,77,0);vk_encode_end();assert(direct_calls==1 && !g_vke_started);
    assert(!placement_calls);env_set("DM_PIN_REPLAY","prime");
    env_set("DM_VK_ENCODE_THREAD","1");vk_encode_begin();assert(g_vke_owner && g_vke_started);gated=1;
    VkCommandBuffer cb=(VkCommandBuffer)(uintptr_t)1;
    for(unsigned i=0;i<257;i++)vkCmdDraw(cb,3,2,i,i^0x1234u);
    while(!atomic_load(&gate_entered))sched_yield();
    snapshots(cb); // worker is still blocked: poison every original argument now
    pthread_t other;assert(!pthread_create(&other,NULL,other_thread,NULL));assert(!pthread_join(other,NULL));assert(direct_calls==2);
    pthread_t release;assert(!pthread_create(&release,NULL,release_when_full,NULL));
    want_draws=12000;
    for(unsigned i=257;i<want_draws;i++)vkCmdDraw(cb,3,2,i,i^0x1234u);
    assert(vkEndCommandBuffer(cb)==VK_ERROR_OUT_OF_HOST_MEMORY);assert(!pthread_join(release,NULL));assert(pointer_checks==9);
    assert(g_vke_stats.backpressure && g_vke_stats.blocks>VKE_BLOCKS && g_vke_stats.calls==want_draws+9);
    // Unknown extension graphs and oversized valid data preserve synchronous semantics.
    VkDeviceGroupRenderPassBeginInfo group={.sType=VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO,.deviceMask=1};
    VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.pNext=&group};fallback_next=&group;
    vkCmdBeginRenderPass(cb,&rp,VK_SUBPASS_CONTENTS_INLINE);
    uint32_t inline_data[4]={1,2,3,4};VkWriteDescriptorSetInlineUniformBlock inline_block={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,NULL,16,inline_data};
    VkWriteDescriptorSet inline_write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&inline_block,.descriptorCount=16,.descriptorType=VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
    fallback_next=&inline_block;fallback_data=inline_data;vkUpdateDescriptorSets((VkDevice)(uintptr_t)18,1,&inline_write,0,NULL);
    unsigned char large[65536]={0};fallback_data=large;vkCmdUpdateBuffer(cb,(VkBuffer)(uintptr_t)6,0,sizeof large,large);
    VkBufferCopy region={3,7,21};fallback_data=&region;vkCmdCopyBuffer(cb,(VkBuffer)(uintptr_t)4,(VkBuffer)(uintptr_t)5,UINT32_MAX,&region);
    vkCmdCopyBuffer(cb,(VkBuffer)(uintptr_t)4,(VkBuffer)(uintptr_t)5,1,&region);memset(&region,0,sizeof region);
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandBufferCount=1};VkCommandBuffer allocated;
    assert(vkAllocateCommandBuffers((VkDevice)(uintptr_t)8,&ai,&allocated)==VK_SUCCESS && (uintptr_t)allocated==7);
    VkFence fence=(VkFence)(uintptr_t)6;assert(vkWaitForFences((VkDevice)(uintptr_t)8,1,&fence,VK_TRUE,17)==VK_TIMEOUT);
    assert(pointer_checks==10 && g_vke_stats.fallbacks==4);vk_encode_end();assert(!g_vke_owner && !g_vke_current && !atomic_load(&g_vke_outstanding));
    pthread_t owners[2];for(unsigned i=0;i<2;i++)assert(!pthread_create(&owners[i],NULL,new_scope_owner,NULL));
    for(unsigned i=0;i<2;i++) assert(!pthread_join(owners[i],NULL));
    assert(draw_next==want_draws+100 && !g_vke_owner && placement_calls==4);
    // Repeated scope handoffs keep the queue reusable, with disabled calls direct.
    for(unsigned n=0;n<10;n++) {env_set("DM_VK_ENCODE_THREAD","0");vk_encode_begin();vkCmdDraw((VkCommandBuffer)(uintptr_t)2,3,1,77,0);vk_encode_end();env_set("DM_VK_ENCODE_THREAD","1");vk_encode_begin();vk_encode_end();}
    assert(direct_calls==12 && placement_calls==4);vk_encode_report();
    pthread_mutex_lock(&g_vke_mu);g_vke_stop=1;pthread_cond_signal(&g_vke_work);pthread_mutex_unlock(&g_vke_mu);assert(!pthread_join(g_vke_thread,NULL));
    unsigned free_blocks=0;for(vke_block *b=g_vke_free;b;b=b->next)free_blocks++;assert(free_blocks==VKE_BLOCKS);
    return 0;
}
