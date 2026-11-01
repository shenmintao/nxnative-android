// Find pool-backed descriptors that translated SPIR-V can write. Older bundles
// do not reliably carry the reflection's rw flag. Unknown pointer provenance is
// conservative: every pool-backed binding is then considered writable.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_SPV_WRITES_CALLOC
#define VK_SPV_WRITES_CALLOC calloc
#endif

typedef struct { uint32_t def, binding, pointer_class; uint8_t pointer, visiting; } vk_spv_write_id;
typedef struct {
    const uint32_t *words; uint32_t count, bound;
    const uint16_t *bindings; const uint8_t *pool; size_t nbindings;
    uint8_t *written; vk_spv_write_id *ids;
} vk_spv_write_context;

// Result bits: 1 a known root, 2 an unresolved root; zero only for a cycle edge.
static inline int vk_spv_write_root(vk_spv_write_context *c, uint32_t id, unsigned depth) {
    if (!id || id >= c->bound || depth > 256) return 2;
    vk_spv_write_id *meta = &c->ids[id];
    if (meta->visiting) return 0;
    uint32_t at = meta->def;
    if (!at || at >= c->count) return 2;
    const uint32_t *w = c->words + at; uint32_t n = w[0] >> 16, op = w[0] & 0xffff;
    meta->visiting = 1;
    int result = 2;
    if (op == 59 && n >= 4) { // OpVariable
        result = 1;
        if (meta->binding != UINT32_MAX) {
            int found = 0;
            for (size_t i = 0; i < c->nbindings; i++) if (c->bindings[i] == meta->binding) {
                found = 1; if (c->pool[i]) c->written[i] = 1;
            }
            if (!found) result = 2;
        } else if (w[3] == 0 || w[3] == 2 || w[3] == 12) {
            result = 2; // descriptor storage without a reflected Binding
        }
    } else if ((op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 100 || op == 121 || op == 122 || op == 123 || op == 124) && n >= 4) {
        // Access chains, copies, OpImage and pointer casts keep their source.
        result = vk_spv_write_root(c, w[3], depth + 1);
    } else if ((op == 60 || op == 86) && n >= 5) { // OpImageTexelPointer / OpSampledImage
        result = vk_spv_write_root(c, w[3], depth + 1);
    } else if (op == 61 && n >= 4) { // OpLoad: an image retains its descriptor, a loaded pointer does not identify its pointee
        result = w[1] < c->bound && c->ids[w[1]].pointer ? 2 : vk_spv_write_root(c, w[3], depth + 1);
    } else if (op == 169 && n >= 6) { // OpSelect
        result = vk_spv_write_root(c, w[4], depth + 1) | vk_spv_write_root(c, w[5], depth + 1);
    } else if (op == 245 && n >= 5) { // OpPhi
        result = 0;
        for (uint32_t j = 3; j + 1 < n; j += 2) result |= vk_spv_write_root(c, w[j], depth + 1);
    } else if ((op == 55 || op == 57) && n >= 3 && w[1] < c->bound && c->ids[w[1]].pointer && c->ids[w[1]].pointer_class == 7) {
        result = 1; // Function-local pointer parameters/results cannot name pool storage.
    }
    meta->visiting = 0;
    return result;
}

// 1 exact roots, 2 conservative unresolved roots, 0 malformed input/allocation
// failure. Both 0 and 2 retain every pool binding as writable, never omit it.
static inline int vk_spv_buffer_writes(const uint32_t *w, uint32_t count, const uint16_t *bindings,
                                      const uint8_t *pool, size_t nbindings, uint8_t *written) {
    int any = 0;
    for (size_t i = 0; i < nbindings; i++) { written[i] = pool[i] != 0; any |= written[i]; }
    if (!w || count < 5 || w[0] != 0x07230203 || !w[3]) return 0;
    if (!any) return 1;
    // Sparse IDs are valid, but do not allocate unbounded memory to inspect a
    // tiny module. Falling back retains all potentially written bindings.
    if ((uint64_t)w[3] > (uint64_t)count * 16 + 4096) return 2;
#if SIZE_MAX < UINT64_MAX
    if (w[3] > SIZE_MAX / sizeof(vk_spv_write_id)) return 0;
#endif
    vk_spv_write_id *ids = VK_SPV_WRITES_CALLOC(w[3], sizeof *ids);
    if (!ids) return 0;
    for (uint32_t i = 0; i < w[3]; i++) ids[i].binding = UINT32_MAX;
    int valid = 1, conservative = 0;
    uint32_t glsl450 = 0;
    for (uint32_t at = 5; at < count;) {
        uint32_t n = w[at] >> 16, op = w[at] & 0xffff;
        if (!n || n > count - at) { valid = 0; break; }
        if (op == 32) { // OpTypePointer
            if (n < 4 || w[at + 1] >= w[3]) { valid = 0; break; }
            ids[w[at + 1]].pointer = 1; ids[w[at + 1]].pointer_class = w[at + 2];
        } else if (op == 71 && n >= 4 && w[at + 2] == 33) { // OpDecorate Binding
            if (w[at + 1] >= w[3]) { valid = 0; break; }
            ids[w[at + 1]].binding = w[at + 3];
        } else if (op == 11 && n >= 3) { // OpExtInstImport
            const char *name = (const char *)(w + at + 2);
            if (!memchr(name, 0, (n - 2) * 4)) { valid = 0; break; }
            if (!strcmp(name, "GLSL.std.450")) glsl450 = w[at + 1];
        }
        switch (op) {
        case 1: case 12: case 46: case 55: case 57: case 59: case 60: case 61:
        case 65: case 66: case 67: case 70: case 83: case 86: case 100:
        case 120: case 121: case 122: case 123: case 124: case 169: case 245:
            if (n < 3 || w[at + 2] >= w[3]) { valid = 0; }
            else ids[w[at + 2]].def = at;
            break;
        }
        if (!valid) break;
        at += n;
    }
    if (valid) for (uint32_t at = 5; at < count;) {
        uint32_t n = w[at] >> 16, op = w[at] & 0xffff;
        if (op == 74 && n >= 3) { // OpGroupDecorate, including Binding
            if (w[at + 1] >= w[3]) { valid = 0; break; }
            uint32_t binding = ids[w[at + 1]].binding;
            if (binding != UINT32_MAX) for (uint32_t j = 2; j < n; j++) {
                if (w[at + j] >= w[3]) { valid = 0; break; }
                ids[w[at + j]].binding = binding;
            }
        }
        at += n;
    }
    if (valid) {
        memset(written, 0, nbindings);
        vk_spv_write_context c = {w, count, w[3], bindings, pool, nbindings, written, ids};
        for (uint32_t at = 5; at < count;) {
            uint32_t n = w[at] >> 16, op = w[at] & 0xffff, target = 0;
            if (op == 62 || op == 63 || op == 64 || op == 99 || op == 228 || op == 319) {
                if (n < 2) { valid = 0; break; }
                target = w[at + 1]; // stores, copies, image writes, atomic stores/flag clears
                if (!target) { valid = 0; break; }
            } else if ((op >= 229 && op <= 242) || op == 318 || op == 5614 || op == 5615 || op == 6035) {
                if (n < 4) { valid = 0; break; }
                target = w[at + 3]; // atomic read/modify/write, including floating-point extensions
                if (!target) { valid = 0; break; }
            } else if (op == 12 && n >= 5 && w[at + 3] == glsl450 && (w[at + 4] == 35 || w[at + 4] == 51)) {
                if (n < 7) { valid = 0; break; }
                target = w[at + 6]; // GLSL Modf / Frexp write their out pointer
                if (!target) { valid = 0; break; }
            } else if (op == 259) { conservative = 1; } // kernel async copies: not a graphics-shader operation
            if (target) {
                int roots = vk_spv_write_root(&c, target, 0);
                if (!roots || (roots & 2)) conservative = 1;
            }
            at += n;
        }
    }
    if (!valid || conservative) for (size_t i = 0; i < nbindings; i++) written[i] = pool[i] != 0;
    free(ids);
    return !valid ? 0 : conservative ? 2 : 1;
}
