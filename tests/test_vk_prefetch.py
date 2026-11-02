"""Bounded preparation keeps exact demand results and leaves replay state intact."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class VulkanPrefetch(unittest.TestCase):
    def run_case(self, case, arguments=('',)):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and NDK Vulkan headers required')
        source = (ANDROID / 'gfx_vk_exec.c').read_text(encoding='utf-8')
        state_start = source.index('typedef struct {\n    const prog_t *prog;')
        state = source[state_start:source.index('} vk_replay_state;') + len('} vk_replay_state;')]
        key_start = source.index('typedef struct {\n    uint64_t key[3]')
        key = source[key_start:source.index('} pkey;') + len('} pkey;')]
        body = (ROOT / 'tests/native/vk_prefetch.c').read_text(encoding='utf-8')
        code = body.replace('/* REPLAY TYPES */', state + '\n' + key)
        if case == 2:
            functions = '\n'.join(function(source, name) for name in (
                'static VkPrimitiveTopology topo(', 'static uint32_t vertex_input_kind(',
                'static uint32_t vertex_input_code(', 'static uint64_t key64(',
                'static int rt_same_state(const vk_replay_state *state, const p_set_rt *r) {',
                'static void pipeline_key('))
            code = code.replace('/* KEY FUNCTIONS */', functions)
        if case == 0:
            shader = (ANDROID / 'gfx_vk_shader.c').read_text(encoding='utf-8')
            code = code.replace('/* CACHED SHADER */', function(shader, 'vkshader_t *vk_shader_cached('))
        with tempfile.TemporaryDirectory(prefix='vk-prefetch-') as temp:
            src, exe = Path(temp) / 'check.c', Path(temp) / 'check.exe'
            src.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers', '-Wno-misleading-indentation',
                '-pthread', f'-DCASE={case}', '-idirafter', str(headers), '-I', str(ANDROID),
                '-I', str(ROOT / 'nxnative/dm/gfx/nvn'), '-I', str(ROOT / 'nxnative/dm/gfx/nvn/include'),
                str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for argument in arguments:
                result = subprocess.run([str(exe), argument], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_program_prefetch_owns_keys_is_bounded_and_keeps_demand_fallback(self):
        self.run_case(0, ('on', 'off'))

    def test_pipeline_jobs_wait_for_exact_key_steal_queued_work_and_publish_once(self):
        self.run_case(1)

    def test_upcoming_command_scan_preserves_state_masks_and_dependency_boundaries(self):
        self.run_case(2)
