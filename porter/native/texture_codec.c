/* Desktop-only BC decode/direct ASTC conversion. No allocator, CRT or GPU API.
 * The decoder and Tegra addressing are the runtime sources, not approximations.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../../nxnative/dm/third_party/bcdec/bcdec.h"
#include "../../nxnative/dm/gfx/tex/nvn_deswizzle.h"
#include "bc_astc.h"

EXPORT int texture_avx2(void) {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if ((c & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28))) return 0;
    __asm__("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    if ((a & 6) != 6) return 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b & (1u << 5)) != 0;
#else
    return 0;
#endif
}

EXPORT int linear_bc(const uint8_t *src, uint64_t size, uint8_t *dst, unsigned w, unsigned h,
                     unsigned z, unsigned bpb, unsigned th, unsigned td, unsigned linear) {
    unsigned bx = (w + 3) / 4, by = (h + 3) / 4, row = bx * bpb;
    if (!w || !h || !z || th > 5 || td > 5 || (bpb != 8 && bpb != 16)) return -1;
    for (unsigned k = 0; k < z; k++) for (unsigned y = 0; y < by; y++) for (unsigned x = 0; x < row; x += bpb) {
        uint64_t off = linear ? ((uint64_t)k * by + y) * row + x : nvn_bl_offset(x, y, k, row, by, th, td);
        if (off > size || bpb > size - off) return -1;
        memcpy(dst + ((uint64_t)k * by + y) * row + x, src + off, bpb);
    }
    return 0;
}
EXPORT void decode_bc(unsigned fmt, const uint8_t *src, uint8_t *dst, unsigned n) {
    unsigned base = fmt >> 8, bpb = base == 0x1a ? 8 : 16;
    for (unsigned b = 0; b < n; b++, src += bpb, dst += 64) {
        if (base == 0x1a) bcdec_bc1(src, dst, 16);
        else if (base == 0x1c) bcdec_bc3(src, dst, 16);
        else if (base == 0x1e) {
            uint8_t rg[32]; bcdec_bc5(src, rg, 8, 0);
            for (unsigned i = 0; i < 16; i++) { dst[4*i] = rg[2*i]; dst[4*i+1] = rg[2*i+1]; dst[4*i+2] = 0; dst[4*i+3] = 255; }
        }
    }
}
