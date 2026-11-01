"""Vertex register bits and independently validated generated interfaces."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT=Path(__file__).resolve().parents[1]


class VertexInputClasses(unittest.TestCase):
    def test_register_bits_aliases_failure_guards_and_valid_shader_interfaces(self):
        cc=shutil.which('cc') or shutil.which('clang')
        if not cc:self.skipTest('host C compiler required')
        ndk,_=find_ndk()
        tools=next(ndk.glob('shader-tools/*'),None) if ndk else None
        with tempfile.TemporaryDirectory(prefix='vertex-inputs-') as temp:
            p=Path(temp);exe=p/'check.exe'
            subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'nxnative/dm/android'),str(ROOT/'tests/native/spv_vertex_inputs.c'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            self.assertIn('PASS',result.stdout)
            if not tools:return
            glslc=next(tools.glob('glslc*'),None);validator=next(tools.glob('spirv-val*'),None)
            if not glslc or not validator:return
            shader=p/'input.vert';original=p/'input.spv'
            shader.write_text('''#version 450
layout(location=0) in vec4 a;
layout(location=1) in ivec4 b;
layout(location=2) in uvec4 c;
layout(set=0,binding=0,std430) buffer Results {uvec4 results[];};
void main(){results[0]=floatBitsToUint(a);results[1]=uvec4(b);results[2]=c;gl_Position=vec4(a.xy,0,1);}
''')
            subprocess.run([str(glslc),'--target-env=vulkan1.1',str(shader),'-o',str(original)],check=True)
            signatures=[0,*[k|(k<<3)|(k<<6) for k in range(1,6)],2|(1<<3)|(3<<6)]
            for signature in signatures:
                transformed=p/f'variant-{signature:x}.spv'
                subprocess.run([str(exe),str(original),str(transformed),f'{signature:x}'],check=True)
                subprocess.run([str(validator),'--target-env','vulkan1.1',str(transformed)],check=True,capture_output=True)
                if not signature:self.assertEqual(original.read_bytes(),transformed.read_bytes())
