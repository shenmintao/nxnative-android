// NVN/Maxwell gives unwritten floating-point vertex outputs (0, 0, 0, 1).
// Initialize plain float32 vec4 Location outputs of the main vertex entry.
// Built-ins, other stages/types, non-interface variables and explicit global
// initializers keep their original behavior. The input module is never edited.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int vk_spv_vo_constant(const uint32_t *w, const uint32_t *defs,
                                     uint32_t bound, uint32_t id, uint32_t type, uint32_t bits) {
    uint32_t at = id < bound ? defs[id] : 0;
    return at && (w[at] & 0xffff) == 43 && (w[at] >> 16) == 4 && w[at + 1] == type && w[at + 3] == bits;
}
static inline int vk_spv_vo_debug(const uint32_t *w, uint32_t at, const uint32_t *defs, uint32_t bound) {
    uint32_t op = w[at] & 0xffff, n = w[at] >> 16;
    if (op == 8 || op == 317) return 1; // OpLine / OpNoLine
    if (op != 12 || n < 5 || w[at + 3] >= bound) return 0;
    uint32_t d = defs[w[at + 3]];
    return d && (w[d] & 0xffff) == 11 && (w[d] >> 16) >= 5 &&
        !memcmp(w + d + 2, "NonSemantic.", 12);
}
static inline int vk_spv_vo_default(const uint32_t *w, const uint32_t *defs, uint32_t bound, uint32_t id, uint32_t type) {
    uint32_t d = id < bound ? defs[id] : 0;
    if (!d || (w[d] & 0xffff) != 44 || (w[d] >> 16) != 7 || w[d + 1] != type) return 0;
    uint32_t v = type < bound ? defs[type] : 0;
    if (!v || (w[v] & 0xffff) != 23 || w[v + 3] != 4) return 0;
    uint32_t scalar = w[v + 2], f = scalar < bound ? defs[scalar] : 0;
    return f && (w[f] & 0xffff) == 22 && w[f + 2] == 32 &&
        vk_spv_vo_constant(w, defs, bound, w[d + 3], scalar, 0) &&
        vk_spv_vo_constant(w, defs, bound, w[d + 4], scalar, 0) &&
        vk_spv_vo_constant(w, defs, bound, w[d + 5], scalar, 0) &&
        vk_spv_vo_constant(w, defs, bound, w[d + 6], scalar, 0x3f800000);
}

// 1: changed, 0: unchanged/not applicable, -1: malformed, -2: allocation failure.
// mask names the backend's 32 NVN locations; variants can initialize only the
// locations they added. Repeated initialization of the same prefix is a no-op.
static inline int vk_spv_vertex_outputs(const uint32_t *w, uint32_t nw, uint32_t mask,
                                        uint32_t **out, uint32_t *out_words) {
    if (!out || !out_words) return -1;
    *out = NULL; *out_words = nw;
    if (!w || nw < 5 || w[0] != 0x07230203 || !w[3] ||
        (uint64_t)w[3] > (uint64_t)nw * 16 + 4096) return -1;
    if (!mask) return 0;
    uint32_t bound = w[3], fn = 0, entry = 0, ep = 0;
    uint32_t *defs = calloc(bound, sizeof *defs);
    uint8_t *info = calloc(bound, 1); // low 6 bits: Location+1; 64: interface; 128: excluded decoration
    int result = -1;
    if (!defs || !info) { result = -2; goto done; }
    for (uint32_t at = 5; at < nw;) {
        uint32_t n = w[at] >> 16, op = w[at] & 0xffff, id = 0;
        if (!n || n > nw - at) goto done;
        const uint32_t *x = w + at;
        if ((op == 22 && n != 3) || (op == 23 && n != 4) || (op == 32 && n != 4) ||
            (op == 54 && n != 5) || (op == 248 && n != 2) ||
            (op == 59 && n != 4 && n != 5) || (op == 43 && n < 4) ||
            (op == 44 && n < 4) || (op == 11 && n < 3) || (op == 15 && n < 4)) goto done;
        if (op == 22 || op == 23 || op == 32 || op == 11 || op == 248) id = x[1];
        else if (op == 43 || op == 44 || op == 54 || op == 59) id = x[2];
        if (op == 22 || op == 23 || op == 32 || op == 11 || op == 248 || op == 43 || op == 44 || op == 54 || op == 59) {
            if (!id || id >= bound || defs[id]) goto done;
            defs[id] = at;
        }
        if (op == 54 && !fn) fn = at;
        if (op == 71 && n >= 3) {
            if (!x[1] || x[1] >= bound) goto done;
            if (x[2] == 30 && n == 4 && x[3] < 32) info[x[1]] = (uint8_t)((info[x[1]] & 0xc0) | (x[3] + 1));
            if (x[2] == 11 || (x[2] == 31 && n == 4 && x[3])) info[x[1]] |= 128;
        }
        if (op == 15) {
            const char *name = (const char *)(x + 3);
            const char *end = memchr(name, 0, (size_t)(n - 3) * 4);
            if (!end || !x[2] || x[2] >= bound) goto done;
            if (x[1] == 0 && end - name == 4 && !memcmp(name, "main", 4)) {
                if (entry) goto done;
                entry = x[2]; ep = at;
            }
        }
        at += n;
    }
    if (!entry) { result = 0; goto done; }
    if (!fn || !defs[entry] || (w[defs[entry]] & 0xffff) != 54) goto done;
    {
        uint32_t n = w[ep] >> 16, start = 3;
        while (start < n && !memchr(w + ep + start, 0, 4)) start++;
        if (start == n) goto done;
        for (uint32_t j = start + 1; j < n; j++) {
            uint32_t id = w[ep + j]; if (!id || id >= bound) goto done; info[id] |= 64;
        }
    }
    struct { uint32_t variable, type, group, initialized; } targets[32];
    struct { uint32_t vector, scalar, zero, one, value, needed; } types[32];
    uint32_t ntargets = 0, ntypes = 0, locations = 0;
    memset(targets, 0, sizeof targets); memset(types, 0, sizeof types);
    for (uint32_t at = 5; at < fn; at += w[at] >> 16) {
        const uint32_t *x = w + at;
        if ((x[0] & 0xffff) != 59 || (x[0] >> 16) != 4 || x[3] != 3) continue;
        uint32_t variable = x[2], flags = info[variable], location = flags & 63;
        if (!location || (flags & 192) != 64 || !(mask >> (location - 1) & 1)) continue;
        uint32_t p = x[1] < bound ? defs[x[1]] : 0;
        if (!p || (w[p] & 0xffff) != 32 || w[p + 2] != 3) continue;
        uint32_t type = w[p + 3], v = type < bound ? defs[type] : 0;
        if (!v || (w[v] & 0xffff) != 23 || w[v + 3] != 4) continue;
        uint32_t scalar = w[v + 2], f = scalar < bound ? defs[scalar] : 0;
        if (!f || (w[f] & 0xffff) != 22 || w[f + 2] != 32) continue;
        if (ntargets == 32 || (locations >> (location - 1) & 1)) goto done;
        locations |= 1u << (location - 1);
        uint32_t group = 0;
        while (group < ntypes && types[group].vector != type) group++;
        if (group == ntypes) { types[group].vector = type; types[group].scalar = scalar; ntypes++; }
        targets[ntargets].variable = variable; targets[ntargets].type = type; targets[ntargets++].group = group;
    }
    if (!ntargets) { result = 0; goto done; }
    uint32_t at = defs[entry] + (w[defs[entry]] >> 16);
    while (at < nw && (w[at] & 0xffff) == 55) at += w[at] >> 16; // parameters
    if (at >= nw || (w[at] & 0xffff) != 248) goto done;
    at += w[at] >> 16;
    while (at < nw && (((w[at] & 0xffff) == 59 && w[at + 3] == 7) || vk_spv_vo_debug(w, at, defs, bound))) at += w[at] >> 16;
    uint32_t insert = at;
    if (insert >= nw) goto done;
    // No Function variable may follow the first executable instruction.
    uint32_t end = insert;
    for (; end < nw && (w[end] & 0xffff) != 56; end += w[end] >> 16)
        if ((w[end] & 0xffff) == 59 || (w[end] & 0xffff) == 54) goto done;
    if (end >= nw || (w[end] >> 16) != 1) goto done;
    for (uint32_t j = insert; j < nw;) {
        if (vk_spv_vo_debug(w, j, defs, bound)) { j += w[j] >> 16; continue; }
        if ((w[j] & 0xffff) != 62 || (w[j] >> 16) != 3) break;
        uint32_t target = 0;
        while (target < ntargets && targets[target].variable != w[j + 1]) target++;
        if (target == ntargets || !vk_spv_vo_default(w, defs, bound, w[j + 2], targets[target].type)) break;
        targets[target].initialized = 1; j += 3;
    }
    uint32_t stores = 0;
    for (uint32_t i = 0; i < ntargets; i++) if (!targets[i].initialized) { stores++; types[targets[i].group].needed = 1; }
    if (!stores) { result = 0; goto done; }
    for (uint32_t j = 5; j < fn; j += w[j] >> 16) {
        uint32_t n = w[j] >> 16, op = w[j] & 0xffff;
        for (uint32_t g = 0; g < ntypes; g++) if (types[g].needed) {
            if (op == 43 && n == 4 && w[j + 1] == types[g].scalar) {
                if (w[j + 3] == 0) types[g].zero = w[j + 2];
                if (w[j + 3] == 0x3f800000) types[g].one = w[j + 2];
            }
            if (op == 44 && n == 7 && w[j + 1] == types[g].vector &&
                vk_spv_vo_default(w, defs, bound, w[j + 2], types[g].vector)) types[g].value = w[j + 2];
        }
    }
    if (bound > UINT32_MAX - 3 * ntypes) goto done;
    uint32_t declarations[32 * 15], nd = 0, next = bound;
    for (uint32_t g = 0; g < ntypes; g++) if (types[g].needed && !types[g].value) {
        for (uint32_t other = 0; other < g; other++) if (types[other].scalar == types[g].scalar) {
            if (!types[g].zero) types[g].zero = types[other].zero;
            if (!types[g].one) types[g].one = types[other].one;
        }
        if (!types[g].zero) {
            types[g].zero = next++;
            declarations[nd++] = 4u << 16 | 43; declarations[nd++] = types[g].scalar;
            declarations[nd++] = types[g].zero; declarations[nd++] = 0;
        }
        if (!types[g].one) {
            types[g].one = next++;
            declarations[nd++] = 4u << 16 | 43; declarations[nd++] = types[g].scalar;
            declarations[nd++] = types[g].one; declarations[nd++] = 0x3f800000;
        }
        types[g].value = next++;
        declarations[nd++] = 7u << 16 | 44; declarations[nd++] = types[g].vector; declarations[nd++] = types[g].value;
        declarations[nd++] = types[g].zero; declarations[nd++] = types[g].zero; declarations[nd++] = types[g].zero; declarations[nd++] = types[g].one;
    }
    uint64_t total = (uint64_t)nw + nd + 3 * stores;
    if (total > UINT32_MAX || total > SIZE_MAX / sizeof(uint32_t)) goto done;
    uint32_t *patched = malloc((size_t)total * sizeof *patched), used = fn;
    if (!patched) { result = -2; goto done; }
    memcpy(patched, w, (size_t)fn * 4); patched[3] = next;
    memcpy(patched + used, declarations, (size_t)nd * 4); used += nd;
    memcpy(patched + used, w + fn, (size_t)(insert - fn) * 4); used += insert - fn;
    for (uint32_t i = 0; i < ntargets; i++) if (!targets[i].initialized) {
        patched[used++] = 3u << 16 | 62;
        patched[used++] = targets[i].variable;
        patched[used++] = types[targets[i].group].value;
    }
    memcpy(patched + used, w + insert, (size_t)(nw - insert) * 4); used += nw - insert;
    *out = patched; *out_words = used; result = 1;
done:
    free(defs); free(info); return result;
}
