"""NVN vertex output defaults preserve explicit writes and valid SPIR-V."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


class VertexOutputDefaults(unittest.TestCase):
    def test_default_values_guards_failures_and_valid_interfaces(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='vertex-outputs-') as temp:
            folder = Path(temp)
            exe = folder / 'outputs.exe'
            args = [cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'nxnative/dm/android')]
            subprocess.run([*args, str(ROOT / 'tests/native/spv_vertex_outputs.c'), '-o', str(exe)], check=True)
            checked = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertIn('PASS', checked.stdout)
            # A wrong W default must fail the independent value evaluator.
            broken = folder / 'spv_vertex_outputs.h'
            broken.write_text((ROOT / 'nxnative/dm/android/spv_vertex_outputs.h').read_text(encoding='utf8')
                              .replace('0x3f800000', '0'), encoding='utf8')
            damaged = folder / 'damaged.exe'
            subprocess.run([cc, '-std=c11', '-O2', '-I' + str(folder),
                            str(ROOT / 'tests/native/spv_vertex_outputs.c'), '-o', str(damaged)], check=True)
            rejected = subprocess.run([str(damaged)], capture_output=True, text=True)
            self.assertEqual(rejected.returncode, 1)
            self.assertIn('expected', rejected.stderr)
            ndk, _ = find_ndk()
            tools = next(ndk.glob('shader-tools/*'), None) if ndk else None
            if not tools:
                self.skipTest('NDK shader tools are required for generated-module validation')
            glslc = next(tools.glob('glslc*'), None)
            validator = next(tools.glob('spirv-val*'), None)
            self.assertIsNotNone(glslc)
            self.assertIsNotNone(validator)
            vertex = '''#version 450
layout(location=0) out vec4 value;
layout(push_constant) uniform Params {uint mask;} p;
void main(){
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    if((p.mask&1u)!=0u)value.x=0.25;
    if((p.mask&2u)!=0u)value.y=0.5;
    if((p.mask&4u)!=0u)value.z=0.75;
    if((p.mask&8u)!=0u)value.w=0.125;
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
}
'''
            fragment = '#version 450\nlayout(location=0) out vec4 color;void main(){color=vec4(1);}\n'
            for stage, source, extra in [('vert', vertex, []), ('vert', vertex, ['-g']), ('frag', fragment, [])]:
                name = stage + ('-debug' if extra else '')
                shader, original = folder / (name + '.' + stage), folder / (name + '.spv')
                shader.write_text(source, encoding='utf8')
                subprocess.run([str(glslc), '--target-env=vulkan1.1', *extra, str(shader), '-o', str(original)], check=True)
                for mask in ('0', '1', 'ffffffff'):
                    output = folder / (name + '-' + mask + '.spv')
                    subprocess.run([str(exe), str(original), str(output), mask], check=True, capture_output=True)
                    subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(output)], check=True, capture_output=True)
                    if stage == 'frag' or mask == '0':
                        self.assertEqual(original.read_bytes(), output.read_bytes())
                    else:
                        again = folder / 'again.spv'
                        subprocess.run([str(exe), str(output), str(again), mask], check=True, capture_output=True)
                        self.assertEqual(output.read_bytes(), again.read_bytes())
            loader = (ROOT / 'nxnative/dm/android/gfx_vk_shader.c').read_text(encoding='utf8')
            self.assertIn('vk_spv_vertex_outputs(w, nw, UINT32_MAX,', loader)
            self.assertIn('vk_spv_vertex_outputs(w, nw, xo,', loader)
