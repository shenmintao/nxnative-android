"""Production driver encoding preserves snapshots, FIFO, ownership and drains."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


class VulkanEncodingWorker(unittest.TestCase):
    def test_command_coverage_and_owned_argument_graphs(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_encode.c').read_text(encoding='utf8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf8')
        calls = set(re.findall(r'X\((vkCmd\w+)\)', header))
        hooks = set(re.findall(r'X\((vk\w+)\)', source.split('#define VKE_REAL')[0]))
        self.assertLessEqual(calls, hooks, 'a command escaping the FIFO could reorder driver calls')
        preamble = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define pthread_setname_np(...) ((void)0)
#define VKLOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
static _Noreturn void die(const char *message, ...) { va_list args; va_start(args,message); vfprintf(stderr,message,args); va_end(args); abort(); }
static _Atomic unsigned placement_calls;
static void dm_perf_replay_thread(void) {
    const char *e = getenv("DM_PIN_REPLAY");
    if (e && !strcmp(e,"prime")) atomic_fetch_add(&placement_calls,1);
}
static void env_set(const char *name,const char *value) {
#ifdef _WIN32
    assert(!_putenv_s(name,value));
#else
    assert(!setenv(name,value,1));
#endif
}
'''
        start = header.index('typedef struct {\n    int end_error')
        preamble += header[start:header.index('int vk_encode_deferred_parts(void);', start)]
        preamble += '\npthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;\n'
        preamble += '\n'.join(f'PFN_{name} {name};' for name in sorted(hooks)) + '\n'
        code = preamble + source.replace('#include "gfx_vk.h"', '')
        code += (ROOT / 'tests/native/vk_encode.c').read_text(encoding='utf8')
        with tempfile.TemporaryDirectory(prefix='vk-encode-') as temp:
            path, exe = Path(temp) / 'encode.c', Path(temp) / 'encode'
            path.write_text(code, encoding='utf8')
            built = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                    '-idirafter', str(headers), str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            # A deliberate borrowed vertex-array pointer must be detected while
            # the worker is blocked and the producer poisons its original data.
            good = 'a->buffers = vke_pack(&tail, buffers, count, sizeof(VkBuffer));'
            start = code.index('static void VKAPI_CALL vke_vkCmdBindVertexBuffers(')
            end = code.index('\n}', start)
            self.assertIn(good, code[start:end])
            damaged = code[:start] + code[start:end].replace(good, 'a->buffers = buffers;') + code[end:]
            path.write_text(damaged, encoding='utf8')
            built = subprocess.run([cc, '-std=c11', '-O2', '-pthread', '-idirafter', str(headers),
                                    str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(ran.returncode, 0, 'borrowed argument graph escaped the snapshot oracle')
            self.assertIn('buffers[0]', ran.stderr)


if __name__ == '__main__':
    unittest.main()
