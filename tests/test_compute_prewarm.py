"""Exercise recorded compute variants, legacy files and concurrent cache insertion."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk
from test_vk_guest_notify import function

ROOT = Path(__file__).resolve().parents[1]


class ComputePrewarm(unittest.TestCase):
    def test_compute_cache_and_record_compatibility(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host C compiler and NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf8')
        key = source[source.index('typedef struct {\n    uint64_t key[3]'):source.index('} pkey;') + len('} pkey;')]
        prefix, suffix = (ROOT / 'tests/native/compute_prewarm.c').read_text(encoding='utf8').split('/* PRODUCTION HELPERS */')
        code = prefix + key + '\n'
        code += 'static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER;\n'
        code += 'static nvn_hmap g_cpipes = {.klen=24}, g_pipes = {.klen=sizeof(pkey)};\nstatic FILE *g_prec;\n'
        code += '\n'.join(re.findall(r'^#define PIPE_MAGIC[^\n]*', source, re.M)) + '\n'
        code += function(source, 'static void pipe_record(const pkey *k) {') + '\n'
        code += function(source, 'static VkPipeline compute_pipeline(') + '\n'
        code += 'static pkey *g_pw; static size_t g_pw_n; static volatile size_t g_pw_next, g_pw_done;\n'
        code += 'static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout l, vkshader_t *const s[3]) { (void)k;(void)l;(void)s;graphics_calls++;return (VkPipeline)77; }\n'
        code += '\n'.join(function(source, signature) for signature in (
            'static void prewarm_progress(', 'static void *prewarm_thread(', 'static void prewarm_start(void) {'))
        with tempfile.TemporaryDirectory(prefix='compute-prewarm-') as temp:
            directory = Path(temp)
            src, exe = directory / 'check.c', directory / 'check.exe'
            src.write_text(code + suffix, encoding='utf8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers', '-pthread', '-idirafter', str(headers),
                '-I', str(ROOT / 'nxnative/dm/gfx/nvn'), str(src), '-o', str(exe)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
