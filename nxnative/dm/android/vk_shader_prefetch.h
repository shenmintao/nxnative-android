// Program-creation prefetch, adapted from nxnative c646d43 mtl_prog_prefetch.
// Own only bundle keys: the game can finalize its program as soon as SetShaders
// returns. Queue pressure never changes a draw; it loads synchronously instead.
enum { SHADER_FETCH_N = 256 };
typedef struct { uint64_t key, stage; } shader_fetch_key;
static shader_fetch_key g_shader_fetch[SHADER_FETCH_N];
static unsigned g_shader_fetch_head, g_shader_fetch_tail;
static nvn_hmap g_shader_fetch_seen = {.klen = sizeof(shader_fetch_key)};
static pthread_mutex_t g_shader_fetch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shader_fetch_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_shader_fetch_once = PTHREAD_ONCE_INIT;
static int g_shader_fetch_on, g_shader_fetch_started;
static uint64_t g_shader_fetch_queued, g_shader_fetch_done, g_shader_fetch_full;
static void shader_fetch_config(void) {
    const char *e = getenv("DM_VK_SHADER_PREFETCH"); g_shader_fetch_on = !e || atoi(e);
}
static void *shader_fetch_worker(void *unused) {
    (void)unused; vk_compile_thread("vk shader fetch");
    for (;;) {
        pthread_mutex_lock(&g_shader_fetch_mu);
        while (g_shader_fetch_head == g_shader_fetch_tail) pthread_cond_wait(&g_shader_fetch_cv, &g_shader_fetch_mu);
        shader_fetch_key key = g_shader_fetch[g_shader_fetch_tail++ % SHADER_FETCH_N];
        pthread_mutex_unlock(&g_shader_fetch_mu);
        (void)vk_shader(key.key, (int)key.stage);
        __atomic_add_fetch(&g_shader_fetch_done, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
void vk_shader_prefetch(uint64_t key, int stage) {
    pthread_once(&g_shader_fetch_once, shader_fetch_config);
    if (!g_shader_fetch_on || !V.dev || !key || stage < 0 || stage > 5) return;
    if (stage == 3 || stage == 4 || (stage == 2 && !V.has_gs)) return;   // unsupported stages stay on the demand path
    shader_fetch_key k = {key, (uint64_t)stage};
    pthread_mutex_lock(&g_shader_fetch_mu);
    if (nvn_hm_get(&g_shader_fetch_seen, &k)) { pthread_mutex_unlock(&g_shader_fetch_mu); return; }
    if (g_shader_fetch_head - g_shader_fetch_tail == SHADER_FETCH_N || g_shader_fetch_started < 0) {
        __atomic_add_fetch(&g_shader_fetch_full, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_shader_fetch_mu); return;
    }
    if (!g_shader_fetch_started) {
        pthread_t thread; int r = pthread_create(&thread, NULL, shader_fetch_worker, NULL);
        if (r) { g_shader_fetch_started = -1; pthread_mutex_unlock(&g_shader_fetch_mu); VKLOG("shader prefetch worker unavailable (%d): demand loading", r); return; }
        pthread_detach(thread); g_shader_fetch_started = 1;
        VKLOG("shader prefetch at program creation enabled (DM_VK_SHADER_PREFETCH=0 disables it)");
    }
    nvn_hm_put(&g_shader_fetch_seen, &k, (void *)1);
    g_shader_fetch[g_shader_fetch_head++ % SHADER_FETCH_N] = k;
    __atomic_add_fetch(&g_shader_fetch_queued, 1, __ATOMIC_RELAXED);
    pthread_cond_signal(&g_shader_fetch_cv); pthread_mutex_unlock(&g_shader_fetch_mu);
}
void vk_shader_prefetch_report(void) {
    pthread_once(&g_shader_fetch_once, shader_fetch_config);
    if (g_shader_fetch_on) VKLOG("shader prefetch: %llu queued, %llu completed, %llu deferred to demand",
        (unsigned long long)__atomic_load_n(&g_shader_fetch_queued, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_shader_fetch_done, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_shader_fetch_full, __ATOMIC_RELAXED));
}
