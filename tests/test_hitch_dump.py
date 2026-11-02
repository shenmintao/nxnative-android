"""The diagnostic must resume a thread even when register/stack reads fail."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_host_compat import function


class HitchDump(unittest.TestCase):
    def test_snapshot_resumes_before_logging_and_survives_unreadable_frames(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/host/hle_thread.c').read_text(encoding='utf-8')
        types = source[source.index('typedef struct {   // what one dump line needs'):source.index('// Holding G (thr_free')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uintptr_t vm_size_t, vm_address_t;
typedef unsigned mach_msg_type_number_t;
typedef int *thread_info_t, *thread_state_t;
typedef struct { int run_state; } thread_basic_info_data_t;
typedef struct { uint64_t __x[29], pc, fp, lr; } arm_thread_state64_t;
#define KERN_SUCCESS 0
#define THREAD_BASIC_INFO 1
#define THREAD_BASIC_INFO_COUNT 1
#define ARM_THREAD_STATE64 2
#define ARM_THREAD_STATE64_COUNT 1
#define __darwin_arm_thread_state64_get_pc(s) ((s).pc)
#define __darwin_arm_thread_state64_get_fp(s) ((s).fp)
#define __darwin_arm_thread_state64_get_lr(s) ((s).lr)
typedef struct { int present; } module_t;
static module_t mod;
typedef struct {
    uint32_t handle, last_svc; int prio, core, wkind, sleeping, mport;
    uint64_t mask, waddr, wkey, last_lr; int *host_work;
} thr_t;
static int parked, resumes, suspend_error, register_error, readable;
static int checked_snprintf(char *dst, size_t size, const char *fmt, ...) {
    assert(!parked); va_list a; va_start(a, fmt); int n=vsnprintf(dst,size,fmt,a); va_end(a); return n;
}
#define snprintf checked_snprintf
static const char *thr_name(thr_t *t, char *b, size_t n) { (void)t; snprintf(b,n,"worker"); return b; }
static module_t *module_of(void *p) { return (uintptr_t)p >= 0x100000000ull ? &mod : NULL; }
static int thread_info(int t, int flavor, thread_info_t out, mach_msg_type_number_t *n) {
    assert(t==7 && flavor==THREAD_BASIC_INFO && *n==THREAD_BASIC_INFO_COUNT && !parked);
    ((thread_basic_info_data_t *)out)->run_state=3; return KERN_SUCCESS;
}
static int thread_suspend(int t) { assert(t==7 && !parked); if(suspend_error)return 1; parked=1; return KERN_SUCCESS; }
static int thread_resume(int t) { assert(t==7 && parked); parked=0; resumes++; return KERN_SUCCESS; }
static int thread_get_state(int t, int flavor, thread_state_t out, mach_msg_type_number_t *n) {
    assert(t==7 && parked && flavor==ARM_THREAD_STATE64 && *n==ARM_THREAD_STATE64_COUNT);
    if(register_error)return 1;
    arm_thread_state64_t *s=(void *)out; memset(s,0,sizeof *s); s->pc=0x100012340ull; s->lr=0x100056780ull; s->fp=0x1000;
    for(int i=0;i<29;i++)s->__x[i]=(unsigned)i+10;
    return KERN_SUCCESS;
}
static int mach_task_self(void) { return 1; }
static int vm_read_overwrite(int task, vm_address_t src, vm_size_t n, vm_address_t dst, vm_size_t *got) {
    assert(parked && task==1 && n==16); if(!readable){*got=0;return 1;} readable--;
    uint64_t *p=(void *)dst; p[0]=src+16; p[1]=0x100010000ull+src; *got=n; return KERN_SUCCESS;
}
''' + types + function(source, 'static void hitch_grab(') + r'''
int main(void) {
    int host=1; thr_t t={.handle=0x1010,.last_svc=0x18,.prio=44,.core=2,.wkind=4,.sleeping=1,
        .mport=7,.mask=6,.waddr=0x120,.wkey=0x340,.last_lr=0x1000,.host_work=&host}; hitch_t h;
    readable=3; hitch_grab(&t,&h);
    assert(!parked && resumes==1 && h.have && h.npc==5 && h.guest && h.rs==3);
    assert(h.x[0]==10 && h.x[2]==12 && h.x[3]==29 && h.x[9]==35);
    assert(h.handle==t.handle && h.waddr==t.waddr && h.wkey==t.wkey && h.sleeping && h.host_work);
    register_error=1; hitch_grab(&t,&h); assert(!parked && resumes==2 && !h.have && !h.npc);
    register_error=0; readable=0; hitch_grab(&t,&h); assert(!parked && resumes==3 && h.have && h.npc==2);
    suspend_error=1; hitch_grab(&t,&h); assert(!parked && resumes==3 && !h.have);
    t.mport=0; hitch_grab(&t,&h); assert(!parked && resumes==3 && !h.have && h.rs==-1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-hitch-') as temp:
            c, exe = Path(temp) / 'check.c', Path(temp) / 'check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
