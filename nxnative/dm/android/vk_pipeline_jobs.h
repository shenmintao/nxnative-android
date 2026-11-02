// Bounded speculative builds, adapted from nxnative c646d43 pso_pend_*.
// A draw never skips: take a finished pipeline, steal a queued build, or wait
// for its exact running build. Workers own keys and immortal shader/layout handles.
enum { PIPE_PREDICT_JOBS = 2 };
typedef struct { pkey key; VkPipelineLayout layout; vkshader_t *stages[3]; uint64_t generation; int used, running; } pipe_predict_job;
static pipe_predict_job g_pipe_jobs[PIPE_PREDICT_JOBS];
static pthread_mutex_t g_pipe_jobs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pipe_jobs_cv = PTHREAD_COND_INITIALIZER;
static int g_pipe_workers;
static uint64_t g_pipe_pred_queued, g_pipe_pred_done, g_pipe_pred_stolen, g_pipe_pred_waits, g_pipe_pred_wait_ns;
static VkPipeline pipeline_publish(const pkey *key, VkPipeline p) {
    pthread_mutex_lock(&g_pipe_mu); VkPipeline old = nvn_hm_get(&g_pipes, key);
    if (old && old != (VkPipeline)2) { if (p != (VkPipeline)1 && p != old) vkDestroyPipeline(V.dev, p, NULL); p = old; }
    else { nvn_hm_put(&g_pipes, key, p); if (p != (VkPipeline)1) pipe_record(key); }
    pthread_mutex_unlock(&g_pipe_mu); return p;
}
static void *pipeline_predict_worker(void *unused) {
    (void)unused; vk_compile_thread("vk pipe predict");
    for (;;) {
        pthread_mutex_lock(&g_pipe_jobs_mu); int slot;
        for (;;) {
            for (slot = 0; slot < PIPE_PREDICT_JOBS; slot++) if (g_pipe_jobs[slot].used && !g_pipe_jobs[slot].running) break;
            if (slot < PIPE_PREDICT_JOBS) break;
            pthread_cond_wait(&g_pipe_jobs_cv, &g_pipe_jobs_mu);
        }
        g_pipe_jobs[slot].running = 1; pipe_predict_job job = g_pipe_jobs[slot]; pthread_mutex_unlock(&g_pipe_jobs_mu);
        pthread_mutex_lock(&g_pipe_mu); VkPipeline have = nvn_hm_get(&g_pipes, &job.key); pthread_mutex_unlock(&g_pipe_mu);
        if (!have) pipeline_publish(&job.key, build_pipeline(&job.key, job.layout, job.stages));
        __atomic_add_fetch(&g_pipe_pred_done, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_pipe_jobs_mu);
        if (g_pipe_jobs[slot].generation == job.generation) g_pipe_jobs[slot].used = 0;
        pthread_cond_broadcast(&g_pipe_jobs_cv); pthread_mutex_unlock(&g_pipe_jobs_mu);
    }
    return NULL;
}
static int pipeline_predict_kick(const pkey *key, VkPipelineLayout layout, vkshader_t *const stages[3]) {
    pthread_mutex_lock(&g_pipe_mu); VkPipeline have = nvn_hm_get(&g_pipes, key); pthread_mutex_unlock(&g_pipe_mu);
    if (have) return 0;
    pthread_mutex_lock(&g_pipe_jobs_mu); int slot = -1;
    for (int i = 0; i < PIPE_PREDICT_JOBS; i++) {
        if (g_pipe_jobs[i].used && !memcmp(&g_pipe_jobs[i].key, key, sizeof *key)) { pthread_mutex_unlock(&g_pipe_jobs_mu); return 0; }
        if (!g_pipe_jobs[i].used) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&g_pipe_jobs_mu); return 0; }
    if (!g_pipe_workers) {
        for (int i = 0; i < PIPE_PREDICT_JOBS; i++) { pthread_t thread;
            if (!pthread_create(&thread, NULL, pipeline_predict_worker, NULL)) { pthread_detach(thread); g_pipe_workers++; } }
        if (!g_pipe_workers) { pthread_mutex_unlock(&g_pipe_jobs_mu); VKLOG("pipeline prediction workers unavailable: demand compilation"); return 0; }
        VKLOG("pipeline prediction enabled: %d workers, at most %d outstanding builds", g_pipe_workers, PIPE_PREDICT_JOBS);
    }
    pipe_predict_job *job = &g_pipe_jobs[slot]; job->key = *key; job->layout = layout; memcpy(job->stages, stages, sizeof job->stages);
    job->generation++; job->used = 1; job->running = 0;
    __atomic_add_fetch(&g_pipe_pred_queued, 1, __ATOMIC_RELAXED);
    pthread_cond_signal(&g_pipe_jobs_cv); pthread_mutex_unlock(&g_pipe_jobs_mu); return 1;
}
static VkPipeline pipeline_predict_take(const pkey *key) {
    pthread_mutex_lock(&g_pipe_jobs_mu); int slot;
    for (slot = 0; slot < PIPE_PREDICT_JOBS; slot++) if (g_pipe_jobs[slot].used && !memcmp(&g_pipe_jobs[slot].key, key, sizeof *key)) break;
    if (slot == PIPE_PREDICT_JOBS) { pthread_mutex_unlock(&g_pipe_jobs_mu); return VK_NULL_HANDLE; }
    pipe_predict_job *job = &g_pipe_jobs[slot];
    if (!job->running) {
        job->used = 0; job->generation++; __atomic_add_fetch(&g_pipe_pred_stolen, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_pipe_jobs_mu); return VK_NULL_HANDLE;
    }
    uint64_t generation = job->generation, start = tick(); __atomic_add_fetch(&g_pipe_pred_waits, 1, __ATOMIC_RELAXED);
    while (job->used && job->generation == generation) pthread_cond_wait(&g_pipe_jobs_cv, &g_pipe_jobs_mu);
    pthread_mutex_unlock(&g_pipe_jobs_mu); __atomic_add_fetch(&g_pipe_pred_wait_ns, tick() - start, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_pipe_mu); VkPipeline p = nvn_hm_get(&g_pipes, key); pthread_mutex_unlock(&g_pipe_mu); return p;
}
void vk_pipeline_predict_report(void) {
    if (g_pipe_predict_on) VKLOG("pipeline prediction: %llu queued, %llu completed, %llu stolen, %llu waits / %.3f ms",
        (unsigned long long)__atomic_load_n(&g_pipe_pred_queued, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_pipe_pred_done, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_pipe_pred_stolen, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&g_pipe_pred_waits, __ATOMIC_RELAXED),
        __atomic_load_n(&g_pipe_pred_wait_ns, __ATOMIC_RELAXED) / 1e6);
}
