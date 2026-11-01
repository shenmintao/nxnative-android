"""Diagnostic scopes are opt-in, balanced, lossless and absent from release."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'
STUB = '''#include <stdbool.h>
#include <stdint.h>
bool ATrace_isEnabled(void);
void ATrace_beginSection(const char *);
void ATrace_endSection(void);
void ATrace_setCounter(const char *, int64_t);
'''
SUPPORT = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sync_trace.h"
static int capture, queries, begins, ends, counters, depth;
static char names[8][128];
bool ATrace_isEnabled(void) { queries++; return capture; }
void ATrace_beginSection(const char *name) {
    assert(depth >= 0 && begins < 8); snprintf(names[begins],128,"%s",name); begins++; depth++;
}
void ATrace_endSection(void) { assert(depth>0); depth--; ends++; }
void ATrace_setCounter(const char *name, int64_t value) {
    assert(!strcmp(name,"counter") && value==4294967297ll); counters++;
}
'''


class SyncTrace(unittest.TestCase):
    def setUp(self):
        self.cc = shutil.which('cc') or shutil.which('clang')
        if not self.cc:
            self.skipTest('a host C compiler is required')

    def build(self, directory, code, flags=(), headers=None, assembly=False):
        (directory / 'android').mkdir(exist_ok=True)
        (directory / 'android/trace.h').write_text(STUB, encoding='utf8')
        source, output = directory / 'probe.c', directory / ('probe.s' if assembly else 'probe.exe')
        source.write_text(code, encoding='utf8')
        cmd = [self.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-I', str(directory), '-I', str(ANDROID), *flags]
        if headers:
            cmd += ['-idirafter', str(headers)]
        if assembly:
            cmd += ['-S']
        ran = subprocess.run([*cmd, str(source), '-o', str(output)], capture_output=True, text=True, timeout=30)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return output

    def test_scopes_follow_capture_state_and_balance_early_returns(self):
        code = SUPPORT + r'''
static void exercise(int early) {
    DM_SYNC_SCOPE("outer");
    { DM_SYNC_FENCE_SCOPE("fence",4294967297ull); if (early) return; }
    dm_sync_pair_point("pair",4294967297ull,18446744073709551615ull);
    dm_sync_counter("counter",4294967297ll);
}
static void stop_capture(void) { DM_SYNC_SCOPE("stop"); capture=0; }
int main(int argc,char **argv) {
    assert(argc==2); int enabled=atoi(argv[1]);
    exercise(0); assert(!begins && !ends && !counters);
    if (!enabled) assert(!queries);
    capture=1; exercise(1);
    if (!enabled) { assert(!begins && !queries); return 0; }
    assert(begins==2 && ends==2 && !depth);
    assert(!strcmp(names[1],"fence f=4294967297"));
    exercise(0);
    assert(begins==5 && ends==5 && counters==1 && !depth);
    assert(!strcmp(names[4],"pair a=4294967297 b=18446744073709551615"));
    stop_capture(); assert(begins==6 && ends==6 && !depth);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='sync-trace-') as temp:
            exe = self.build(Path(temp), code, ['-DDM_SYNC_TRACE_TEST=1'])
            for flag in (None, '0', '1', '10', 'true'):
                env = dict(os.environ)
                env.pop('DM_TRACE_SYNC', None)
                if flag is not None:
                    env['DM_TRACE_SYNC'] = flag
                ran = subprocess.run([str(exe), str(int(flag == '1'))], env=env, capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, repr(flag) + ran.stdout + ran.stderr)

    def test_release_has_identical_assembly_and_does_not_evaluate_arguments(self):
        baseline = 'int sample(int x) { return x*7+1; }\n'
        observed = '''#include "sync_trace.h"
int sample(int x) {
    DM_SYNC_SCOPE((++x,"scope")); DM_SYNC_FENCE_SCOPE("fence",++x);
    dm_sync_pair_point("pair",++x,++x); dm_sync_counter("counter",++x);
    dm_sync_vulkan_install(); return x*7+1;
}
'''
        with tempfile.TemporaryDirectory(prefix='sync-trace-release-') as temp:
            directory = Path(temp)
            expected = self.build(directory, baseline, assembly=True).read_text()
            actual = self.build(directory, observed, ['-DDM_RELEASE=1', '-DDM_SYNC_TRACE_TEST=1'], assembly=True).read_text()
            self.assertEqual(actual, expected)

    def test_driver_hooks_keep_arguments_results_and_prior_wrapper(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ANDROID / 'gfx_vk_trace.c').read_text(encoding='utf8')
        functions = re.findall(r'X\((vk\w+),', source)
        code = '#define VK_NO_PROTOTYPES\n#include <vulkan/vulkan.h>\n' + SUPPORT
        code += '\n'.join('PFN_' + name + ' ' + name + ';' for name in functions) + '\n'
        code += source.replace('#include "gfx_vk.h"', '')
        code += r'''
static int calls;
static VkResult VKAPI_CALL prior(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *o) {
    assert(d==(VkDevice)12 && i->size==4096 && !a); *o=(VkBuffer)42; calls++; return VK_INCOMPLETE;
}
int main(int argc,char **argv) {
    assert(argc==2); int enabled=atoi(argv[1]); capture=1; vkCreateBuffer=prior;
    dm_sync_vulkan_install(); dm_sync_vulkan_install();
    assert((vkCreateBuffer!=prior)==enabled && !vkCreateImage);
    VkBufferCreateInfo info={0}; info.size=4096; VkBuffer result=0;
    assert(vkCreateBuffer((VkDevice)12,&info,NULL,&result)==VK_INCOMPLETE);
    assert(result==(VkBuffer)42 && calls==1 && begins==enabled && ends==enabled && !depth);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='sync-trace-driver-') as temp:
            exe = self.build(Path(temp), code, ['-DDM_SYNC_TRACE_TEST=1'], headers=headers)
            for flag in ('0', '1'):
                ran = subprocess.run([str(exe), flag], env=dict(os.environ, DM_TRACE_SYNC=flag), capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
