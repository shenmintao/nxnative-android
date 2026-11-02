"""Upstream fusion plus the Android transport and real HID return path."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'nxnative/dm/host'
ANDROID = ROOT / 'nxnative/dm/android'


class Motion(unittest.TestCase):
    def run_c(self, code):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='nx2a-motion-') as temp:
            c, exe = Path(temp) / 'check.c', Path(temp) / 'check'
            c.write_text(code, encoding='utf8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     '-I', str(HOST), '-I', str(ANDROID), str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = dict(os.environ, DM_MOTION='1', DM_PHONE_MOTION='auto', DM_MOTION_GAIN='0', DM_MOTION_LOG='1', DM_MOTION_AXES='x,y,z')
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_upstream_fusion(self):
        code = (HOST / 'tests/t_motion.c').read_text(encoding='utf8').replace('#include "../motion_fusion.h"', '#include "motion_fusion.h"')
        self.run_c(code)

    def test_android_rotations_units_and_suspend_clock(self):
        self.run_c(r'''
#include "motion_android.h"
#include "motion_fusion.h"
#include <assert.h>
static int near(float a,float b) { return fabsf(a-b)<1e-5f; }
int main(void) {
    const float flat[3]={0,0,9.80665f};
    // The same positive turn about the picture's right edge in all four display rotations.
    const float pitch[4][3]={{1,0,0},{0,-1,0},{-1,0,0},{0,1,0}};
    for(int r=0;r<4;r++) {
        float a[3],w[3]; assert(motion_android_vectors(r,flat,pitch[r],a,w));
        assert(near(a[0],0) && near(a[1],0) && near(a[2],-1));
        assert(near(w[0],1) && near(w[1],0) && near(w[2],0));
        motion_state s; motion_reset(&s); motion_map map; motion_map_parse(&map,NULL);
        motion_update(&s,&map,a,w,0,0);
        for(int i=0;i<100;i++)motion_update(&s,&map,a,w,.01f,0);
        assert(near(s.angle[0],1/(2*(float)M_PI)) && near(s.gyro[0],1/(2*(float)M_PI)));
    }
    float a[3],w[3],bad[3]={NAN,0,0}; assert(!motion_android_vectors(0,flat,bad,a,w));
    assert(motion_android_time(5000000000ull,100000000000ull,99995000000ll)==4995000000ull);
    assert(!motion_android_time(5,100,0)); assert(!motion_android_time(5,100,90));
    assert(motion_android_time(5,100,101)==5); // tiny future skew is clamped to arrival
    return 0;
}
''')

    def test_live_phone_samples_reach_hid_and_expire(self):
        source = (HOST / 'hle_hid.c').read_text(encoding='utf8')
        start = source.index('typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9];')
        end = source.index('HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandles', start)
        self.run_c(r'''
#include "motion_fusion.h"
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#define LOG(...) do { if(0)printf(__VA_ARGS__); } while(0)
#define DM_PLAYERS 8
#define SHAKE_HZ 8.0
static uint64_t clock_ns=10000000000ull;
static _Atomic uint64_t g_shake_t0,g_shake_end;
static uint64_t now_ns(void) { return clock_ns; }
static uint64_t guest_ns(void) { return clock_ns; }
static uint32_t merged_players(void) { return 0; }
static unsigned style_of(unsigned id) { return id==0 || id==0x20 ? 1 : 0; }
static int player_of(unsigned id) { return id==0x20 ? 0 : id<8 ? (int)id : -1; }
''' + source[start:end] + r'''
int main(void) {
    SixAxisState out[16]; const float a[3]={0,0,-1},w[3]={(float)M_PI/2,0,0};
    assert(six_states(out,16,0)==16 && out[0].acc[2]==-1 && out[0].gyro[0]==0);
    dm_motion_set_switch_at(0,a,w,clock_ns);
    for(int i=0;i<100;i++) { clock_ns+=10000000; dm_motion_set_switch_at(0,a,w,clock_ns); }
    assert(six_states(out,16,0)==16 && atomic_load(&g_motion_reads[0])==1);
    assert(fabsf(out[0].gyro[0]-.25f)<1e-5f && fabsf(out[0].angle[0]-.25f)<1e-5f);
    assert(fabsf(out[0].dir[5]-1)<1e-4f && fabsf(out[0].dir[7]+1)<1e-4f);
    for(int i=0;i<16;i++) assert(out[i].attributes==1 && out[i].delta_ns==5000000 && out[i].sampling==out[0].sampling-i);
    dm_motion_clear_switch(); assert(six_states(out,1,0)==1 && out[0].gyro[0]==0 && out[0].dir[0]==1);
    dm_motion_set_switch_at(0,a,w,clock_ns-400000000); assert(six_states(out,1,0)==1 && out[0].gyro[0]==0);
    dm_motion_set_switch_at(0,a,w,clock_ns+1); assert(six_states(out,1,0)==1 && out[0].gyro[0]==0);
    dm_motion_set_switch_at(0,a,w,clock_ns); clock_ns+=300000001;
    assert(six_states(out,1,0)==1 && out[0].gyro[0]==0);
    return 0;
}
''')
