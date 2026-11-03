"""The upstream statistics mailbox never blocks the audio callback on logging."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def function(src, sig):
    i=src.index(sig)
    return src[i:src.index('\n}',i)+3]


class AudioStats(unittest.TestCase):
    def test_full_mailbox_and_blocked_logger(self):
        cc=shutil.which('cc') or shutil.which('clang')
        if not cc:self.skipTest('C compiler required')
        src=(ROOT/'nxnative/dm/host/hle_audio.c').read_text(encoding='utf8')
        typedef=next(l for l in src.splitlines() if l.startswith('typedef struct { uint64_t calls, count,'))
        funcs='\n'.join(function(src,s) for s in ('static void stats_publish(', 'static int stats_take(', 'static void stats_log(', 'static void stats_drain('))
        code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
/* STATS */
typedef struct {
 uint64_t st_calls,st_count,st_qsum,st_u0,st_ub0;uint32_t st_fmin,st_fmax;
 _Atomic unsigned q_tail,q_head;_Atomic uint64_t under_n,under_b;
 audio_stats st_report;_Atomic int st_pending;int id,ch;
}aout;
enum {AOMAX=8,RATE=48000};
typedef struct {audio_stats stats;int id,ch;}audio_report;
static pthread_mutex_t g_aos_lock=PTHREAD_MUTEX_INITIALIZER,logger=PTHREAD_MUTEX_INITIALIZER;
static aout*g_aos[AOMAX];static _Atomic int entered;static uint64_t logged;
static void log_snapshot(uint64_t count){
 assert(!pthread_mutex_trylock(&g_aos_lock));pthread_mutex_unlock(&g_aos_lock);
 atomic_store(&entered,1);pthread_mutex_lock(&logger);logged+=count;pthread_mutex_unlock(&logger);
}
#define LOG(...) log_snapshot(s->count)
/* FUNCTIONS */
static void*drain(void*p){(void)p;stats_drain();return NULL;}
int main(void){
 aout a={.id=1,.ch=2};g_aos[0]=&a;for(int i=0;i<2000;i++)stats_publish(&a,240);
 pthread_mutex_lock(&logger);pthread_t t;assert(!pthread_create(&t,NULL,drain,NULL));
 while(!atomic_load(&entered)){struct timespec d={0,1000000};nanosleep(&d,NULL);}
 // The logger is deliberately blocked, but 4000 more callbacks return and the
 // full mailbox is not overwritten. Accumulated counts survive the next drain.
 for(int i=0;i<4000;i++)stats_publish(&a,240);
 assert(a.st_calls==6000&&a.st_report.calls==4000&&a.st_report.count==2000&&a.st_count==2000);
 pthread_mutex_unlock(&logger);pthread_join(t,NULL);stats_drain();
 for(int i=0;i<2000;i++)stats_publish(&a,240);stats_drain();
 assert(logged==8000);puts("PASS");return 0;
}
'''
        code=code.replace('/* STATS */',typedef).replace('/* FUNCTIONS */',funcs)
        with tempfile.TemporaryDirectory(prefix='audio-stats-') as temp:
            d=Path(temp);c=d/'stats.c';exe=d/'stats.exe';c.write_text(code,encoding='utf8')
            r=subprocess.run([cc,'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(c),'-pthread','-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('PASS',r.stdout)
