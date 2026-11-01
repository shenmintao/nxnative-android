
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"[RED] %d: %s\n",__LINE__,#c);exit(87);}}while(0)
#define VKLOG(...) do {if(0)fprintf(stderr,__VA_ARGS__);}while(0)
#define die(...) do {fprintf(stderr,__VA_ARGS__);exit(86);}while(0)
#define VKCHK(x) do{VkResult r_=(x);if(r_<0)die("[RED] driver error %d",r_);}while(0)
#define pthread_setname_np(t,n) ((void)(t),(void)(n),0)
void dm_perf_replay_thread(void){}
static PFN_vkBeginCommandBuffer real_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer real_vkEndCommandBuffer;
static PFN_vkAllocateCommandBuffers real_vkAllocateCommandBuffers;
static PFN_vkQueueSubmit real_vkQueueSubmit;
pthread_mutex_t g_vk_queue_mu=PTHREAD_MUTEX_INITIALIZER;
static const void *g_test_borrowed_info;

/* ENCODER IMPLEMENTATION */

#define MAIN_CBS 32
#define vkAllocateCommandBuffers vke_vkAllocateCommandBuffers
#define vkBeginCommandBuffer vke_vkBeginCommandBuffer
#define vkEndCommandBuffer vke_vkEndCommandBuffer
#define vkQueueSubmit vke_vkQueueSubmit
typedef struct {
 VkCommandPool pool;VkCommandBuffer main,up;int up_used;
 VkCommandBuffer mains[MAIN_CBS];int nmain_alloc,nmain_used,cb_err;
 VkCommandBuffer ups[MAIN_CBS];int nup_alloc,nup_used,nmain_sent;
 vk_deferred_part_state deferred_part;int deferred_pending;uint64_t cpu_first;
} vksub_t;
static vksub_t g_slot[1];static int g_cur;
static struct {VkQueue q;VkDevice dev;} V={(VkQueue)(uintptr_t)0x500,(VkDevice)(uintptr_t)0x600};
static VkQueryPool g_segment_q=(VkQueryPool)(uintptr_t)1;
static uint64_t g_cmd_gen,g_ns_submit;
static vksub_t *cur(void){CHECK(g_cur==0);return &g_slot[0];}
static int enabled,model[128],begin_worker[128],end_worker[128],fail_end;
static VkResult forced_begin,forced_submit;
static _Atomic int copied[128],registered[128],producer_returned;
static pthread_mutex_t gate_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static int gate_target,gate_open;
static int submits,last_submitted;
static unsigned cbid(VkCommandBuffer cb){unsigned id=(unsigned)(uintptr_t)cb;CHECK(id>0&&id<128);return id;}
static int owned_pointer(const void *p){
 for(unsigned i=0;i<VKE_BLOCKS;i++)if((uintptr_t)p>=(uintptr_t)g_vke_blocks[i].data&&(uintptr_t)p<(uintptr_t)g_vke_blocks[i].data+VKE_BYTES)return 1;
 return 0;
}
static VkResult VKAPI_CALL mock_begin(VkCommandBuffer cb,const VkCommandBufferBeginInfo *info){
 unsigned id=cbid(cb);
 CHECK(pthread_equal(pthread_self(),g_vke_thread)==begin_worker[id]);
 if(info){CHECK(info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);CHECK(info->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);}
 CHECK(!begin_worker[id]||owned_pointer(info));
 if(forced_begin)return forced_begin;
 CHECK(model[id]==0);model[id]=1;return VK_SUCCESS;
}
static VkResult VKAPI_CALL mock_end(VkCommandBuffer cb){
 unsigned id=cbid(cb);CHECK(pthread_equal(pthread_self(),g_vke_thread)==end_worker[id]);
 if((int)id==gate_target){pthread_mutex_lock(&gate_mu);while(!gate_open)pthread_cond_wait(&gate_cv,&gate_mu);pthread_mutex_unlock(&gate_mu);}
 CHECK(model[id]==1);if((int)id==fail_end){model[id]=4;return VK_ERROR_OUT_OF_HOST_MEMORY;}
 model[id]=2;return VK_SUCCESS;
}
static VkResult VKAPI_CALL mock_submit(VkQueue q,uint32_t n,const VkSubmitInfo *infos,VkFence fence){
 CHECK(q==V.q&&n==1&&fence==VK_NULL_HANDLE);
 CHECK(pthread_equal(pthread_self(),g_vke_thread)==enabled);
 CHECK(pthread_mutex_trylock(&g_vk_queue_mu)==EBUSY);
 const VkSubmitInfo *i=infos;CHECK(i->sType==VK_STRUCTURE_TYPE_SUBMIT_INFO&&!i->pNext&&!i->waitSemaphoreCount&&!i->signalSemaphoreCount);
 CHECK(i->commandBufferCount==1||i->commandBufferCount==2);
 unsigned main=cbid(i->pCommandBuffers[i->commandBufferCount-1]);CHECK(main<40&&main>(unsigned)last_submitted);
 if(i->commandBufferCount==2)CHECK(cbid(i->pCommandBuffers[0])>=40);
 for(unsigned k=0;k<i->commandBufferCount;k++){unsigned id=cbid(i->pCommandBuffers[k]);CHECK(model[id]==2&&atomic_load(&copied[id]));}
 if(forced_submit)return forced_submit;
 for(unsigned k=0;k<i->commandBufferCount;k++)model[cbid(i->pCommandBuffers[k])]=3;
 submits++;last_submitted=(int)main;return VK_SUCCESS;
}
static VkResult VKAPI_CALL mock_allocate(VkDevice dev,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *out){
 CHECK(dev==V.dev&&i->commandBufferCount==1);CHECK(!atomic_load(&g_vke_outstanding));
 vksub_t *s=cur();
 for(unsigned k=0;k<MAIN_CBS;k++){
  if(out==&s->mains[k]){*out=(VkCommandBuffer)(uintptr_t)(k+1);return VK_SUCCESS;}
  if(out==&s->ups[k]){*out=(VkCommandBuffer)(uintptr_t)(k+40);return VK_SUCCESS;}
 }
 CHECK(0&&"unexpected command-pool output");return VK_SUCCESS;
}
typedef struct{VkCommandBuffer cb;} marker;
static void run_marker(const void *payload){const marker *a=payload;CHECK(model[cbid(a->cb)]==1);}
static void record(VkCommandBuffer cb){
 atomic_store(&registered[cbid(cb)],1);atomic_store(&copied[cbid(cb)],0);
 marker *a=vke_alloc(sizeof(*a),run_marker);*a=(marker){cb};
}
static void segment_mark(vksub_t *s,int end){(void)s;(void)end;}
static void upload_segment_mark(vksub_t *s,int end){(void)s;(void)end;}
static void vk_img_barrier_all(VkCommandBuffer cb){marker *a=vke_alloc(sizeof(*a),run_marker);*a=(marker){cb};}
static uint64_t ucopy_now(void){return vke_now();}
static void ucopy_drain(void){for(unsigned i=1;i<128;i++)if(atomic_load(&registered[i]))atomic_store(&copied[i],1);}
static void vk_crumbs_report(void){}
static void *release_on_progress(void *unused){
 (void)unused;uint64_t deadline=vke_now()+2000000000ull;
 while(!atomic_load(&producer_returned)){CHECK(vke_now()<deadline);struct timespec t={0,1000000};nanosleep(&t,NULL);}
 pthread_mutex_lock(&gate_mu);gate_open=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate_mu);return NULL;
}

/* SPLIT IMPLEMENTATION */

static void fresh(void){
 vk_encode_drain();memset(model,0,sizeof model);memset(begin_worker,0,sizeof begin_worker);memset(end_worker,0,sizeof end_worker);
 for(unsigned i=0;i<128;i++){atomic_store(&registered[i],0);atomic_store(&copied[i],0);}
 memset(g_slot,0,sizeof g_slot);g_cur=0;submits=last_submitted=0;gate_target=gate_open=0;
 vksub_t *s=cur();s->pool=(VkCommandPool)(uintptr_t)0x700;s->nmain_alloc=s->nup_alloc=MAIN_CBS;
 for(unsigned i=0;i<MAIN_CBS;i++){s->mains[i]=(VkCommandBuffer)(uintptr_t)(i+1);s->ups[i]=(VkCommandBuffer)(uintptr_t)(i+40);}
 s->main=s->mains[0];s->nmain_used=1;s->up=s->ups[0];s->nup_used=1;
}
static void begin(VkCommandBuffer cb){
 unsigned id=cbid(cb);begin_worker[id]=enabled;VkCommandBufferBeginInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
 g_test_borrowed_info=&info;vk_encode_begin_checked(cb,&info);
}
int main(int argc,char **argv){
 CHECK(argc==3);enabled=atoi(argv[1]);const char *fatal=argv[2];
 real_vkBeginCommandBuffer=mock_begin;real_vkEndCommandBuffer=mock_end;real_vkQueueSubmit=mock_submit;real_vkAllocateCommandBuffers=mock_allocate;
 vk_encode_begin();CHECK(g_vke_owner);
 if(!strcmp(fatal,"begin")){fresh();forced_begin=VK_ERROR_OUT_OF_HOST_MEMORY;begin(cur()->main);vk_encode_drain();return 0;}
 for(int round=0;round<24;round++)for(int scenario=0;scenario<4;scenario++){
  fresh();vksub_t *s=cur();fail_end=scenario==1?1:scenario==2?3:scenario==3?40:0;
  begin(s->main);record(s->main);s->up_used=1;begin(s->up);record(s->up);
  if(!strcmp(fatal,"submit"))forced_submit=VK_ERROR_DEVICE_LOST;
  pthread_t releaser;int gated=enabled&&round==0&&scenario==0;
  if(gated){gate_target=1;atomic_store(&producer_returned,0);CHECK(!pthread_create(&releaser,NULL,release_on_progress,NULL));}
  for(int part=1;part<=12;part++){
   end_worker[cbid(s->main)]=enabled;end_worker[cbid(s->up)]=enabled;
   // The actual split begins the next main buffer itself.
   begin_worker[part+1]=enabled;
   vk_cmd_split();CHECK(vk_sub_sent_early()||scenario==1||scenario==3);
   if(gated&&part==1){atomic_store(&producer_returned,1);CHECK(!pthread_join(releaser,NULL));}
   record(s->main);
  }
  // The unchanged final driver end drains all prior jobs; collection then sees
  // actual successful sends and errors, never optimistic sent bookkeeping.
  end_worker[cbid(s->main)]=0;
  CHECK(vkEndCommandBuffer(s->main)==VK_SUCCESS);split_collect(s);
  CHECK(!s->deferred_pending);
  int expected=scenario==0?12:scenario==2?2:0;
  CHECK(s->nmain_sent==expected&&submits==expected&&last_submitted==expected);
  CHECK(!!s->cb_err==!!scenario);CHECK(!expected||s->cpu_first>0);
 }
 // Cold command-buffer allocation must drain before touching its pool.
 fresh();vksub_t *s=cur();s->nmain_alloc=s->nup_alloc=1;fail_end=0;
 begin(s->main);record(s->main);s->up_used=1;begin(s->up);record(s->up);
 for(int part=1;part<=4;part++){
  end_worker[cbid(s->main)]=enabled;end_worker[cbid(s->up)]=enabled;begin_worker[part+1]=enabled;
  vk_cmd_split();record(s->main);
 }
 end_worker[cbid(s->main)]=0;CHECK(vkEndCommandBuffer(s->main)==VK_SUCCESS);split_collect(s);
 CHECK(s->nmain_sent==4&&submits==4&&s->nmain_alloc==5&&s->nup_alloc==2);
 // Switching off early sends while jobs are pending must collect actual state
 // before the synchronous path, without submitting the disabled part early.
 fresh();s=cur();begin(s->main);record(s->main);end_worker[1]=enabled;begin_worker[2]=enabled;
 vk_cmd_split();record(s->main);CHECK(!putenv("DM_VK_SUBMIT_SPLIT=0"));
 end_worker[2]=0;begin_worker[3]=enabled;vk_cmd_split();
 CHECK(s->nmain_sent==1&&!s->deferred_pending&&submits==1&&vk_sub_sent_early());
 record(s->main);end_worker[3]=0;CHECK(vkEndCommandBuffer(s->main)==VK_SUCCESS);split_collect(s);
 CHECK(!putenv("DM_VK_SUBMIT_SPLIT=1"));
 // Ordinary Vulkan entry points still return their real errors synchronously.
 fresh();VkCommandBuffer cb=(VkCommandBuffer)(uintptr_t)96;
 VkCommandBufferBeginInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
 forced_begin=VK_ERROR_OUT_OF_DEVICE_MEMORY;CHECK(vkBeginCommandBuffer(cb,&info)==forced_begin);forced_begin=VK_SUCCESS;
 // A checked extended/inherited begin retains the caller path, without
 // dereferencing the ignored inheritance pointer or an unknown chain.
 info.pInheritanceInfo=(void *)(uintptr_t)1;vk_encode_begin_checked(cb,&info);end_worker[96]=0;CHECK(vkEndCommandBuffer(cb)==VK_SUCCESS);
 model[96]=0;info.pInheritanceInfo=NULL;info.pNext=(void *)(uintptr_t)1;vk_encode_begin_checked(cb,&info);CHECK(vkEndCommandBuffer(cb)==VK_SUCCESS);
 vk_encode_end();CHECK(!vk_encode_deferred_parts());model[96]=0;info.pNext=NULL;vk_encode_begin_checked(cb,&info);CHECK(vkEndCommandBuffer(cb)==VK_SUCCESS);
 if(enabled){CHECK(atomic_load(&g_vke_deferred_parts)>1000);CHECK(atomic_load(&g_vke_deferred_failed_ends)>0);CHECK(atomic_load(&g_vke_deferred_skips)>0);}
 else CHECK(!atomic_load(&g_vke_deferred_parts));
 vk_encode_report();pthread_mutex_lock(&g_vke_mu);g_vke_stop=1;pthread_cond_signal(&g_vke_work);pthread_mutex_unlock(&g_vke_mu);CHECK(!pthread_join(g_vke_thread,NULL));
 printf("actual split core/worker verified: enabled=%d, rounds=96, ordinary Vulkan PFNs preserved\n",enabled);return 0;
}
