// Exercise the production conservative SPIR-V analysis. With a .spv argument,
// print its inferred binding ranges (also used with glslc/spirv-val fixtures).
#include "spv_uniform_extent.h"
#include <stdio.h>

static uint32_t code[512], used;
static void emit(uint32_t op, const uint32_t *args, uint32_t n) {
    code[used++] = (n + 1) << 16 | op;
    if (n) memcpy(code + used, args, n * 4);
    used += n;
}
#define E(op, ...) do { const uint32_t args[] = {__VA_ARGS__}; emit(op, args, sizeof args / sizeof *args); } while (0)
static void module(int mode) {
    uint32_t header[] = {0x07230203, 0x00010000, 0, 64, 0}; memcpy(code, header, sizeof header); used = 5;
    E(71, 5, 6, mode == 3 ? 32 : 16); E(71, 6, 2); E(72, 6, 0, 35, mode == 7 ? 16 : 0); E(71, 8, 33, 0);
    E(21, 1, 32, 0); E(22, 2, mode == 6 ? 64 : 32); E(23, 3, 2, 4); E(43, 1, 4, 4096);
    E(28, 5, 3, 4); E(30, 6, 5); E(32, 7, 2, 6); E(59, 7, 8, 2); E(32, 9, 2, 2);
    E(43, 1, 10, 0); E(43, 1, 11, 2); E(43, 1, 12, 3); E(32, 13, 2, 3); E(43, 1, 14, 255);
    if (mode == 8) { E(71, 25, 33, 1); E(59, 7, 25, 2); }
    E(54, 2, 15, 0, 16); E(128, 1, 20, 10, 11);
    if (mode == 4) { E(65, 13, 21, 8, 10, 14); E(61, 3, 22, 21); }
    else { E(65, 9, 21, 8, 10, mode == 1 ? 20 : 11, 12); E(61, 2, 22, 21); }
    if (mode == 2) { E(83, 9, 23, 21); E(61, 2, 24, 23); }
    if (mode == 5) { E(65, 9, 23, 21, 10); E(61, 2, 24, 23); }
    if (mode == 8) { E(65, 9, 26, 25, 10, 14, 12); E(61, 2, 27, 26); }
    emit(253, NULL, 0); emit(56, NULL, 0);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
        fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
        if (size < 20 || size % 4) { fclose(f); return 2; }
        uint32_t *w = malloc((size_t)size), out[96];
        if (!w || fread(w, 1, (size_t)size, f) != (size_t)size) { free(w); fclose(f); return 2; }
        fclose(f); vk_spv_uniform_extents(w, (uint32_t)size / 4, out, 96);
        for (int i = 0; i < 96; i++) if (out[i]) printf("%d:%u ", i, out[i]);
        puts(""); free(w); return 0;
    }
    const uint32_t expected[] = {48, 0, 0, 0, 4096, 0, 0, 0, 48};
    for (int mode = 0; mode < 9; mode++) {
        uint32_t out[4]; module(mode); vk_spv_uniform_extents(code, used, out, 4);
        if (out[0] != expected[mode] || out[1] != (mode == 8 ? 4096u : 0u)) { fprintf(stderr, "extent case %d failed: %u / %u\n", mode, out[0], out[1]); return 1; }
    }
    module(0);
    for (uint32_t i = 0; i < used; i++) { uint32_t out[4]; vk_spv_uniform_extents(code, i, out, 4); }
    code[5] = 0; uint32_t out[4]; vk_spv_uniform_extents(code, used, out, 4);
    if (out[0]) return 1;
    puts("uniform extents: static, dynamic, escaping/nested pointers, layouts, multiple blocks and truncated instructions passed");
    return 0;
}
