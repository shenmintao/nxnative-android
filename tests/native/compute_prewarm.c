#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"
#define DM_SYNC_SCOPE(name) ((void)0)
#define LOG(...) ((void)0)
#define VKLOG(...) ((void)0)
#define PRIO_PROCESS 0
#define _SC_NPROCESSORS_ONLN 1
#define setpriority(...) ((void)0)
#define sysconf(...) 8
#define pthread_setname_np(...) ((void)0)
typedef struct { uint64_t key; VkDescriptorSetLayout dsl; int set; } vkshader_t;
static struct { VkDevice dev; VkPipelineCache pcache; int descriptor_buffer; } V = {(VkDevice)2, (VkPipelineCache)3, 1};
static vkshader_t shaders[2] = {{17, (VkDescriptorSetLayout)4, 0}, {18, (VkDescriptorSetLayout)5, 0}};
static _Atomic unsigned created, destroyed, observed;
static unsigned graphics_calls;
static int race, fail_create;
static pthread_barrier_t barrier;
static vkshader_t *vk_shader(uint64_t key, int stage) { assert(stage == 0 || stage == 5); return key == 17 ? shaders : key == 18 ? shaders+1 : NULL; }
static VkShaderModule vk_shader_module(vkshader_t *cs, uint64_t um, uint64_t im, uint32_t xo, int fd) {
    assert(!xo && !fd); return (VkShaderModule)(uintptr_t)(100 + cs->key + um * 10 + im * 100);
}
static VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int nsets) {
    assert(nsets == 1 && (sets[0] == shaders[0].dsl || sets[0] == shaders[1].dsl)); return (VkPipelineLayout)9;
}
static uint64_t pipeline_observe_start(void) { atomic_fetch_add(&observed, 1); return 1; }
static void pipeline_observe_end(const char *kind, uint64_t start) { assert(!strcmp(kind,"compute") && start <= 1); }
static VkResult vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    assert(device == V.dev && cache == V.pcache && count == 1 && !alloc);
    assert(info->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO && info->layout == (VkPipelineLayout)9);
    assert(info->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && !strcmp(info->stage.pName, "main"));
    assert(info->flags == (V.descriptor_buffer ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0));
    assert(info->stage.module != VK_NULL_HANDLE);
    unsigned id = atomic_fetch_add(&created, 1) + 10;
    if (race) pthread_barrier_wait(&barrier);
    if (fail_create) return VK_ERROR_UNKNOWN;
    *out = (VkPipeline)(uintptr_t)id; return VK_SUCCESS;
}
static void vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
    assert(device == V.dev && (uintptr_t)pipeline >= 10 && !alloc); atomic_fetch_add(&destroyed, 1);
}
/* PRODUCTION HELPERS */
static void reset(void) {
    if (g_prec) { pipe_record_flush(); fclose(g_prec); }
    g_prec = NULL; free(g_pw); g_pw = NULL; g_pw_n = g_pw_next = g_pw_done = 0;
    nvn_hm_clear(&g_cpipes, NULL); nvn_hm_clear(&g_pipes, NULL);
    created = destroyed = observed = graphics_calls = 0;
}
static void file_with(uint32_t magic, uint32_t stride, const pkey *keys, size_t count, int torn) {
    FILE *file = fopen("vk_pipelines.bin", "wb"); assert(file);
    uint32_t header[2] = {magic, stride}; assert(fwrite(header, 8, 1, file) == 1);
    assert(fwrite(keys, sizeof(pkey), count, file) == count);
    if (torn) fputc(0x99, file);
    fclose(file);
}
static void *contend(void *arg) {
    uint64_t key[3] = {17, 91, 47};
    return compute_pipeline(shaders, (VkPipelineLayout)9, key, (int)(intptr_t)arg);
}
int main(void) {
    assert(sizeof(pkey) == 1000);
    assert(offsetof(pkey, kind) == 936 && offsetof(pkey, um) == 944 && offsetof(pkey, im) == 968);
    putenv("DM_ROOT=."); putenv("DM_VK_NOPREWARM=1");
    pkey keys[3] = {{.key={17}, .kind=99}, {.key={18}, .kind=1, .um={3}, .im={4}}, {.kind=77}};
    file_with(PIPE_MAGIC_V1, sizeof(pkey), keys, 1, 1);
    prewarm_start(); assert(g_pw_n == 1 && g_pw[0].kind == 0);
    prewarm_thread(NULL); assert(graphics_calls == 1 && created == 0 && g_pw_done == 1);
    pipe_record_flush(); fclose(g_prec); g_prec = NULL;
    FILE *file = fopen("vk_pipelines.bin", "rb"); uint32_t header[2];
    assert(fread(header, 8, 1, file) == 1 && header[0] == PIPE_MAGIC && header[1] == sizeof(pkey));
    assert(fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long)(8 + sizeof(pkey))); fclose(file);
    reset(); keys[0].kind = 1;
    file_with(PIPE_MAGIC_V1, sizeof(pkey), keys, 1, 0); prewarm_start();
    assert(g_pw_n == 1 && g_pw[0].kind == 0 && g_pw[0].fdz == 2);
    reset(); keys[0].kind = 0; keys[0].fdz = 0;
    file_with(PIPE_MAGIC, sizeof(pkey), keys, 3, 0); prewarm_start(); prewarm_thread(NULL);
    assert(graphics_calls == 1 && created == 1 && observed == 0 && g_pw_done == 2);
    uint64_t key[3] = {18, 3, 4};
    VkPipeline cached = compute_pipeline(shaders+1, (VkPipelineLayout)9, key, 1);
    assert(cached && created == 1 && observed == 0);
    key[2] = 5; VkPipeline variant = compute_pipeline(shaders+1, (VkPipelineLayout)9, key, 1);
    assert(variant && variant != cached && created == 2 && observed == 1);
    pipe_record_flush();
    assert(ftell(g_prec) == (long)(8 + 4*sizeof(pkey)));
    pipe_record_flush(); fclose(g_prec); g_prec = NULL;
    file = fopen("vk_pipelines.bin", "rb"); assert(file && !fseek(file, 8+3*sizeof(pkey), SEEK_SET));
    pkey recorded; assert(fread(&recorded, sizeof recorded, 1, file) == 1); fclose(file);
    assert(recorded.kind == 1 && recorded.key[0] == 18 && recorded.um[0] == 3 && recorded.im[0] == 5);
    reset(); file_with(PIPE_MAGIC, sizeof(pkey)+8, keys, 3, 0); prewarm_start(); assert(g_pw_n == 0);
    reset(); file_with(0x42424242, sizeof(pkey), keys, 3, 0); prewarm_start(); assert(g_pw_n == 0);
    reset(); g_prec = tmpfile(); assert(g_prec);
    pthread_barrier_init(&barrier, NULL, 2); race = 1;
    pthread_t a, b; pthread_create(&a, NULL, contend, (void *)1); pthread_create(&b, NULL, contend, NULL);
    void *pa, *pb; pthread_join(a, &pa); pthread_join(b, &pb); race = 0;
    assert(pa == pb && pa && created == 2 && destroyed == 1 && g_cpipes.n == 1);
    pthread_barrier_destroy(&barrier);
    reset(); g_prec = tmpfile(); assert(g_prec); fail_create = 1;
    key[0] = 18; key[1] = 7; key[2] = 8;
    assert(!compute_pipeline(shaders+1, (VkPipelineLayout)9, key, 1));
    assert(!compute_pipeline(shaders+1, (VkPipelineLayout)9, key, 1));
    assert(created == 1 && ftell(g_prec) == 0); reset();
    return 0;
}
