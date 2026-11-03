// Devices without vertexPipelineStoresAndAtomics require NonWritable on
// read-only storage buffers in vertex, geometry and tessellation modules.
// The caller supplies the existing conservative write analysis; unreflected
// bindings and bindings that might be written are never decorated.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t binding, type, storage; uint8_t block, array, variable, readonly; } vk_spv_readonly_id;

// Returns added decorations, or -1 on malformed input/allocation failure.
// A disabled gate is byte-identical and does not even inspect the module.
static inline int vk_spv_nonwritable(const uint32_t *w, uint32_t nw, int vertex_stores, int stage,
                                    const uint16_t *bindings, const uint8_t *storage, const uint8_t *written,
                                    size_t nbindings, uint32_t **out, uint32_t *onw) {
    *out = NULL; *onw = nw;
    if (vertex_stores || !(stage == 0 || (stage >= 2 && stage <= 4))) return 0;
    int any = 0;
    for (size_t i = 0; i < nbindings; i++) any |= storage[i] && !written[i];
    if (!any) return 0;
    if (!w || nw < 5 || w[0] != 0x07230203 || !w[3] || (uint64_t)w[3] > (uint64_t)nw * 16 + 4096) return -1;
    vk_spv_readonly_id *ids = calloc(w[3], sizeof *ids);
    if (!ids) return -1;
    for (uint32_t i = 0; i < w[3]; i++) ids[i].binding = UINT32_MAX;
    uint32_t firsttype = 0, added = 0;
    for (uint32_t at = 5; at < nw;) {
        uint32_t n = w[at] >> 16, op = w[at] & 65535; const uint32_t *x = w + at;
        if (!n || n > nw - at) goto invalid;
        if (!firsttype && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 59)) firsttype = at;
        if (op == 71 && n >= 3) { // OpDecorate
            if (x[1] >= w[3]) goto invalid;
            if (x[2] == 24) ids[x[1]].readonly = 1;
            if (x[2] == 2 || x[2] == 3) ids[x[1]].block = (uint8_t)x[2];
            if (x[2] == 33) { if (n != 4) goto invalid; ids[x[1]].binding = x[3]; }
        } else if (op == 32 || op == 59 || op == 28 || op == 29) { // pointer, variable, descriptor arrays
            if (n < (op == 29 ? 3u : 4u)) goto invalid;
            uint32_t id = x[op == 59 ? 2 : 1];
            if (id >= w[3]) goto invalid;
            ids[id].type = x[op == 59 ? 1 : op == 32 ? 3 : 2];
            ids[id].storage = op == 59 ? x[3] : op == 32 ? x[2] : 0;
            ids[id].variable = op == 59; ids[id].array = op == 28 || op == 29;
        }
        at += n;
    }
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        uint32_t n = w[at] >> 16; const uint32_t *x = w + at;
        if ((x[0] & 65535) == 74 && n >= 3) { // OpGroupDecorate
            if (x[1] >= w[3]) goto invalid;
            for (uint32_t j = 2; j < n; j++) {
                if (x[j] >= w[3]) goto invalid;
                if (ids[x[1]].binding != UINT32_MAX) ids[x[j]].binding = ids[x[1]].binding;
                ids[x[j]].readonly |= ids[x[1]].readonly;
                if (ids[x[1]].block) ids[x[j]].block = ids[x[1]].block;
            }
        }
    }
    for (uint32_t id = 1; id < w[3]; id++) {
        vk_spv_readonly_id *v = &ids[id];
        if (!v->variable || v->readonly || !(v->storage == 2 || v->storage == 12)) continue;
        uint32_t type = v->type;
        if (type >= w[3] || ids[type].storage != v->storage) goto invalid;
        type = ids[type].type;
        for (unsigned depth = 0; type < w[3] && ids[type].array && depth < 256; depth++) type = ids[type].type;
        if (type >= w[3] || ids[type].block != (v->storage == 2 ? 3 : 2)) continue;
        int reflected = 0, writable = 0;
        for (size_t i = 0; i < nbindings; i++) if (v->binding == bindings[i] && storage[i]) { reflected = 1; writable |= written[i]; }
        if (reflected && !writable) { v->readonly = 2; added++; }
    }
    if (added) {
        if (!firsttype || (uint64_t)nw + 3ull * added > SIZE_MAX / 4 || added > (UINT32_MAX - nw) / 3) goto invalid;
        uint32_t *o = malloc(((size_t)nw + 3 * added) * 4);
        if (!o) goto invalid;
        memcpy(o, w, (size_t)firsttype * 4); uint32_t at = firsttype;
        for (uint32_t id = 1; id < w[3]; id++) if (ids[id].readonly == 2) { o[at++] = 3u << 16 | 71; o[at++] = id; o[at++] = 24; }
        memcpy(o + at, w + firsttype, (size_t)(nw - firsttype) * 4);
        *out = o; *onw = nw + 3 * added;
    }
    free(ids); return (int)added;
invalid:
    free(ids); return -1;
}
