#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "nvn_obj.h"
#include "nvn_hmap.h"
#define VKLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
typedef struct { uint64_t key; int stage, set; VkDescriptorSetLayout dsl; uint32_t local[3]; int xfb, depth_repl; } vkshader_t;
typedef struct { int is_int; } vkfmt_t;
typedef struct { uint64_t fdz_frame; } vkimg_t;
static struct { VkDevice dev; int has_gs; } V = {.dev=(VkDevice)(uintptr_t)1,.has_gs=1};
static void vk_compile_thread(const char *name) { (void)name; }
static uint64_t tick(void) { struct timespec t; clock_gettime(CLOCK_REALTIME,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void wait_for(_Atomic unsigned *value, unsigned expected) {
    uint64_t until=tick()+UINT64_C(5000000000);
    while (atomic_load(value)<expected) { assert(tick()<until); struct timespec t={0,1000000}; nanosleep(&t,NULL); }
}
/* REPLAY TYPES */

#if CASE == 0
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static unsigned release;
static _Atomic unsigned loaded, entered;
static uint64_t first_key;
static nvn_hmap g_shaders={.klen=16}; static pthread_mutex_t g_sh_mu=PTHREAD_MUTEX_INITIALIZER;
static vkshader_t *vk_shader(uint64_t key,int stage) {
    (void)stage; pthread_mutex_lock(&gate);
    if (!first_key) first_key=key;
    atomic_fetch_add(&entered,1); pthread_cond_broadcast(&gate_cv);
    while (!release) pthread_cond_wait(&gate_cv,&gate);
    pthread_mutex_unlock(&gate); atomic_fetch_add(&loaded,1); return NULL;
}
#include "vk_shader_prefetch.h"
/* CACHED SHADER */
int main(int argc,char **argv) {
    assert(argc==2); if(!strcmp(argv[1],"off")) {
        putenv("DM_VK_SHADER_PREFETCH=0"); vk_shader_prefetch(1,0); assert(!g_shader_fetch_queued && !g_shader_fetch_started); return 0;
    }
    putenv("DM_VK_SHADER_PREFETCH=1");
    V.dev=VK_NULL_HANDLE;vk_shader_prefetch(1,0);assert(!g_shader_fetch_queued);V.dev=(VkDevice)(uintptr_t)1;
    vk_shader_prefetch(0,0);vk_shader_prefetch(1,-1);vk_shader_prefetch(1,6);assert(!g_shader_fetch_queued);
    vk_shader_prefetch(1,3);vk_shader_prefetch(1,4);V.has_gs=0;vk_shader_prefetch(1,2);V.has_gs=1;assert(!g_shader_fetch_queued);
    uint64_t owned=1;vk_shader_prefetch(owned,0);owned=999;wait_for(&entered,1);
    for(uint64_t key=2;key<=257;key++)vk_shader_prefetch(key,0);
    vk_shader_prefetch(1,0);vk_shader_prefetch(258,0);
    assert(g_shader_fetch_queued==257 && g_shader_fetch_full==1);
    pthread_mutex_lock(&gate);release=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate);
    wait_for(&loaded,257);assert(first_key==1 && owned==999);
    vk_shader_prefetch(258,0);wait_for(&loaded,258);assert(g_shader_fetch_queued==258);
    vkshader_t shader={.key=999};uint64_t k[2]={999,0};nvn_hm_put(&g_shaders,k,&shader);
    assert(vk_shader_cached(999,0)==&shader && !vk_shader_cached(998,0));
    pthread_mutex_lock(&g_sh_mu);assert(!vk_shader_cached(999,0));pthread_mutex_unlock(&g_sh_mu);
    nvn_hm_put(&g_shaders,k,(void*)1);assert(!vk_shader_cached(999,0));
    vk_shader_prefetch_report();puts("PASS owned bounded shader prefetch");return 0;
}
#endif

#if CASE == 1
static nvn_hmap g_pipes={.klen=sizeof(pkey)};static pthread_mutex_t g_pipe_mu=PTHREAD_MUTEX_INITIALIZER;
static int g_pipe_predict_on=1;
static _Atomic unsigned records,destroys,entered,active,max_active;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;static unsigned release[32];
static void pipe_record(const pkey *key) { (void)key;atomic_fetch_add(&records,1); }
static void vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a) { (void)d;(void)p;(void)a;atomic_fetch_add(&destroys,1); }
static VkPipeline build_pipeline(const pkey *key,VkPipelineLayout layout,vkshader_t *const shaders[3]) {
    (void)layout;(void)shaders;unsigned id=(unsigned)key->key[0];assert(id<32);
    pthread_mutex_lock(&gate);unsigned n=atomic_fetch_add(&active,1)+1;
    if(n>atomic_load(&max_active))atomic_store(&max_active,n);
    atomic_fetch_add(&entered,1);pthread_cond_broadcast(&gate_cv);
    while(!release[id])pthread_cond_wait(&gate_cv,&gate);
    atomic_fetch_sub(&active,1);pthread_mutex_unlock(&gate);
    return (VkPipeline)(uintptr_t)(id==13?1:1000+id);
}
#include "vk_pipeline_jobs.h"
static VkPipeline taken;static pkey demand;
static void *take_thread(void *unused) { (void)unused;taken=pipeline_predict_take(&demand);return NULL; }
static void open_gate(unsigned id) { pthread_mutex_lock(&gate);release[id]=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate); }
static void drained(void) {
    pthread_mutex_lock(&g_pipe_jobs_mu);
    while(g_pipe_jobs[0].used||g_pipe_jobs[1].used)pthread_cond_wait(&g_pipe_jobs_cv,&g_pipe_jobs_mu);
    pthread_mutex_unlock(&g_pipe_jobs_mu);
}
int main(void) {
    vkshader_t *shaders[3]={0};pkey a={.key={1}},b={.key={2}},c={.key={3}};
    g_pipe_workers=-1;assert(pipeline_predict_kick(&a,0,shaders));assert(pipeline_predict_kick(&b,0,shaders));
    assert(!pipeline_predict_kick(&a,0,shaders)&&!pipeline_predict_kick(&c,0,shaders));
    assert(!pipeline_predict_take(&b)&&g_pipe_pred_stolen==1);
    assert(pipeline_predict_kick(&c,0,shaders));assert(!pipeline_predict_take(&a));assert(!pipeline_predict_take(&c));
    g_pipe_workers=0;a.key[0]=10;b.key[0]=11;c.key[0]=12;
    assert(pipeline_predict_kick(&a,0,shaders)&&pipeline_predict_kick(&b,0,shaders));wait_for(&entered,2);
    assert(!pipeline_predict_kick(&c,0,shaders)&&atomic_load(&max_active)==2);
    demand=a;pthread_t waiter;pthread_create(&waiter,NULL,take_thread,NULL);
    uint64_t until=tick()+UINT64_C(5000000000);while(!__atomic_load_n(&g_pipe_pred_waits,__ATOMIC_RELAXED))assert(tick()<until);
    open_gate(10);pthread_join(waiter,NULL);assert(taken==(VkPipeline)(uintptr_t)1010);
    open_gate(11);drained();assert(atomic_load(&records)==2);
    assert(pipeline_publish(&a,(VkPipeline)(uintptr_t)9999)==taken&&atomic_load(&destroys)==1&&atomic_load(&records)==2);
    pkey failed={.key={13}};open_gate(13);assert(pipeline_predict_kick(&failed,0,shaders));drained();
    assert(nvn_hm_get(&g_pipes,&failed)==(VkPipeline)1&&atomic_load(&records)==2);
    for(unsigned id=14;id<30;id++) {
        pkey key={.key={id}};open_gate(id);assert(pipeline_predict_kick(&key,0,shaders));
        VkPipeline p=pipeline_predict_take(&key);
        if(!p)p=pipeline_publish(&key,build_pipeline(&key,0,shaders));
        assert(p==(VkPipeline)(uintptr_t)(1000+id));drained();
    }
    assert(atomic_load(&max_active)<=2);vk_pipeline_predict_report();puts("PASS exact-key wait steal publication");return 0;
}
#endif

#if CASE == 2
static vk_replay_state S;
static int g_pipe_predict_on=1,g_var_now,g_spv_alt,g_tracing,g_tf_on,g_rc_skip,g_x_nokeep;
static int g_no_cull,g_lower_left,g_flipy=1,g_x_nodepth,g_depth_zero=1,g_fdz=1,P_depth_copied;
static uint64_t g_tex_frame=10;
static const uint8_t *g_predict_next,*g_predict_returns[16];static unsigned g_predict_depth;
static struct {vkimg_t *im;} P_att[9];
static vkshader_t shaders[3]={{.key=1,.set=0},{.key=2,.set=1},{.key=11,.set=0}};
static unsigned queued,prefetched;static pkey keys[2];
static const vkfmt_t *vk_fmt(int format) {static vkfmt_t f;f.is_int=format==2;return &f;}
static VkFormat vertex_format(int f) {return f==1?VK_FORMAT_R32G32B32A32_SFLOAT:f==2?VK_FORMAT_R32G32B32A32_UINT:f==3?VK_FORMAT_R32G32B32A32_SINT:VK_FORMAT_UNDEFINED;}
int prog_key(const prog_t *p,unsigned i,uint8_t key[8]) {
    if(i>=p->nstage)return 0;for(int j=0;j<8;j++)key[j]=(uint8_t)(p->sd[i].data>>(56-8*j));return 1;
}
static int vk_shader_stage(uint64_t key) {return key==1||key==11?0:key==2?1:-1;}
static vkshader_t *vk_shader_cached(uint64_t key,int stage) {(void)stage;for(int i=0;i<3;i++)if(shaders[i].key==key)return &shaders[i];return NULL;}
static void vk_shader_prefetch(uint64_t key,int stage) {(void)key;(void)stage;prefetched++;}
static VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5],int n) {(void)sets;assert(n==2);return (VkPipelineLayout)(uintptr_t)1;}
static int pipeline_predict_kick(const pkey *key,VkPipelineLayout layout,vkshader_t *const stages[3]) {
    (void)layout;(void)stages;for(unsigned i=0;i<queued;i++)if(!memcmp(&keys[i],key,sizeof *key))return 0;
    assert(queued<2);keys[queued++]=*key;return 1;
}
/* KEY FUNCTIONS */
#include "vk_pipeline_scan.h"
static uint8_t stream[8192],child[512];static size_t end;
static void append(uint32_t op,const void *payload,size_t n) {
    size_t size=(8+n+7)&~(size_t)7;assert(end+size<=sizeof stream);op_hdr h={op,(uint32_t)size};
    memcpy(stream+end,&h,8);if(n)memcpy(stream+end+8,payload,n);end+=size;
}
static void init(void) {
    memset(&S,0,sizeof S);S.sh[0]=&shaders[0];S.sh[1]=&shaders[1];S.ncol=1;S.cfmt[0]=VK_FORMAT_R8G8B8A8_UNORM;S.samples=1;
    S.rt_n=1;S.rt_has_col[0]=1;S.rt_col[0].magic=MAGIC('T','E','X',0);S.rt_col[0].serial=100;S.rt_col[0].d.format=1;
    S.chmask.mask=15;S.poly.mode=2;S.poly.front=NVN_FRONT_FACE_CCW;S.color.logic_op=3;S.ds.df=2;
    end=queued=0;g_predict_depth=0;g_predict_next=stream;g_rc_skip=0;g_pipe_predict_on=1;
}
int main(void) {
    _Static_assert(sizeof(pkey)==1000,"persistent pipeline ABI changed");
    pkey current,k;init();pipeline_key(&current,&S,4,0,0);
    assert(current.key[0]==1&&current.key[1]==2&&current.topo==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST&&current.wmask[0]==15);
    S.daspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;S.ds.dt=1;S.ds.dw=1;S.ds.st=1;S.depth_clamp=1;
    shaders[0].local[0]=1;S.vattr[0]=0;pipeline_key(&k,&S,5,1,0);
    assert(k.dt&&k.dw&&k.st&&k.depth_clamp&&k.nattr==1&&k.attr[0].binding==16&&k.nbind==1&&k.bind[0].binding==16);
    shaders[0].local[0]=0;
    init();pipeline_key(&current,&S,4,0,0);vk_replay_state before=S;
    p_depth_clamp clamp={1};p_draw draw={.prim=4};append(OP_DEPTH_CLAMP,&clamp,sizeof clamp);append(OP_DRAW,&draw,20);
    clamp.on=0;append(OP_DEPTH_CLAMP,&clamp,sizeof clamp);draw.prim=5;append(OP_DRAW,&draw,20);append(OP_END,NULL,0);
    assert(pipeline_predict_rest(&current,2)==2&&queued==2&&keys[0].depth_clamp==1&&keys[1].topo==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    assert(!memcmp(&S,&before,sizeof S));
    init();append(OP_BARRIER,NULL,0);append(OP_DRAW,&draw,20);assert(!pipeline_predict_rest(&current,2)&&!queued);
    init();p_set_rt changed={0};append(OP_SET_RT,&changed,sizeof changed);append(OP_DRAW,&draw,20);assert(!pipeline_predict_rest(&current,2)&&!queued);
    init();p_set_rt same={.n=1,.color={&S.rt_col[0]}};append(OP_SET_RT,&same,sizeof same);append(OP_DRAW,&draw,20);append(OP_END,NULL,0);
    assert(pipeline_predict_rest(&current,2)==1&&keys[0].cfmt[0]==VK_FORMAT_R8G8B8A8_UNORM);
    init();p_bind_handles deferred={.stage=0,.n=1,.defer=1,.src=1};append(OP_BIND_TEX,&deferred,sizeof deferred);
    draw.prim=4;append(OP_DRAW,&draw,20);append(OP_END,NULL,0);current.um[0]=123;
    assert(pipeline_predict_rest(&current,2)==1&&keys[0].um[0]==0); // invalid deferred address is never read
    init();prog_t prog={.nstage=1};prog.sd[0].data=11;p_bind_program bind={&prog,1};
    append(OP_BIND_PROGRAM,&bind,sizeof bind);append(OP_SCISSOR,&(p_scissor){1,2,3,4},sizeof(p_scissor));append(OP_DRAW,&draw,20);append(OP_END,NULL,0);
    size_t child_len=end;memcpy(child,stream,child_len);end=0;const uint8_t *target=child;append(OP_CALL,&target,8);append(OP_END,NULL,0);before=S;
    assert(pipeline_predict_rest(&current,2)==1&&keys[0].key[0]==11&&keys[0].key[1]==2&&prefetched);assert(!memcmp(&S,&before,sizeof S));
    init();target=stream;append(OP_JUMP,&target,8);assert(!pipeline_predict_rest(&current,2));
    init();p_bind_vattr bad={.first=15,.n=2};append(OP_BIND_VATTR,&bad,sizeof bad);append(OP_DRAW,&draw,20);assert(!pipeline_predict_rest(&current,2));
    init();append(OP_DRAW,&draw,20);append(OP_END,NULL,0);g_pipe_predict_on=0;assert(!pipeline_predict_rest(&current,2));
    g_pipe_predict_on=1;g_predict_depth=17;assert(!pipeline_predict_rest(&current,2));
    puts("PASS private state exact keys bounded record lookahead");return 0;
}
#endif
