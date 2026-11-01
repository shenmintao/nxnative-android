"""NVN diagnostics honor front-end configuration loaded after constructors."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^static (?:void|void \*) ?' + name + r'\([^\n]*\) \{', source, re.MULTILINE)
    start = match.start()
    return source[start:source.index('\n}', start) + 3]


class NvnRuntimeConfig(unittest.TestCase):
    def test_late_environment_concurrent_resolution_and_wrapper_ownership(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        source = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NVN_NFUNC 6
static struct { void *fn; } nvn_slot[NVN_NFUNC];
static void *nvn_real[NVN_NFUNC], *nvn_thunk[NVN_NFUNC];
static const char *nvn_names[NVN_NFUNC]={"unimplemented", "nvnQueuePresentTexture", "semantic", "empty", "big-stack", "other"};
static int g_log,g_nvn_prof,configurations;
static const char *option;
static _Atomic unsigned slots_ready;
static void nvn_unimpl_entry(void) {}
static void nvn_prof_entry(void) {}
static void semantic(void) {}
static void empty(void) {}
static void big_stack(void) {}
static void present(void) {}
static void nvn_init_slots(void) { atomic_fetch_add(&slots_ready,1); }
static const char *mock_getenv(const char *name) {
    if (!strcmp(name,"DM_NVN_LOG")) return "1";
    assert(!strcmp(name,"DM_NVN_PROF")); configurations++; return option;
}
#define getenv mock_getenv
#define LOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
''' + function(source, 'nvn_runtime_init') + '\n' + function(source, 'nvn_lookup') + r'''
static int enabled;
static void *resolve(void *unused) {
    (void)unused;
    for(unsigned n=0;n<500;n++) for(unsigned i=0;i<NVN_NFUNC;i++) {
        assert(nvn_lookup(nvn_names[i])==nvn_thunk[i]);
        assert(g_log && g_nvn_prof==enabled);
        if(enabled && i>=2) { assert(nvn_slot[i].fn==(void *)nvn_prof_entry); assert(nvn_real[i]!=(void *)nvn_prof_entry); }
    }
    assert(!nvn_lookup("missing")); return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    // Constructor registration is complete while the environment is still unset.
    void *original[NVN_NFUNC]={(void *)nvn_unimpl_entry,(void *)present,(void *)semantic,(void *)empty,(void *)big_stack,(void *)semantic};
    for(unsigned i=0;i<NVN_NFUNC;i++) { nvn_slot[i].fn=original[i]; nvn_thunk[i]=(void *)(uintptr_t)(i+1); }
    assert(!configurations && !g_nvn_prof);
    option=!strcmp(argv[1],"unset") ? NULL : argv[1]; enabled=option && atoi(option)!=0;
    pthread_t threads[12];
    for(unsigned i=0;i<12;i++) assert(!pthread_create(&threads[i],NULL,resolve,NULL));
    for(unsigned i=0;i<12;i++) assert(!pthread_join(threads[i],NULL));
    assert(configurations==1 && slots_ready==12*(500*NVN_NFUNC+1));
    for(unsigned i=0;i<NVN_NFUNC;i++) {
        int wrapped=enabled && i>=2;
        assert(nvn_slot[i].fn==(wrapped ? (void *)nvn_prof_entry : original[i]));
        assert(nvn_real[i]==(wrapped ? original[i] : NULL));
    }
    // Summary enablement must not depend on the first two slots being implemented.
    assert(!nvn_real[0] && !nvn_real[1] && g_nvn_prof==enabled);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nvn-runtime-') as temp:
            path, exe = Path(temp) / 'runtime.c', Path(temp) / 'runtime'
            path.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for option in ('unset', '0', '1'):
                result = subprocess.run([str(exe), option], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, option + ': ' + result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
