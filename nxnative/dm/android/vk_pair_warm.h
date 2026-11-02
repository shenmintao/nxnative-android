// WarmTask pair grouping / after-startup ordering from nxnative 65501fbb.
// Adreno's costly unit is usually the transformed VS/FS pair. A throwaway
// pipeline warms V.pcache without retaining a PSO or recording a PPE2 key.
#include "vk_pair_queue.h"

static vk_pair_queue g_pair_q;
static pthread_mutex_t g_pair_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pair_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_pair_once = PTHREAD_ONCE_INIT;
static int g_pair_on, g_pair_workers, g_pair_ready;
static uint64_t g_pair_queued, g_pair_warmed, g_pair_skipped, g_pair_failed, g_pair_ns;
static void pair_config(void) { const char *e = getenv("DM_VK_PAIR_WARM"); g_pair_on = !e || atoi(e); }
static void pair_warm_ready(void) {
    pthread_mutex_lock(&g_pair_mu); g_pair_ready = 1; pthread_cond_broadcast(&g_pair_cv); pthread_mutex_unlock(&g_pair_mu);
}

// A canonical input must match the module's numeric class, so no vertex
// retyping (vin) changes the shader we mean to warm. Unsupported interfaces
// stay demand work. Only ordinary location-decorated 32-bit scalars/vecs.
static int pair_formats(const vkshader_t *s, uint32_t storage, VkFormat *formats, unsigned limit) {
    if (!s->spv || s->nspv < 5 || !s->spv[3] || s->spv[3] > s->nspv * 16ull + 4096) return 0;
    const uint32_t *w = s->spv; uint32_t bound = w[3];
    uint32_t *defs = calloc(bound, sizeof *defs), *loc = malloc((size_t)bound * sizeof *loc);
    int ok = 0; if (!defs || !loc) goto done;
    memset(loc, 255, (size_t)bound * sizeof *loc);
    for (uint32_t at = 5; at < s->nspv;) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535;
        if (!n || n > s->nspv - at) goto done;
        if (op == 21 || op == 22 || op == 23 || op == 32) {
            if (n < 2 || !x[1] || x[1] >= bound) goto done; defs[x[1]] = at;
        }
        if (op == 71 && n == 4 && x[2] == 30) { if (x[1] >= bound) goto done; loc[x[1]] = x[3]; }
        at += n;
    }
    for (uint32_t at = 5; at < s->nspv; at += w[at] >> 16) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16;
        if ((x[0] & 65535) != 59 || n < 4 || x[3] != storage) continue;
        if (x[2] >= bound || x[1] >= bound) goto done;
        if (loc[x[2]] == UINT32_MAX) continue;   // BuiltIns have no location
        if (loc[x[2]] >= limit || !defs[x[1]]) goto done;
        const uint32_t *p = w + defs[x[1]];
        if ((p[0] & 65535) != 32 || p[0] >> 16 != 4 || p[2] != storage || p[3] >= bound || !defs[p[3]]) goto done;
        p = w + defs[p[3]];
        if ((p[0] & 65535) == 23) {
            if (p[0] >> 16 != 4 || !p[3] || p[3] > 4 || p[2] >= bound || !defs[p[2]]) goto done; p = w + defs[p[2]];
        }
        VkFormat fmt;
        if ((p[0] & 65535) == 22 && p[0] >> 16 == 3 && p[2] == 32)
            fmt = storage == 1 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        else if ((p[0] & 65535) == 21 && p[0] >> 16 == 4 && p[2] == 32)
            fmt = p[3] ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
        else goto done;
        if (formats[loc[x[2]]]) goto done; formats[loc[x[2]]] = fmt;
    }
    ok = 1;
done:
    free(defs); free(loc); return ok;
}
static int pair_canonical(const vk_pair_key *pair, vkshader_t *const stg[3], pkey *k) {
    if (!stg[0] || !stg[1] || stg[0]->xfb) return 0;
    if (stg[2]) {   // The canonical topology is triangle class, as in the probe.
        int triangles = 0;
        for (uint32_t at = 5; at < stg[2]->nspv;) {
            const uint32_t *x = stg[2]->spv + at; uint32_t n = x[0] >> 16;
            if (!n || n > stg[2]->nspv - at) return 0;
            if ((x[0] & 65535) == 16 && n >= 3 && x[2] == 22) triangles = 1;
            at += n;
        }
        if (!triangles) return 0;
    }
    VkFormat attr[16] = {0}, colour[8] = {0};
    if (!pair_formats(stg[0], 1, attr, 16) || !pair_formats(stg[1], 3, colour, 8)) return 0;
    memset(k, 0, sizeof *k); memcpy(k->key, pair->key, sizeof k->key);
    k->topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; k->mode = 2; k->samples = 1;
    k->dt = k->dw = 1; k->df = 2; k->dfmt = VK_FORMAT_D32_SFLOAT;
    for (unsigned i = 0; i < 16; i++) if (attr[i]) {
        k->attr[k->nattr++] = (typeof(k->attr[0])){i, 0, attr[i], i * 16}; k->nbind = 1;
    }
    if (k->nbind) k->bind[0].stride = 256;
    for (unsigned i = 0; i < 8; i++) if (colour[i]) k->ncol = i + 1;
    for (unsigned i = 0; i < k->ncol; i++) {
        k->cfmt[i] = colour[i]; k->wmask[i] = 15;
        k->bl[i].src = k->bl[i].srca = 2; k->bl[i].dst = k->bl[i].dsta = 1; k->bl[i].eq = k->bl[i].eqa = 1;
    }
    return 1;
}
static void *pair_worker(void *unused) {
    (void)unused; vk_compile_thread("vk pair warm");
    for (;;) {
        vk_pair_key pair;
        pthread_mutex_lock(&g_pair_mu);
        while (!g_pair_ready || !vk_pair_pop(&g_pair_q, &pair)) pthread_cond_wait(&g_pair_cv, &g_pair_mu);
        pthread_mutex_unlock(&g_pair_mu);
        uint64_t start = tnow(); vkshader_t *stg[3] = {0};
        for (int s = 0; s < 3; s++) if (pair.key[s]) stg[s] = vk_shader(pair.key[s], s);
        pkey key; int result = 0;
        if (!stg[0] || !stg[1] || (pair.key[2] && !stg[2])) result = -1;
        else if (pair_canonical(&pair, stg, &key)) {
            VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
            for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
            VkPipelineLayout layout = vk_pipeline_layout(dsl, nsets); result = 1;
            for (int fd = 0; fd <= !stg[1]->depth_repl; fd++) {
                key.fdz = fd; VkPipeline p = build_pipeline(&key, layout, stg);
                if (p && p != (VkPipeline)1) vkDestroyPipeline(V.dev, p, NULL); else result = -1;
            }
        }
        __atomic_add_fetch(&g_pair_ns, tnow() - start, __ATOMIC_RELAXED);
        if (result > 0) __atomic_add_fetch(&g_pair_warmed, 1, __ATOMIC_RELAXED);
        else if (result < 0) __atomic_add_fetch(&g_pair_failed, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&g_pair_skipped, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
void vk_pair_warm_program(const uint8_t keys[][8], unsigned known, int n) {
    pthread_once(&g_pair_once, pair_config);
    if (!g_pair_on || !V.dev) return;
    uint64_t values[6] = {0}; int stages[6]; vk_pair_key pair;
    if (n < 2 || n > 3 || known != (1u << n) - 1) goto skip;
    for (int i = 0; i < n; i++) { values[i] = key64(keys[i]); stages[i] = vk_shader_stage(values[i]); }
    if (!vk_pair_stages(values, stages, n, V.has_gs, &pair) || vk_shader_is_xfb(pair.key[0])) goto skip;
    pthread_mutex_lock(&g_pair_mu);
    if (!g_pair_workers) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nc >= 8 ? 4 : nc >= 4 ? 2 : 1;
        for (int i = 0; i < nt; i++) { pthread_t t; if (!pthread_create(&t, NULL, pair_worker, NULL)) { pthread_detach(t); g_pair_workers++; } }
        if (!g_pair_workers) { pthread_mutex_unlock(&g_pair_mu); goto skip; }
        VKLOG("pair warm: enabled, %d workers after startup prewarm (DM_VK_PAIR_WARM=0 disables it)", g_pair_workers);
    }
    int added = vk_pair_push(&g_pair_q, &pair);
    if (added > 0) { __atomic_add_fetch(&g_pair_queued, 1, __ATOMIC_RELAXED); pthread_cond_signal(&g_pair_cv); }
    pthread_mutex_unlock(&g_pair_mu);
    if (added > 0) return;
skip:
    __atomic_add_fetch(&g_pair_skipped, 1, __ATOMIC_RELAXED);
}
void vk_pair_warm_report(void) {
    pthread_once(&g_pair_once, pair_config);
    pthread_mutex_lock(&g_pair_mu); unsigned high = g_pair_q.high, pending = g_pair_q.head - g_pair_q.tail; pthread_mutex_unlock(&g_pair_mu);
    VKLOG("pair warm: on=%d queued=%llu warmed=%llu skipped=%llu failed=%llu warm_ms=%.3f queue_high=%u pending=%u", g_pair_on,
        (unsigned long long)__atomic_load_n(&g_pair_queued, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_pair_warmed, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_pair_skipped, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_pair_failed, __ATOMIC_RELAXED),
        __atomic_load_n(&g_pair_ns, __ATOMIC_RELAXED) / 1e6, high, pending);
}
__attribute__((constructor)) static void pair_set_on(void) {
    extern void (*g_nvn_on_program)(const uint8_t (*)[8], unsigned, int); g_nvn_on_program = vk_pair_warm_program;
}
