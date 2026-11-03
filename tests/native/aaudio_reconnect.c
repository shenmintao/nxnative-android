#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <aaudio/AAudio.h>

struct AAudioStreamBuilder { AAudioStream_dataCallback data; AAudioStream_errorCallback error; void *ud,*ed; int rate,channels,format,frames; };
struct AAudioStream { struct AAudioStreamBuilder config; _Atomic int stop,fault,state; int thread_started; pthread_t thread; };
static _Thread_local int in_callback;
static _Atomic int opens,closes,live,fail_opens,fail_early,attempts,host_callbacks;
static uint64_t attempt_time[128];
static uint64_t clock_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void delay_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,NULL);}
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b){assert(!in_callback);*b=calloc(1,sizeof **b);return 0;}
#define SETTER(name,field) void AAudioStreamBuilder_set##name(AAudioStreamBuilder*b,int32_t v){assert(!in_callback);b->field=v;}
SETTER(SampleRate,rate) SETTER(ChannelCount,channels) SETTER(Format,format) SETTER(FramesPerDataCallback,frames)
#define IGNORE(name) void AAudioStreamBuilder_set##name(AAudioStreamBuilder*b,int32_t v){(void)b;(void)v;assert(!in_callback);}
IGNORE(Direction) IGNORE(PerformanceMode) IGNORE(SharingMode) IGNORE(Usage) IGNORE(ContentType)
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*b,AAudioStream_dataCallback f,void*u){b->data=f;b->ud=u;}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*b,AAudioStream_errorCallback f,void*u){b->error=f;b->ed=u;}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*b,AAudioStream**s){
 assert(!in_callback);int n=atomic_fetch_add(&attempts,1);assert(n<128);attempt_time[n]=clock_ns();
 if(atomic_load(&fail_opens)>0){atomic_fetch_sub(&fail_opens,1);return AAUDIO_ERROR_DISCONNECTED;}
 assert(b->rate==48000&&b->channels==2&&b->format==AAUDIO_FORMAT_PCM_I16&&b->frames==240);
 *s=calloc(1,sizeof **s);(*s)->config=*b;atomic_fetch_add(&opens,1);atomic_fetch_add(&live,1);return 0;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*b){assert(!in_callback);free(b);return 0;}
static void*data_thread(void*p){
 AAudioStream*s=p;short pcm[480];
 while(!atomic_load(&s->stop)){
  int fault=atomic_exchange(&s->fault,0);
  if(fault){atomic_store(&s->state,AAUDIO_STREAM_STATE_DISCONNECTED);if(fault==1){in_callback=1;s->config.error(s,s->config.ed,AAUDIO_ERROR_DISCONNECTED);in_callback=0;}break;}
  in_callback=1;int result=s->config.data(s,s->config.ud,pcm,240);in_callback=0;
  if(result==AAUDIO_CALLBACK_RESULT_STOP){atomic_store(&s->state,AAUDIO_STREAM_STATE_STOPPED);break;}
  delay_ms(5);
 }
 return NULL;
}
aaudio_result_t AAudioStream_requestStart(AAudioStream*s){
 assert(!in_callback);atomic_store(&s->state,AAUDIO_STREAM_STATE_STARTED);
 if(atomic_load(&fail_early)>0){atomic_fetch_sub(&fail_early,1);atomic_store(&s->fault,1);}
 s->thread_started=1;return pthread_create(&s->thread,NULL,data_thread,s);
}
aaudio_result_t AAudioStream_requestStop(AAudioStream*s){assert(!in_callback);atomic_store(&s->stop,1);atomic_store(&s->state,AAUDIO_STREAM_STATE_STOPPED);return 0;}
aaudio_result_t AAudioStream_close(AAudioStream*s){
 assert(!in_callback);atomic_store(&s->stop,1);if(s->thread_started)pthread_join(s->thread,NULL);
 atomic_fetch_add(&closes,1);atomic_fetch_sub(&live,1);free(s);return 0;
}
aaudio_stream_state_t AAudioStream_getState(AAudioStream*s){assert(!in_callback);return atomic_load(&s->state);}
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream*s,aaudio_stream_state_t old,aaudio_stream_state_t*out,int64_t timeout){
 (void)old;(void)timeout;assert(!in_callback);*out=AAudioStream_getState(s);return 0;
}
#include "../../nxnative/dm/android/compat/audiotoolbox_aaudio.c"

static OSStatus render(void*ref,AudioUnitRenderActionFlags*flags,const AudioTimeStamp*ts,UInt32 bus,UInt32 frames,AudioBufferList*b){
 (void)ref;(void)flags;(void)ts;(void)bus;(void)frames;memset(b->mBuffers[0].mData,0,b->mBuffers[0].mDataByteSize);atomic_fetch_add(&host_callbacks,1);return 0;
}
static void recovered(AudioUnit u,uint64_t n){
 uint64_t deadline=clock_ns()+3000000000ull;while(atomic_load(&u->reconnects)<n&&clock_ns()<deadline)delay_ms(1);
 assert(atomic_load(&u->reconnects)==n);
}
static void fault(AudioUnit u,int notify){
 pthread_mutex_lock(&u->control);assert(u->st);atomic_store(&u->st->fault,notify?1:2);pthread_mutex_unlock(&u->control);
}
int main(void){
 AudioComponentDescription desc={.componentType=kAudioUnitType_Output};AudioUnit u;
 assert(!AudioComponentInstanceNew(AudioComponentFindNext(NULL,&desc),&u));
 AudioStreamBasicDescription fmt={.mSampleRate=48000,.mFormatID=kAudioFormatLinearPCM,.mBitsPerChannel=16,.mChannelsPerFrame=2,.mBytesPerFrame=4};
 AURenderCallbackStruct cb={render,NULL};
 assert(!AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,0,0,&fmt,sizeof fmt));
 assert(!AudioUnitSetProperty(u,kAudioUnitProperty_SetRenderCallback,0,0,&cb,sizeof cb));assert(!AudioUnitInitialize(u));assert(!AudioOutputUnitStart(u));
 delay_ms(20);assert(atomic_load(&host_callbacks)>0);
 // A normal hold keeps the original mixer track and cannot enter host render.
 int prior=atomic_load(&opens);assert(!AudioOutputUnitStop(u));int held=atomic_load(&host_callbacks);delay_ms(20);assert(atomic_load(&host_callbacks)==held);
 assert(!AudioOutputUnitStart(u)&&atomic_load(&opens)==prior);
 for(uint64_t n=1;n<=3;n++){fault(u,1);recovered(u,n);}
 fault(u,0);recovered(u,4); // dead/disconnected state without an error callback
 int first=atomic_load(&attempts);atomic_store(&fail_opens,2);fault(u,1);recovered(u,5);
 assert(atomic_load(&attempts)>=first+3);
 assert(attempt_time[first+1]-attempt_time[first]>=24000000ull);
 assert(attempt_time[first+2]-attempt_time[first+1]>=49000000ull);
 atomic_store(&fail_early,2);fault(u,1);recovered(u,6); // repeated disconnects before first data also back off
 // Long actual callback spacing must retain RED even if the reopen succeeds.
 pthread_mutex_lock(&u->control);atomic_store(&u->st->stop,1);pthread_join(u->st->thread,NULL);u->st->thread_started=0;
 atomic_store(&u->last_callback,clock_ns()-150000000ull);atomic_store(&u->st->state,AAUDIO_STREAM_STATE_DISCONNECTED);
 in_callback=1;on_error(u->st,u,AAUDIO_ERROR_DISCONNECTED);in_callback=0;pthread_mutex_unlock(&u->control);recovered(u,7);
 // Cancel a pending retry during disposal; join before freeing userdata.
 atomic_store(&fail_opens,20);fault(u,1);delay_ms(12);assert(!AudioComponentInstanceDispose(u));
 assert(!atomic_load(&live)&&atomic_load(&opens)==atomic_load(&closes));puts("PASS");return 0;
}
