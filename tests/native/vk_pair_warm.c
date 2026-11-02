#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vk_pair_queue.h"
/* PIPELINE KEY */
typedef struct {
    int xfb, depth_repl, set;
    uint32_t *spv, nspv;
    VkDescriptorSetLayout dsl;
} vkshader_t;
static struct { VkDevice dev; int has_gs; } V = {(VkDevice)1, 1};
static void (*g_nvn_on_program)(const uint8_t (*)[8], unsigned, int);
static _Atomic unsigned built, destroyed, workers, fd_mask;
static int fail_pipeline;
static vkshader_t shaders[3];
static uint64_t tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void vk_compile_thread(const char *name) { assert(!strcmp(name, "vk pair warm")); atomic_fetch_add(&workers, 1); }
static uint64_t key64(const uint8_t key[8]) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | key[i]; return v; }
static int vk_shader_stage(uint64_t key) { return key >= 1 && key <= 6 ? (int)key - 1 : -1; }
static int vk_shader_is_xfb(uint64_t key) { return key == 99; }
static vkshader_t *vk_shader(uint64_t key, int stage) { return key == (unsigned)stage + 1 ? &shaders[stage] : NULL; }
static VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int n) { assert(n == 2 && sets[0] && sets[1]); return (VkPipelineLayout)7; }
static VkPipeline build_pipeline(const pkey *key, VkPipelineLayout layout, vkshader_t *const stages[3]) {
    assert(layout == (VkPipelineLayout)7 && stages[0] && stages[1]);
    assert(!key->vin && !key->um[0] && !key->im[0] && key->topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    atomic_fetch_or(&fd_mask, 1u << key->fdz); atomic_fetch_add(&built, 1);
    return fail_pipeline ? (VkPipeline)1 : (VkPipeline)8;
}
static void vkDestroyPipeline(VkDevice dev, VkPipeline pipeline, const VkAllocationCallbacks *allocator) {
    assert(dev == V.dev && pipeline == (VkPipeline)8 && !allocator); atomic_fetch_add(&destroyed, 1);
}
// Keep log arguments typechecked, without noisy output.
#define VKLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
static const char *pair_switch;
#define getenv(name) pair_switch
#define sysconf(name) 8
#include "vk_pair_warm.h"

static uint32_t spv[] = {
    0x07230203, 0x10000, 0, 20, 0,
    4u<<16|71, 7, 30, 3, 4u<<16|71, 8, 30, 0,
    3u<<16|22, 1, 32, 4u<<16|21, 2, 32, 0,
    4u<<16|23, 3, 1, 4,
    4u<<16|32, 4, 1, 3, 4u<<16|32, 5, 3, 2,
    4u<<16|59, 4, 7, 1, 4u<<16|59, 5, 8, 3,
};
static void queue_cases(void) {
    vk_pair_queue *q = calloc(1, sizeof *q); assert(q); vk_pair_key key, out;
    uint64_t values[] = {2,1,3}; int stages[] = {1,0,2};
    assert(vk_pair_stages(values, stages, 3, 1, &key) && key.key[0] == 1 && key.key[1] == 2 && key.key[2] == 3);
    assert(!vk_pair_stages(values, stages, 3, 0, &key));
    for (int st = -1; st < 6; st++) if (st != 0) { stages[1] = st; assert(!vk_pair_stages(values, stages, 3, 1, &key)); }
    stages[1] = 0; assert(!vk_pair_stages(values, stages, 1, 1, &key));
    for (unsigned i = 1; i <= PAIR_QUEUE_N; i++) { key = (vk_pair_key){{i,2,0}}; assert(vk_pair_push(q, &key) == 1); }
    key.key[0]++; assert(vk_pair_push(q, &key) == -1 && q->high == PAIR_QUEUE_N);
    assert(vk_pair_pop(q, &out) && out.key[0] == 1);
    assert(vk_pair_push(q, &key) == 1);   // pressure did not poison dedup
    assert(vk_pair_push(q, &key) == 0);
    while (vk_pair_pop(q, &out)) {}
    key.key[0] = 1; assert(vk_pair_push(q, &key) == 0);   // completed pair stays seen
    q->head = q->tail = UINT32_MAX - 4;
    for (unsigned i = PAIR_QUEUE_N + 2; i <= PAIR_SEEN_N; i++) {
        key.key[0] = i; assert(vk_pair_push(q, &key) == 1); assert(vk_pair_pop(q, &out));
    }
    key.key[0] = PAIR_SEEN_N + 1; assert(vk_pair_push(q, &key) == -1); free(q);
}
static void canonical_cases(void) {
    vk_pair_key pair = {{1,2,0}}; pkey key; vkshader_t *stages[] = {shaders,shaders+1,NULL};
    assert(sizeof(pkey) == 1000 && pair_canonical(&pair, stages, &key));
    assert(key.nattr == 1 && key.attr[0].loc == 3 && key.attr[0].off == 48 && key.attr[0].fmt == VK_FORMAT_R32G32B32A32_SFLOAT);
    assert(key.nbind == 1 && key.bind[0].stride == 256 && key.ncol == 1 && key.cfmt[0] == VK_FORMAT_R32G32B32A32_UINT);
    assert(key.dfmt == VK_FORMAT_D32_SFLOAT && key.samples == 1 && key.dt && key.dw && !key.vin && !key.fdz);
    shaders[0].xfb = 1; assert(!pair_canonical(&pair, stages, &key)); shaders[0].xfb = 0;
    uint32_t geometry[] = {0x07230203,0x10000,0,2,0, 3u<<16|16,1,22};
    stages[2] = shaders+2; shaders[2].spv = geometry; shaders[2].nspv = sizeof geometry/4;
    pair.key[2] = 3; assert(pair_canonical(&pair, stages, &key));
    geometry[7] = 20; assert(!pair_canonical(&pair, stages, &key)); stages[2] = NULL; pair.key[2] = 0;
    spv[19] = 1; assert(pair_canonical(&pair, stages, &key) && key.cfmt[0] == VK_FORMAT_R32G32B32A32_SINT); spv[19] = 0;
    spv[15] = 64; assert(!pair_canonical(&pair, stages, &key)); spv[15] = 32;
    uint32_t bad[] = {0x07230203,0,0,10,0,0}; shaders[0].spv = bad; shaders[0].nspv = 6;
    assert(!pair_canonical(&pair, stages, &key)); shaders[0].spv = spv; shaders[0].nspv = sizeof spv / 4;
}
int main(int argc, char **argv) {
    assert(argc == 2 && g_nvn_on_program == vk_pair_warm_program);
    for (int i = 0; i < 3; i++) shaders[i] = (vkshader_t){.set=i,.spv=spv,.nspv=sizeof spv/4,.dsl=(VkDescriptorSetLayout)(uintptr_t)(i+2)};
    queue_cases(); canonical_cases();
    if (!strcmp(argv[1],"off")) pair_switch = "0";
    uint8_t keys[2][8] = {{0},{0}}; keys[0][7] = 2; keys[1][7] = 1;
    vk_pair_warm_program(keys,3,2); memset(keys,0,sizeof keys);   // worker owns its copy
    usleep(20000); assert(!built);   // startup prewarm has priority
    if (!strcmp(argv[1],"off")) { assert(!g_pair_queued && !workers); return 0; }
    pair_switch = "0"; pair_warm_ready();   // startup switch is not re-read
    uint64_t deadline = tnow() + 2000000000;
    while (!__atomic_load_n(&g_pair_warmed,__ATOMIC_RELAXED) && tnow() < deadline) usleep(1000);
    assert(g_pair_warmed == 1 && built == 2 && destroyed == 2 && fd_mask == 3 && workers <= 4);
    keys[0][7]=1; keys[1][7]=2; vk_pair_warm_program(keys,3,2);
    assert(g_pair_queued == 1 && g_pair_skipped == 1);
    vk_pair_warm_program(keys,1,2); assert(g_pair_skipped == 2);
    vk_pair_warm_report(); return 0;
}
