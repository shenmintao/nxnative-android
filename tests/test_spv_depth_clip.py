"""Depth arithmetic, XFB preservation, geometry emission and real cache gates."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(src, sig):
    i = src.index(sig)
    return src[i:src.index('\n}', i) + 3]


def instructions(data):
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    at = 5
    while at < len(w):
        n = w[at] >> 16
        yield w[at] & 65535, w[at + 1:at + n]
        at += n


def execute(data):
    # An independent constant-fixture evaluator also detects wrong arithmetic
    # that spirv-val would legitimately accept.
    values, pointers, memory, members = {}, {}, {}, {}
    builtins, captured, floats = set(), set(), set()
    emitted, returns = [], []
    for op, x in instructions(data):
        if op == 71 and x[1] == 11 and x[2] == 0:
            builtins.add(x[0])
        if op == 72 and x[2] == 11 and x[3] == 0:
            members[x[0]] = x[1]
        if op == 71 and x[1] == 35:
            captured.add(x[0])
        if op == 32:
            pointers[x[0]] = x[2]
        if op == 22:
            floats.add(x[0])
    pos = None
    for op, x in instructions(data):
        if op == 43:
            values[x[1]] = struct.unpack('<f', struct.pack('<I', x[2]))[0] if x[0] in floats else x[2]
        elif op == 44:
            values[x[1]] = [values[i] for i in x[2:]]
        elif op == 59:
            pointers[x[1]] = (x[1], None)
            if x[1] in builtins:
                pos = (x[1], None)
            elif pointers.get(x[0]) in members:
                pos = (x[1], members[pointers[x[0]]])
        elif op == 65:
            pointers[x[1]] = (x[2], values[x[3]])
        elif op == 62:
            memory[pointers[x[0]]] = values[x[1]]
        elif op == 61:
            values[x[1]] = memory[pointers[x[2]]]
        elif op == 81:
            values[x[1]] = values[x[2]][x[3]]
        elif op == 129:
            values[x[1]] = values[x[2]] + values[x[3]]
        elif op == 133:
            values[x[1]] = values[x[2]] * values[x[3]]
        elif op == 82:
            v = list(values[x[3]])
            v[x[4]] = values[x[2]]
            values[x[1]] = v
        elif op in (218, 220):
            emitted.append((list(memory[pos]), [list(memory[(v, None)]) for v in sorted(captured) if (v, None) in memory]))
        elif op == 253:
            returns.append(list(memory[pos]))
    return returns, emitted


def assembly(block=False, gs=False, xfb=False, multiple=False):
    caps = 'OpCapability Shader\n' + ('OpCapability Geometry\n' if gs else '') + ('OpCapability TransformFeedback\n' if xfb else '')
    mode = ('OpExecutionMode %main InputPoints\nOpExecutionMode %main OutputPoints\nOpExecutionMode %main OutputVertices 2\n' if gs else '')
    mode += 'OpExecutionMode %main Xfb\n' if xfb else ''
    dec = 'OpMemberDecorate %Block 0 BuiltIn Position\nOpDecorate %Block Block\n' if block else 'OpDecorate %pos BuiltIn Position\n'
    if xfb:
        dec += 'OpDecorate %pos XfbBuffer 0\nOpDecorate %pos XfbStride 16\n'
        dec += 'OpMemberDecorate %Block 0 Offset 0\n' if block else 'OpDecorate %pos Offset 0\n'
    types = '%Block = OpTypeStruct %v4\n%bp = OpTypePointer Output %Block\n%pos = OpVariable %bp Output\n' if block else '%pos = OpVariable %vp Output\n'
    body = '%p = OpAccessChain %vp %pos %zero\nOpStore %p %original\n' if block else 'OpStore %pos %original\n'
    body += 'OpEmitVertex\nOpEmitVertex\n' if gs else ''
    body += ('OpSelectionMerge %merge None\nOpBranchConditional %yes %a %b\n%a = OpLabel\nOpReturn\n%b = OpLabel\nOpReturn\n%merge = OpLabel\nOpUnreachable\n' if multiple else 'OpReturn\n')
    return caps + 'OpMemoryModel Logical GLSL450\nOpEntryPoint ' + ('Geometry' if gs else 'Vertex') + ' %main "main" %pos\n' + mode + dec + '''%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 0
%zero = OpConstant %int 0
%v4 = OpTypeVector %float 4
%vp = OpTypePointer Output %v4
%minus = OpConstant %float -1
%one = OpConstant %float 1
%original = OpConstantComposite %v4 %one %one %minus %one
%bool = OpTypeBool
%yes = OpConstantTrue %bool
''' + types + '%main = OpFunction %void None %fn\n%entry = OpLabel\n' + body + 'OpFunctionEnd\n'


class DepthClip(unittest.TestCase):
    def test_position_forms_returns_geometry_xfb_and_gate(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        if not cc or not ndk:
            self.skipTest('host compiler and NDK shader tools needed')
        tools = next(ndk.glob('shader-tools/*'))
        assembler, validator = next(tools.glob('spirv-as*')), next(tools.glob('spirv-val*'))
        with tempfile.TemporaryDirectory(prefix='depth-clip-') as temp:
            d = Path(temp)
            exe = d / 'depth.exe'
            subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Werror', '-Wno-misleading-indentation',
                            '-I' + str(ROOT / 'nxnative/dm/android'), str(ROOT / 'tests/native/spv_depth_clip.c'), '-o', str(exe)], check=True)
            for block, gs, xfb, multiple in [(False, False, False, False), (True, False, False, False), (True, False, False, True),
                                            (False, True, False, False), (True, True, False, False), (False, False, True, False),
                                            (True, False, True, False), (True, True, True, False)]:
                asm, src, out = d / 'input.spvasm', d / 'input.spv', d / 'patched.spv'
                asm.write_text(assembly(block, gs, xfb, multiple), encoding='utf8')
                subprocess.run([str(assembler), '--target-env', 'vulkan1.1', str(asm), '-o', str(src)], check=True, capture_output=True)
                for enabled in (0, 1):
                    r = subprocess.run([str(exe), str(src), str(out), '2' if gs else '0', str(enabled)],
                                       check=True, capture_output=True, text=True)
                    v = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(out)], capture_output=True, text=True)
                    self.assertEqual(v.returncode, 0, f'{block=} {gs=} {xfb=} {multiple=}\n' + v.stdout + v.stderr)
                    if not enabled:
                        self.assertEqual(src.read_bytes(), out.read_bytes())
                        continue
                    self.assertIn('captured=' + str(int(xfb)), r.stdout)
                    if multiple:
                        self.assertEqual(sum(op == 133 for op, _ in instructions(out.read_bytes())), 2)
                    else:
                        returns, emits = execute(out.read_bytes())
                        if gs:
                            self.assertEqual([v[0] for v in emits], [[1, 1, 0, 1]] * 2)
                            if xfb:
                                self.assertEqual([v[1] for v in emits], [[[1, 1, -1, 1]]] * 2)
                        else:
                            self.assertEqual(returns, [[1, 1, 0, 1]])

    def test_real_module_variants(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        if not cc or not ndk:
            self.skipTest('compiler/NDK required')
        android = ROOT / 'nxnative/dm/android'
        src, h = (android / 'gfx_vk_shader.c').read_text(), (android / 'gfx_vk.h').read_text()
        i = h.index('enum { RK_')
        types = h[i:h.index('} vkshader_t;', i) + len('} vkshader_t;')]
        prefix = (ROOT / 'tests/native/spv_nonwritable.c').read_text().split('/* SHADER FUNCTIONS */')[0].replace('/* SHADER TYPES */', types)
        prefix = prefix.replace('int vertex_stores, has_xfb, descriptor_buffer;', 'int vertex_stores, has_xfb, descriptor_buffer, has_depth_clip_ctl;')
        funcs = '\n'.join(function(src, k) for k in ('static void shader_nonwritable(', 'static int shader_depth_clip(',
                         'static VkShaderModule alt_module(', 'static void shader_variant_add(', 'static VkShaderModule shader_module(',
                         'VkShaderModule vk_preraster_module('))
        body = r'''
#include "vk_depth_variant.h"
int main(int n,char**v){assert(n==2);FILE*f=fopen(v[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);
 uint32_t*w=malloc(bytes);assert(fread(w,1,bytes,f)==(size_t)bytes);fclose(f);
 vkshader_t s={.stage=0,.mod=(VkShaderModule)(uintptr_t)10000,.spv=w,.nspv=bytes/4};
 for(int ext=0;ext<2;ext++)for(int neg=0;neg<2;neg++)for(int gs=0;gs<2;gs++)for(int st=0;st<3;st++)
  assert(vk_depth_variant_stage(neg,ext,gs,st)==(neg&&!ext&&st==(gs?2:0)));
 V.has_depth_clip_ctl=1;assert(vk_preraster_module(&s,0,0,0,0,1)==s.mod&&!modules);
 VkShaderModule normal=vk_preraster_module(&s,0,0,0,1,1);assert(normal!=s.mod&&!memcmp(captured,w,bytes));
 V.has_depth_clip_ctl=0;VkShaderModule clipped=vk_preraster_module(&s,0,0,0,0,1);assert(clipped!=s.mod);unsigned count=modules;
 assert(vk_preraster_module(&s,0,0,0,0,0)==s.mod&&modules==count);
 assert(vk_preraster_module(&s,0,0,0,0,1)==clipped&&modules==count);
 assert(vk_preraster_module(&s,0,0,0,1,0)==normal&&modules==count);
 assert(vk_preraster_module(&s,0,0,0,1,1)!=normal&&modules==count+1);
 assert(vk_preraster_module(&s,0,0,1u<<31,0,1)!=s.mod);
 V.has_depth_clip_ctl=1;assert(vk_preraster_module(&s,0,0,0,0,1)==s.mod);
 free(s.var);free(w);puts("PASS");return 0;}
'''
        with tempfile.TemporaryDirectory(prefix='depth-variant-') as temp:
            d = Path(temp)
            c, exe = d / 'check.c', d / 'check.exe'
            c.write_text(prefix + funcs + body, encoding='utf8')
            headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'))
            r = subprocess.run([cc, '-std=gnu11', '-O2', '-Wno-unused-function', '-Wno-unused-parameter', '-idirafter',
                                str(headers), '-I' + str(android), str(c), '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            tools = next(ndk.glob('shader-tools/*'))
            asm, original = d / 'input.spvasm', d / 'input.spv'
            asm.write_text(assembly(), encoding='utf8')
            subprocess.run([str(next(tools.glob('spirv-as*'))), '--target-env', 'vulkan1.1', str(asm), '-o', str(original)], check=True)
            r = subprocess.run([str(exe), str(original)], capture_output=True, text=True, check=True)
            self.assertIn('PASS', r.stdout)
