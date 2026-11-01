// Texture storage against shapes the original NVN driver gives (digitalhivehub ran Tears of the Kingdom's driver under emulation, nxnative PR #11; MacNativeGames 3c482a7):
// cube array depth counts layer-faces, packaged byte 0 = block height (low nibble) and block depth (high nibble), linear storage (LINEAR or LINEAR_RENDER_TARGET alone)
// counts pixel rows, also for BC. pkg is byte 0 + 1 (0 = automatic), as nvn_texdesc stores it. The MINIMAL_LAYOUT shapes (flag 0x200) are the driver's from PR #11's t_tex,
// the array and packaged ones from TotK 1.2.1's driver (sdk b9046c31, GitHub issue #183): automatic blocks only when they add no padding to the layer, else 1 GOB. The sparse (flag 0x20) shapes are block heights read off a game's guest bytes instead: a sparse texture at least one 64 KB tile wide and high
// takes its tile's block height (384x384 DXT5, 1350x270 BGRA8), a smaller one keeps the automatic one (192x160 DXT5 and 384x384 DXT1, Yoshi's Crafted World)
#include <stdio.h>
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *what; int target, format, flags; unsigned w, h, depth, levels, stride; int pkg; uint64_t size, layer, off1; unsigned align, th, td; } shape_t;
static const shape_t driver[] = {
    {"cube array RGBA8 4x4 depth 6",                9, 0x25, 0,    4, 4, 6, 1, 0, 0,         3072, 512, 0, 512, 0, 0},
    {"cube array RGBA8 64x64 depth 12, 7 levels",   9, 0x25, 0,    64, 64, 12, 7, 0, 0,      294912, 24576, 16384, 512, 3, 0},
    {"packaged 3D BC4 64^3, 7 levels, byte 0x40",   2, 0x49, 0,    64, 64, 64, 7, 0, 0x41,   163328, 163328, 131072, 512, 0, 4},
    {"packaged 3D RGBA8 32^3, byte 0x00",           2, 0x25, 0,    32, 32, 32, 1, 0, 0x01,   131072, 131072, 0, 512, 0, 0},
    {"packaged 3D RGBA8 32^3, byte 0x12",           2, 0x25, 0,    32, 32, 32, 1, 0, 0x13,   131072, 131072, 0, 512, 2, 1},
    {"packaged 2D RGBA8 64x64, byte 0x13",          1, 0x25, 0,    64, 64, 1, 1, 0, 0x14,    16384, 16384, 0, 512, 3, 0},
    {"linear RT RGBA8 260x260 stride 1152",         1, 0x25, 0x40, 260, 260, 1, 1, 1152, 0,  299520, 299520, 0, 128, 0, 0},
    {"linear RT RGBA8 1x1 stride 128",              1, 0x25, 0x40, 1, 1, 1, 1, 128, 0,       128, 128, 0, 128, 0, 0},
    {"linear BC1 64x64 stride 128",                 1, 0x42, 0x10, 64, 64, 1, 1, 128, 0,     8192, 8192, 0, 32, 0, 0},
    {"minimal RG8 260x260, 9 levels",               1, 0x0d, 0x200, 260, 260, 1, 9, 0, 0,   214528, 214528, 152064, 512, 0, 0},
    {"minimal RGB565 320x320 (Depths)",             1, 0x3c, 0x200, 320, 320, 1, 1, 0, 0,   204800, 204800, 0, 512, 0, 0},
    {"minimal RGB5A1 320x320 (Depths)",             1, 0x3b, 0x200, 320, 320, 1, 1, 0, 0,   204800, 204800, 0, 512, 0, 0},
    {"minimal RGBA8 64x128, no padding: automatic", 1, 0x25, 0x200, 64, 128, 1, 1, 0, 0,    32768, 32768, 0, 512, 4, 0},
    {"minimal R8 8x1920, 11 levels",                1, 0x01, 0x200, 8, 1920, 1, 11, 0, 0,   246784, 246784, 122880, 512, 0, 0},
    {"minimal 3D RGBA8 64x64x8: automatic",         2, 0x25, 0x200, 64, 64, 8, 1, 0, 0,     131072, 131072, 0, 512, 0, 3},
    {"minimal 3D RGBA8 64x64x9",                    2, 0x25, 0x200, 64, 64, 9, 1, 0, 0,     147456, 147456, 0, 512, 0, 0},
    {"minimal 2D array RGBA8 64x64x3, 7 levels",    4, 0x25, 0x200, 64, 64, 3, 7, 0, 0,     70656, 23552, 16384, 512, 0, 0},
    {"minimal RGB565 320x320, packaged byte 0x04",  1, 0x3c, 0x200, 320, 320, 1, 1, 0, 5,   245760, 245760, 0, 512, 4, 0},
    {"sparse DXT5 192x160, under a tile",           1, 0x48, 0x20, 192, 160, 1, 1, 0, 0,     0x10000, 0x10000, 0, 0x10000, 2, 0},
    {"sparse DXT1 384x384, narrower than a tile",   1, 0x45, 0x20, 384, 384, 1, 1, 0, 0,     0x20000, 0x20000, 0, 0x10000, 4, 0},
    {"sparse DXT5 384x384, a tile wide",            1, 0x48, 0x20, 384, 384, 1, 1, 0, 0,     0x40000, 0x40000, 0, 0x10000, 3, 0},
    {"sparse BGRA8 1350x270",                       1, 0x9c, 0x20, 1350, 270, 1, 1, 0, 0,    0x210000, 0x210000, 0, 0x10000, 4, 0},
};

int main(void) {
    int fail = 0, n = (int)(sizeof driver / sizeof *driver);
    for (int i = 0; i < n; i++) {
        const shape_t *c = &driver[i];
        nvn_texdesc d = {.target = c->target, .format = c->format, .flags = c->flags, .width = c->w, .height = c->h, .depth = c->depth, .levels = c->levels, .stride = c->stride, .pkg_block_h = c->pkg};
        nvn_texlayout l; nvn_tex_layout(&d, &l);
        uint64_t off1 = c->levels > 1 ? nvn_tex_level_offset(&d, 1) : 0;
        if (l.size != c->size || l.layer_size != c->layer || off1 != c->off1 || l.align != c->align || l.tileH != c->th || l.tileD != c->td) {
            fail++;
            printf("FAIL %s: size %llu layer %llu level 1 at %llu align %u block %u/%u; driver %llu %llu %llu %u %u/%u\n", c->what, (unsigned long long)l.size, (unsigned long long)l.layer_size,
                   (unsigned long long)off1, l.align, l.tileH, l.tileD, (unsigned long long)c->size, (unsigned long long)c->layer, (unsigned long long)c->off1, c->align, c->th, c->td);
        }
    }
    nvn_texdesc icon = {.target = 1, .format = 0x48, .flags = 0x20, .width = 192, .height = 160, .depth = 1, .levels = 1};
    uint32_t tw, th, td; int tiled = nvn_tex_sparse_tiles(&icon, &tw, &th, &td);
    if (tw != 256 || th != 256 || tiled) { fail++; printf("FAIL sparse DXT5 192x160: tile %ux%u texels, %d tiled levels; want 256x256, 0\n", tw, th, tiled); }

    // Every non-linear oracle shape must also stay inside its reported storage
    // while round-tripping every mip and layer, including packaged 3D slices.
    for (int i = 0; i < n; i++) {
        const shape_t *c = &driver[i]; if (c->flags & 0x50) continue;
        nvn_texdesc d = {.target=c->target,.format=c->format,.flags=c->flags,.width=c->w,.height=c->h,.depth=c->depth,.levels=c->levels,.stride=c->stride,.pkg_block_h=c->pkg};
        nvn_texlayout l; nvn_tex_layout(&d,&l);
        uint8_t *guard=malloc((size_t)l.size+64); if(!guard) return 2;
        memset(guard,0xa5,(size_t)l.size+64); uint8_t *storage=guard+32;
        for (uint32_t layer=0;layer<l.layers;layer++) for (uint32_t level=0;level<d.levels;level++) {
            uint32_t row,rows,slices,bpb; uint64_t base; unsigned th,td;
            if(nvn_deswizzle_geom(&d,level,layer,&row,&rows,&slices,&base,&th,&td,&bpb)) {fail++;continue;}
            size_t bytes=(size_t)row*rows*slices; uint8_t *input=malloc(bytes),*output=malloc(bytes);
            if(!input||!output) return 2;
            for(size_t j=0;j<bytes;j++) input[j]=(uint8_t)(j*17+level*7+layer*5);
            int bad=nvn_swizzle(&d,level,layer,input,row,storage) || nvn_deswizzle(&d,level,layer,storage,output,row) || memcmp(input,output,bytes);
            if(bad) {fail++;printf("FAIL round trip %s level %u layer %u\n",c->what,level,layer);}
            for(int j=0;j<32;j++) if(guard[j]!=0xa5 || guard[32+l.size+j]!=0xa5) {fail++;printf("FAIL guard %s\n",c->what);break;}
            free(input);free(output);
        }
        free(guard);
    }
    // A standalone linear-render-target flag must never use block-linear copies.
    nvn_texdesc linear={.target=1,.format=0x25,.flags=0x40,.width=16,.height=16,.depth=1,.levels=1,.stride=128};
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned linear_th,linear_td;
    if(nvn_deswizzle_geom(&linear,0,0,&row,&rows,&slices,&base,&linear_th,&linear_td,&bpb)!=-1) {fail++;puts("FAIL linear RT accepted as block-linear");}
    printf("t_tex_layout: %d of %d shapes match\n", n + 1 - fail, n + 1);
    return fail != 0;
}
