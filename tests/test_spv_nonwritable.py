"""Feature-gated read-only storage declarations, including real module paths."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class NonWritableStorage(unittest.TestCase):
    def test_analysis_device_gate_base_variants_alternates_and_release(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('host compiler and NDK Vulkan headers required')
        android = ROOT / 'nxnative/dm/android'
        source = (android / 'gfx_vk_shader.c').read_text(encoding='utf8')
        header = (android / 'gfx_vk.h').read_text(encoding='utf8')
        a = header.index('enum { RK_')
        types = header[a:header.index('} vkshader_t;', a) + len('} vkshader_t;')]
        code = (ROOT / 'tests/native/spv_nonwritable.c').read_text(encoding='utf8')
        code = code.replace('/* SHADER TYPES */', types)
        code = code.replace('/* SHADER FUNCTIONS */', '\n'.join(function(source, f) for f in (
            'static void shader_nonwritable(', 'vkshader_t *vk_shader(', 'static VkShaderModule alt_module(',
            'static void shader_variant_add(', 'static VkShaderModule shader_module(')))
        core = (android / 'gfx_vk_core.c').read_text(encoding='utf8')
        self.assertIn('V.vertex_stores = en.vertexPipelineStoresAndAtomics;', core)
        with tempfile.TemporaryDirectory(prefix='nonwritable-') as temp:
            folder = Path(temp)
            c = folder / 'check.c'
            c.write_text(code, encoding='utf8')
            for release in (False, True):
                exe = folder / ('release.exe' if release else 'debug.exe')
                args = [cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-misleading-indentation',
                        '-idirafter', str(headers), '-I' + str(android), str(c), '-pthread', '-o', str(exe)]
                if release:
                    args.insert(1, '-DDM_RELEASE=1')
                compiled = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                result = subprocess.run([str(exe), str(folder)], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS', result.stdout)
                tools = next(ndk.glob('shader-tools/*'))
                glslc, validator = next(tools.glob('glslc*')), next(tools.glob('spirv-val*'))
                for target in ('vulkan1.0', 'vulkan1.1'):
                    shader, original = folder / 'input.vert', folder / 'input.spv'
                    shader.write_text('''#version 450
layout(set=0,binding=2,std430) buffer Read {vec4 value[];} r;
layout(set=0,binding=3,std430) buffer Write {vec4 value[];} w;
void main(){gl_Position=r.value[0];w.value[0]=gl_Position;}
''', encoding='utf8')
                    subprocess.run([str(glslc), '--target-env=' + target, str(shader), '-o', str(original)], check=True)
                    for enabled in (0, 1):
                        output = folder / 'patched.spv'
                        subprocess.run([str(exe), str(original), str(output), str(enabled)], check=True, capture_output=True)
                        subprocess.run([str(validator), '--target-env', target, str(output)], check=True, capture_output=True)
                        if enabled:
                            self.assertEqual(original.read_bytes(), output.read_bytes())
                        else:
                            words = struct.unpack('<%dI' % (output.stat().st_size // 4), output.read_bytes())
                            bindings, readonly = {}, set()
                            at = 5
                            while at < len(words):
                                n, op = words[at] >> 16, words[at] & 65535
                                if op == 71:
                                    if words[at + 2] == 33: bindings[words[at + 1]] = words[at + 3]
                                    if words[at + 2] == 24: readonly.add(words[at + 1])
                                at += n
                            self.assertEqual({bindings[i] for i in readonly}, {2})


if __name__ == '__main__':
    unittest.main()
