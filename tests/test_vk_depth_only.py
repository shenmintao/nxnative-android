"""NVN stage bindings admit depth-only draws without treating missing code as absent."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


class DepthOnlyPrograms(unittest.TestCase):
    def test_stage_masks_missing_shaders_and_colour_preservation(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        key_start = source.index('static uint64_t key64(')
        key = source[key_start:source.index('\n', key_start)]
        draw = source[source.index('static int prepare_draw('):source.index('    if (g_x_nogs && S.sh[2])')]
        draw += '    (void)prim; (void)indexed; return 1;\n}\n'
        start = source.index('    for (int i = 0; i < S.ncol; i++) {', source.index('// attachments and blending'))
        colour = source[start:source.index('    k.clip_neg1', start)]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { unsigned nstage; uint64_t keys[6]; } prog_t;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { uint64_t key; } vkshader_t;
typedef struct { int is_int; } vkfmt_t;
typedef struct { uint32_t src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a; } blend_t;
static struct {
    const prog_t *prog; vkshader_t *sh[6]; int prog_ok, rast_discard; uint32_t missing_stages;
    int ncol; uint32_t cfmt[8]; struct { uint32_t mask; } chmask;
    struct { uint32_t blend_en; } color; blend_t blend[8];
    int rt_has_col[8],rt_has_cview[8]; struct { struct { int format; } d; } rt_col[8];
    struct {int set_format,format;} rt_cview[8];
} S;
typedef struct {
    uint32_t cfmt[8], wmask[8], blend_en, fdz, raster_discard;
    struct { uint32_t src, dst, srca, dsta, eq, eqa; } bl[8];
} pkey;
static struct { unsigned skip_prog; } g_vkst;
static int g_tracing, g_draw_i, g_bis_trace=-1, g_bis_var, g_var_now, g_cb_draws;
static vkshader_t shaders[6];
static unsigned missing;
static int prog_key(const prog_t *p, unsigned i, uint8_t key[8]) {
    if (i >= p->nstage) return 0;
    for (int j=0;j<8;j++) key[j]=(uint8_t)(p->keys[i] >> (56-8*j));
    return 1;
}
static int vk_shader_stage(uint64_t key) {
    switch(key) { case 1: case 11: return 0; case 2: case 12: return 1; case 3: return 2; case 6: return 5; default: return -1; }
}
static vkshader_t *vk_shader(uint64_t key, int stage) {
    if(key==11 || key==12) return NULL;
    assert(vk_shader_stage(key)==stage); shaders[stage].key=key; return &shaders[stage];
}
void vk_shader_missing(uint64_t key) { assert(key==99); missing++; }
static void shader_dump(const prog_t *p, unsigned i, uint64_t key) { (void)p; (void)i; (void)key; }
static void vk_end_pass(void) {}
static const vkfmt_t *vk_fmt(int fmt) { static vkfmt_t f; f.is_int=fmt==1; return &f; }
#define VKRED_ONCE(...) ((void)0)
''' + key + '\n' + function(source, 'static void bind_program(') + draw
        harness += 'static pkey colour_key(void) { pkey k = {0};\n' + colour + '    return k;\n}\n'
        harness += r'''
static void bind(const prog_t *p, int stages) { p_bind_program b={p,stages}; bind_program(&b); }
static void accepted(void) { unsigned n=g_vkst.skip_prog; assert(S.prog_ok && prepare_draw(4,0)); assert(g_vkst.skip_prog==n); }
static void rejected(void) { unsigned n=g_vkst.skip_prog; assert(!prepare_draw(4,1)); assert(g_vkst.skip_prog==n+1); }
int main(void) {
    const prog_t vertex={1,{1}}, fragment={1,{2}}, pair={2,{1,2}}, geometry={1,{3}}, compute={1,{6}};
    const prog_t unknown={1,{99}}, bad_vertex={1,{11}}, bad_fragment={1,{12}}, mixed_missing={2,{1,99}};
    bind(&vertex,0); accepted(); assert(S.sh[0] && !S.sh[1] && !S.missing_stages);
    bind(&fragment,2); accepted(); assert(S.sh[0] && S.sh[1]);
    bind(&geometry,4); accepted(); assert(S.sh[0] && S.sh[1] && S.sh[2]);
    bind(NULL,2); accepted(); assert(S.sh[0] && !S.sh[1] && S.sh[2]);
    bind(&fragment,2); bind(NULL,1); rejected(); assert(!S.sh[0] && S.sh[1]);
    bind(&compute,0); rejected(); assert(S.prog_ok && S.sh[5]);
    bind(&vertex,0); S.rast_discard=1; accepted(); S.rast_discard=0; accepted();
    // Unknown code and a known stage whose module failed must not silently become depth-only draws.
    bind(&pair,0); bind(&unknown,2); rejected(); assert(S.sh[0] && S.missing_stages==2 && missing==1);
    bind(&fragment,2); accepted(); assert(!S.missing_stages);
    bind(&bad_fragment,2); rejected(); assert(S.sh[0] && !S.sh[1] && S.missing_stages==2);
    bind(NULL,2); accepted(); assert(!S.missing_stages);
    bind(&bad_vertex,1); rejected();
    bind(&mixed_missing,0); rejected(); assert(S.sh[0] && S.missing_stages);
    // Updating an unrelated stage cannot repair the failed stage.
    bind(&fragment,2); rejected(); bind(&vertex,0); accepted();
    for(int fs=0;fs<2;fs++) for(int fmt=0;fmt<2;fmt++) {
        bind(fs ? &pair : &vertex,0); S.ncol=8; S.chmask.mask=0xa1b2c3d4u; S.color.blend_en=0xff;
        for(int i=0;i<8;i++) {
            S.cfmt[i]=fmt ? (uint32_t)(100+i) : 0; S.rt_has_col[i]=1;
            S.blend[i]=(blend_t){1,2,3,4,5,6};
        }
        pkey k=colour_key();
        for(int i=0;i<8;i++) {
            assert(k.cfmt[i]==S.cfmt[i]);
            assert(k.wmask[i]==(fs && fmt ? ((S.chmask.mask>>(4*i))&15u) : 0));
            assert(k.bl[i].src==1 && k.bl[i].dst==2 && k.bl[i].srca==3 && k.bl[i].dsta==4 && k.bl[i].eq==5 && k.bl[i].eqa==6);
        }
    }
    // Integer block views disable blending even when their compressed parent
    // is a sampled normalized format. The inverse view follows its own type too.
    for(int i=0;i<8;i++){S.rt_has_cview[i]=1;S.rt_cview[i].set_format=1;S.rt_cview[i].format=1;S.rt_col[i].d.format=0;}
    assert(colour_key().blend_en==0);
    for(int i=0;i<8;i++){S.rt_cview[i].format=0;S.rt_col[i].d.format=1;}
    assert(colour_key().blend_en==0xff);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-depth-only-') as temp:
            temp = Path(temp)
            c, exe = temp / 'depth.c', temp / 'depth-check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
