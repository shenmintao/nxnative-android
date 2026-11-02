"""Program warming owns keys, preserves modules and destroys every temporary PSO."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'


class PairWarm(unittest.TestCase):
    def run_native(self, release=False):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host C compiler and NDK Vulkan headers required')
        source = (ANDROID / 'gfx_vk_exec.c').read_text(encoding='utf8')
        key = source[source.index('typedef struct {\n    uint64_t key[3]'):source.index('} pkey;') + len('} pkey;')]
        code = (ROOT / 'tests/native/vk_pair_warm.c').read_text(encoding='utf8').replace('/* PIPELINE KEY */', key)
        with tempfile.TemporaryDirectory(prefix='vk-pair-warm-') as temp:
            src, exe = Path(temp) / 'check.c', Path(temp) / 'check.exe'
            src.write_text(code, encoding='utf8')
            command = [cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-Wno-misleading-indentation', '-Wno-missing-field-initializers',
                       '-idirafter', str(headers), '-I', str(ANDROID), str(src), '-o', str(exe)]
            if release:
                command += ['-DDM_RELEASE']
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for mode in ('on', 'off'):
                result = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_queue_eligibility_canonical_and_workers(self):
        self.run_native()

    def test_warming_and_startup_switch_exist_in_release(self):
        self.run_native(release=True)

    def test_optional_program_hook_and_cache_only_ownership(self):
        hle = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf8')
        warm = (ANDROID / 'vk_pair_warm.h').read_text(encoding='utf8')
        exe = (ANDROID / 'gfx_vk_exec.c').read_text(encoding='utf8')
        self.assertIn('if (g_nvn_on_program) g_nvn_on_program(k->key, k->ok, n);', hle)
        self.assertIn('stages[i] = vk_shader_stage(values[i])', warm)
        self.assertIn('vk_shader_is_xfb(pair.key[0])', warm)
        self.assertIn('vk_compile_thread("vk pair warm")', warm)
        self.assertIn('nc >= 8 ? 4 : nc >= 4 ? 2 : 1', warm)
        self.assertIn('build_pipeline(&key, layout, stg)', warm)
        self.assertIn('vkDestroyPipeline(V.dev, p, NULL)', warm)
        self.assertNotIn('pipe_record(', warm)
        self.assertNotIn('g_pipes', warm)
        self.assertNotIn('DM_RELEASE', warm)
        self.assertIn('!__atomic_sub_fetch(&g_pw_workers, 1, __ATOMIC_ACQ_REL)) pair_warm_ready()', exe)
        self.assertIn('vk_pair_warm_report()', (ANDROID / 'gfx_vk_present.c').read_text(encoding='utf8'))
