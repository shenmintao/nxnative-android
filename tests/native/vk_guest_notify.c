#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed %d\n",__LINE__); exit(86); } } while(0)
typedef uintptr_t VkCommandBuffer;
typedef struct { uint64_t fence;int nmain_alloc,nup_alloc;VkCommandBuffer mains[8],ups[8]; } vksub_t;
typedef struct { uint64_t fence;int rslot; } cpend;
static vksub_t g_slot[3];static int g_prereset=1;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static int reset_started,allow_reset,resets;
static _Atomic int post_result;
uint64_t vk_fence_done(void);
static int vkResetCommandBuffer(VkCommandBuffer cb,int flags) {
 CHECK(cb && flags==0);
 pthread_mutex_lock(&gate);reset_started=1;pthread_cond_broadcast(&gate_cv);
 while(!allow_reset)pthread_cond_wait(&gate_cv,&gate);
 resets++;pthread_mutex_unlock(&gate);return 0;
}

/* PRODUCTION HELPERS */

static void *complete(void *arg) {
 cpend *c=arg;
 atomic_store_explicit(&post_result,(int)c->fence,memory_order_relaxed);
 completion_notify_guest(c->fence);
 completion_retire(c);return 0;
}
static void *visible_waiter(void *arg) {
 uint64_t f=*(uint64_t*)arg;CHECK(vk_guest_wait(f,~0ull));
 CHECK(atomic_load_explicit(&post_result,memory_order_relaxed)==(int)f);return 0;
}
static void *retire_waiter(void *arg) {
 uint64_t f=*(uint64_t*)arg;CHECK(mtl_wait(f,~0ull));CHECK(vk_fence_done()>=f);return 0;
}
int main(int argc,char **argv) {
 CHECK(argc==2);int enabled=atoi(argv[1]);CHECK(guest_notify_enabled()==enabled);
 CHECK(vk_guest_wait(0,0) && mtl_wait(0,0));
 CHECK(!vk_guest_wait(1,0) && !mtl_wait(1,0));
 for(uint64_t f=1;f<=100;f++) {
  int slot=(int)(f%3);vksub_t *s=&g_slot[slot];s->fence=f;s->nmain_alloc=1+(int)(f%5);s->nup_alloc=1+(int)(f%3);
  for(int i=0;i<8;i++){s->mains[i]=(uintptr_t)(1+i);s->ups[i]=(uintptr_t)(10+i);}
  pthread_mutex_lock(&gate);reset_started=allow_reset=resets=0;pthread_mutex_unlock(&gate);
  cpend c={f,slot};pthread_t cthread,vthread,rthread;
  CHECK(!pthread_create(&cthread,0,complete,&c));
  pthread_mutex_lock(&gate);while(!reset_started)pthread_cond_wait(&gate_cv,&gate);pthread_mutex_unlock(&gate);
  // GPU/CPU effects are available, but reset is deterministically held open.
  CHECK(vk_guest_wait(f,0)==enabled);CHECK(!mtl_wait(f,0));CHECK(vk_fence_done()==f-1);
  CHECK(!vk_guest_wait(f+1,1000));CHECK(!mtl_wait(f+1,1000));
  CHECK(atomic_load_explicit(&post_result,memory_order_relaxed)==(int)f);
  CHECK(!pthread_create(&vthread,0,visible_waiter,&c.fence));
  CHECK(!pthread_create(&rthread,0,retire_waiter,&c.fence));
  pthread_mutex_lock(&gate);allow_reset=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate);
  CHECK(!pthread_join(cthread,0) && !pthread_join(vthread,0) && !pthread_join(rthread,0));
  CHECK(resets==s->nmain_alloc+s->nup_alloc);CHECK(vk_guest_wait(f,0) && mtl_wait(f,0));
  CHECK(vk_fence_done()==f);
 }
 // Slot mismatch, no slot, and disabled pre-reset never touch another allocation.
 allow_reset=1;
 for(uint64_t f=101;f<=2100;f++) {
  int slot=(int)(f%3);cpend c={f,slot};int was=resets;
  g_slot[slot].fence=f-1;completion_notify_guest(f);completion_retire(&c);
  CHECK(resets==was && mtl_wait(f,0) && vk_guest_wait(f,0));
  CHECK(vk_fence_done()==f);
 }
 cpend c={2101,-1};completion_notify_guest(c.fence);completion_retire(&c);CHECK(mtl_wait(c.fence,0));
 g_prereset=0;c=(cpend){2102,0};g_slot[0].fence=c.fence;int was=resets;
 completion_notify_guest(c.fence);completion_retire(&c);CHECK(resets==was && mtl_wait(c.fence,0));
 CHECK(!vk_guest_wait(c.fence+1,0));puts("dual completion boundaries verified");return 0;
}
