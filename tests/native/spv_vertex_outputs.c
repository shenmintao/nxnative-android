#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef assert
#define assert(x) do { if (!(x)) { fprintf(stderr, "check failed at %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned allocation_calls, fail_allocation, live_allocations;
static void *test_calloc(size_t n, size_t size) {
    if (++allocation_calls == fail_allocation) return NULL;
    void *p = calloc(n, size); if (p) live_allocations++; return p;
}
static void *test_malloc(size_t size) {
    if (++allocation_calls == fail_allocation) return NULL;
    void *p = malloc(size); if (p) live_allocations++; return p;
}
static void test_free(void *p) { if (p) { assert(live_allocations); live_allocations--; free(p); } }
#define calloc test_calloc
#define malloc test_malloc
#define free test_free
#include "spv_vertex_outputs.h"
#undef calloc
#undef malloc
#undef free

static uint32_t module[512], words;
static void emit(unsigned op, unsigned n, ...) {
    assert(words + n <= sizeof module / sizeof *module);
    module[words++] = n << 16 | op;
    va_list a; va_start(a, n); for (unsigned i = 1; i < n; i++) module[words++] = va_arg(a, unsigned); va_end(a);
}
enum { OTHER_STAGE = 1, BUILTIN = 2, GLOBAL_INITIALIZER = 4, FULL_WRITE = 8, EXTRA_OUTPUT = 16, EXTRA_INTERFACE = 32, LOCAL_VARIABLE = 64, SECOND_TYPE = 128 };
static void make_module(unsigned mode) {
    uint32_t header[] = {0x07230203, 0x00010000, 0, 100, 0};
    memcpy(module, header, sizeof header); words = 5;
    emit(17, 2, 1); emit(14, 3, 0, 1);
    if (mode & EXTRA_INTERFACE) emit(15, 7, mode & OTHER_STAGE ? 4u : 0u, 40, 0x6e69616d, 0, 10, 21);
    else emit(15, 6, mode & OTHER_STAGE ? 4u : 0u, 40, 0x6e69616d, 0, 10);
    emit(71, 4, 10, mode & BUILTIN ? 11u : 30u, 0);
    if (mode & EXTRA_OUTPUT) emit(71, 4, 21, 30, 1);
    emit(19, 2, 1); emit(33, 3, 2, 1); emit(22, 3, 3, 32); emit(23, 4, 4, 3, 4);
    emit(32, 4, 5, 3, 4); emit(32, 4, 11, 3, 3); emit(21, 4, 13, 32, 0); emit(43, 4, 13, 14, 0);
    emit(43, 4, 3, 8, 0x3e800000); // 0.25
    emit(43, 4, 3, 15, 0x40000000); emit(43, 4, 3, 16, 0x40400000);
    emit(43, 4, 3, 17, 0x40800000); emit(43, 4, 3, 18, 0x40a00000);
    emit(44, 7, 4, 19, 15, 16, 17, 18); // (2,3,4,5), explicit writes must win
    if (mode & GLOBAL_INITIALIZER) emit(59, 5, 5, 10, 3, 19); else emit(59, 4, 5, 10, 3);
    if (mode & SECOND_TYPE) { emit(23, 4, 22, 3, 4); emit(32, 4, 23, 3, 22); }
    if (mode & EXTRA_OUTPUT) emit(59, 4, mode & SECOND_TYPE ? 23u : 5u, 21, 3);
    if (mode & LOCAL_VARIABLE) emit(32, 4, 30, 7, 3);
    emit(54, 5, 1, 40, 0, 2); emit(248, 2, 41);
    if (mode & LOCAL_VARIABLE) emit(59, 4, 30, 31, 7);
    if (mode & FULL_WRITE) emit(62, 3, 10, 19);
    emit(65, 5, 11, 50, 10, 14); emit(62, 3, 50, 8);
    emit(253, 1); emit(56, 1);
}
// Independent evaluation of constants and this fixture's straight-line stores.
static void values(const uint32_t *w, uint32_t nw, uint32_t variable, uint32_t output[4]) {
    uint32_t bits[256][4] = {{0}}, root[256] = {0}, component[256] = {0}; int entry = 0;
    for (unsigned i = 0; i < 4; i++) output[i] = 0xcccccccc;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at; unsigned op = x[0] & 0xffff;
        if (op == 43 && (x[0] >> 16) == 4) bits[x[2]][0] = x[3];
        if (op == 44) for (unsigned j = 0; j < 4; j++) bits[x[2]][j] = bits[x[j + 3]][0];
        if (op == 54) entry = x[2] == 40;
        if (op == 56) entry = 0;
        if (op == 65) { root[x[2]] = x[3]; component[x[2]] = bits[x[4]][0]; }
        if (op == 62 && entry) {
            if (x[1] == variable) memcpy(output, bits[x[2]], 16);
            else if (root[x[1]] == variable) { assert(component[x[1]] < 4); output[component[x[1]]] = bits[x[2]][0]; }
        }
    }
}
static uint32_t *change(unsigned mode, uint32_t mask, uint32_t *nw) {
    make_module(mode); uint32_t original[512]; memcpy(original, module, words * 4);
    uint32_t *out = NULL; *nw = words;
    assert(vk_spv_vertex_outputs(module, words, mask, &out, nw) == 1 && out);
    assert(!memcmp(original, module, words * 4)); return out;
}
int main(int argc, char **argv) {
    if (argc == 4) {
        FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
        assert(size >= 20 && !(size & 3)); uint32_t *input = malloc((size_t)size); assert(input);
        assert(fread(input, 1, (size_t)size, f) == (size_t)size); fclose(f);
        uint32_t *output = NULL, count = (uint32_t)size / 4;
        int status = vk_spv_vertex_outputs(input, count, (uint32_t)strtoul(argv[3], NULL, 16), &output, &count);
        assert(status >= 0); f = fopen(argv[2], "wb"); assert(f);
        assert(fwrite(output ? output : input, 4, count, f) == count); fclose(f);
        test_free(output); free(input); assert(!live_allocations); printf("TRANSFORM %d %u\n", status, count); return 0;
    }
    uint32_t nw, values0[4], values1[4], *out = change(LOCAL_VARIABLE, UINT32_MAX, &nw);
    values(out, nw, 10, values0);
    const uint32_t expected[] = {0x3e800000, 0, 0, 0x3f800000}; assert(!memcmp(values0, expected, 16));
    uint32_t *again = NULL, again_words = nw;
    assert(vk_spv_vertex_outputs(out, nw, UINT32_MAX, &again, &again_words) == 0 && !again && again_words == nw);
    test_free(out);
    out = change(FULL_WRITE, UINT32_MAX, &nw); values(out, nw, 10, values0);
    const uint32_t written[] = {0x3e800000, 0x40400000, 0x40800000, 0x40a00000}; assert(!memcmp(values0, written, 16)); test_free(out);
    for (unsigned second = 0; second < 2; second++) {
        out = change(EXTRA_OUTPUT | EXTRA_INTERFACE | (second ? SECOND_TYPE : 0), UINT32_MAX, &nw);
        values(out, nw, 10, values0); values(out, nw, 21, values1);
        const uint32_t defaults[] = {0, 0, 0, 0x3f800000};
        assert(!memcmp(values0, expected, 16) && !memcmp(values1, defaults, 16)); test_free(out);
    }
    out = change(EXTRA_OUTPUT | EXTRA_INTERFACE, 2, &nw); values(out, nw, 10, values0); values(out, nw, 21, values1);
    assert(values0[0] == 0x3e800000 && values0[3] == 0xcccccccc && values1[3] == 0x3f800000); test_free(out);
    const unsigned unchanged[] = {OTHER_STAGE, BUILTIN, GLOBAL_INITIALIZER};
    for (unsigned i = 0; i < sizeof unchanged / sizeof *unchanged; i++) {
        make_module(unchanged[i]); out = NULL; nw = words;
        assert(vk_spv_vertex_outputs(module, words, UINT32_MAX, &out, &nw) == 0 && !out && nw == words);
    }
    // Non-interface variables must not be introduced into main by new stores.
    out = change(EXTRA_OUTPUT, UINT32_MAX, &nw); values(out, nw, 21, values1); assert(values1[3] == 0xcccccccc); test_free(out);
    for (unsigned failure = 1; failure <= 3; failure++) {
        make_module(0); allocation_calls = 0; fail_allocation = failure; out = NULL; nw = words;
        assert(vk_spv_vertex_outputs(module, words, UINT32_MAX, &out, &nw) == -2 && !out && nw == words && !live_allocations);
    }
    fail_allocation = 0;
    make_module(0); module[5] = 0; assert(vk_spv_vertex_outputs(module, words, UINT32_MAX, &out, &nw) == -1 && !out);
    make_module(0); assert(vk_spv_vertex_outputs(module, words - 1, UINT32_MAX, &out, &nw) == -1 && !out);
    make_module(0); module[3] = UINT32_MAX; assert(vk_spv_vertex_outputs(module, words, UINT32_MAX, &out, &nw) == -1 && !out);
    assert(!live_allocations);
    puts("PASS vertex output defaults, partial/explicit writes, selected locations, interface filtering, type groups, idempotence, local variables, allocation failures and malformed inputs");
    return 0;
}
