// Maxwell vertex fetch produces 32-bit register bits independently of the
// shader's declared interpretation. Vulkan requires matching numeric classes.
// Retype the Input pointers and convert each load back to its original type.
// Signature: three bits per location, 0 unchanged, 1 float, 2 uint, 3 sint,
// 4 uint32-to-float and 5 sint32-to-float (NVN's 32-bit UI2F/I2F formats).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint32_t *w;
    uint32_t nw, bound, next, *defs, *code, *ptr;
    uint32_t scalar[4], value[4][5], pointer[4][5];
    uint32_t types[256], nt;
} vk_spv_vin_context;

static inline int vk_spv_vin_shape(vk_spv_vin_context *c, uint32_t id, uint32_t *kind, uint32_t *components) {
    if (id >= c->bound || !c->defs[id]) return 0;
    const uint32_t *x = c->w + c->defs[id];
    *components = 1;
    if ((x[0] & 65535) == 23 && x[0] >> 16 == 4) {
        *components = x[3]; id = x[2];
        if (!*components || *components > 4 || id >= c->bound || !c->defs[id]) return 0;
        x = c->w + c->defs[id];
    }
    if ((x[0] & 65535) == 22 && x[0] >> 16 == 3 && x[2] == 32) *kind = 1;
    else if ((x[0] & 65535) == 21 && x[0] >> 16 == 4 && x[2] == 32) *kind = x[3] ? 3 : 2;
    else return 0;
    return 1;
}
static inline uint32_t vk_spv_vin_value(vk_spv_vin_context *c, uint32_t kind, uint32_t components) {
    if (c->value[kind][components]) return c->value[kind][components];
    if (!c->scalar[kind]) {
        for (uint32_t id = 1; id < c->bound; id++) if (c->defs[id]) {
            const uint32_t *x = c->w + c->defs[id]; uint32_t op = x[0] & 65535;
            if ((kind == 1 && op == 22 && x[0] >> 16 == 3 && x[2] == 32) ||
                (kind != 1 && op == 21 && x[0] >> 16 == 4 && x[2] == 32 && !!x[3] == (kind == 3))) { c->scalar[kind] = id; break; }
        }
        if (!c->scalar[kind]) {
            uint32_t id = c->next++; c->scalar[kind] = id;
            c->types[c->nt++] = (kind == 1 ? 3u : 4u) << 16 | (kind == 1 ? 22u : 21u);
            c->types[c->nt++] = id; c->types[c->nt++] = 32;
            if (kind != 1) c->types[c->nt++] = kind == 3;
        }
    }
    if (components == 1) return c->value[kind][components] = c->scalar[kind];
    for (uint32_t id = 1; id < c->bound; id++) if (c->defs[id]) {
        const uint32_t *x = c->w + c->defs[id];
        if ((x[0] & 65535) == 23 && x[0] >> 16 == 4 && x[2] == c->scalar[kind] && x[3] == components)
            return c->value[kind][components] = id;
    }
    uint32_t id = c->next++; c->value[kind][components] = id;
    c->types[c->nt++] = 4u << 16 | 23u; c->types[c->nt++] = id;
    c->types[c->nt++] = c->scalar[kind]; c->types[c->nt++] = components;
    return id;
}
static inline uint32_t vk_spv_vin_pointer(vk_spv_vin_context *c, uint32_t type, uint32_t code) {
    if (type >= c->bound || !c->defs[type]) return 0;
    const uint32_t *x = c->w + c->defs[type]; uint32_t oldkind, components;
    if ((x[0] & 65535) != 32 || x[0] >> 16 != 4 || x[2] != 1 || !vk_spv_vin_shape(c, x[3], &oldkind, &components)) return 0;
    uint32_t kind = code == 4 ? 2 : code == 5 ? 3 : code;
    if (!kind || kind > 3) return 0;
    if (c->pointer[kind][components]) return c->pointer[kind][components];
    uint32_t value = vk_spv_vin_value(c, kind, components);
    for (uint32_t id = 1; id < c->bound; id++) if (c->defs[id]) {
        x = c->w + c->defs[id];
        if ((x[0] & 65535) == 32 && x[0] >> 16 == 4 && x[2] == 1 && x[3] == value)
            return c->pointer[kind][components] = id;
    }
    uint32_t id = c->next++; c->pointer[kind][components] = id;
    c->types[c->nt++] = 4u << 16 | 32u; c->types[c->nt++] = id;
    c->types[c->nt++] = 1; c->types[c->nt++] = value;
    return id;
}
static inline int vk_spv_vin_alias(uint32_t op) { return op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 169 || op == 245; }

// 1: rewritten, 0: unchanged, -1: unsupported/malformed, -2: allocation failed.
// Unknown pointer uses fail explicitly; they never silently retain an invalid
// vertex interface. Names, locations, descriptor bindings and value IDs stay.
static inline int vk_spv_vertex_inputs(const uint32_t *w, uint32_t nw, uint64_t signature, uint32_t **result, uint32_t *out_words) {
    *result = NULL; *out_words = nw;
    if (!signature) return 0;
    if (!w || nw < 5 || w[0] != 0x07230203 || !w[3] || (uint64_t)w[3] > (uint64_t)nw * 16 + 4096 ||
        nw > (UINT32_MAX - 512) / 4 || w[3] > UINT32_MAX - 4 * nw - 512) return -1;
    vk_spv_vin_context c = {.w = w, .nw = nw, .bound = w[3], .next = w[3]};
    uint32_t *locations = malloc((size_t)c.bound * sizeof *locations), *out = NULL;
    c.defs = calloc(c.bound, sizeof *c.defs); c.code = calloc(c.bound, sizeof *c.code); c.ptr = calloc(c.bound, sizeof *c.ptr);
    int status = -2;
    if (!locations || !c.defs || !c.code || !c.ptr) goto done;
    memset(locations, 255, (size_t)c.bound * sizeof *locations); status = -1;
    for (uint32_t at = 5; at < nw;) {
        uint32_t n = w[at] >> 16, op = w[at] & 65535; const uint32_t *x = w + at;
        if (!n || n > nw - at) goto done;
        if (op == 21 || op == 22 || op == 23 || op == 32) {
            if (n < 2 || !x[1] || x[1] >= c.bound) goto done;
            c.defs[x[1]] = at;
        }
        if (op == 71 && n == 4 && x[2] == 30) { if (x[1] >= c.bound) goto done; locations[x[1]] = x[3]; }
        if (op == 59 || vk_spv_vin_alias(op)) { if (n < 4 || !x[2] || x[2] >= c.bound) goto done; }
        at += n;
    }
    uint32_t changed = 0;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16;
        if ((x[0] & 65535) != 59 || n < 4 || x[3] != 1 || locations[x[2]] >= 16) continue;
        uint32_t code = (uint32_t)(signature >> (3 * locations[x[2]])) & 7;
        if (!code) continue;
        uint32_t type = x[1], kind, components;
        if (code > 5 || n != 4 || type >= c.bound || !c.defs[type]) goto done;
        const uint32_t *pointer = w + c.defs[type];
        if ((pointer[0] & 65535) != 32 || pointer[0] >> 16 != 4 || pointer[2] != 1 || !vk_spv_vin_shape(&c, pointer[3], &kind, &components)) goto done;
        if (code < 4 && code == kind) continue;
        c.code[x[2]] = code; c.ptr[x[2]] = vk_spv_vin_pointer(&c, type, code);
        if (!c.ptr[x[2]]) goto done;
        changed++;
    }
    if (!changed) { status = 0; goto done; }
    // Access chains and pointer copies can precede a back edge's phi source.
    for (;;) {
        uint32_t progress = 0;
        for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
            const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535, code = 0;
            if (!vk_spv_vin_alias(op)) continue;
            if (op == 169) { if (n != 6) goto done; if (x[4] < c.bound) code = c.code[x[4]]; if (!code && x[5] < c.bound) code = c.code[x[5]]; }
            else if (op == 245) { if (n < 5 || !(n & 1)) goto done; for (uint32_t j = 3; j < n && !code; j += 2) if (x[j] < c.bound) code = c.code[x[j]]; }
            else { if (n < 4) goto done; if (x[3] < c.bound) code = c.code[x[3]]; }
            if (!code) continue;
            if (c.code[x[2]]) { if (c.code[x[2]] != code) goto done; continue; }
            c.code[x[2]] = code; c.ptr[x[2]] = vk_spv_vin_pointer(&c, x[1], code);
            if (!c.ptr[x[2]]) goto done;
            progress++;
        }
        if (!progress) break;
    }
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535;
        if (vk_spv_vin_alias(op) && c.code[x[2]]) {
            uint32_t code = c.code[x[2]];
            if (op == 169) { if (x[4] >= c.bound || x[5] >= c.bound || c.code[x[4]] != code || c.code[x[5]] != code) goto done; }
            else if (op == 245) { for (uint32_t j = 3; j < n; j += 2) if (x[j] >= c.bound || c.code[x[j]] != code) goto done; }
            else if (x[3] >= c.bound || c.code[x[3]] != code) goto done;
        }
        // These core instructions can consume a pointer outside supported
        // access/copy/load operations. Extensions are checked conservatively.
        uint32_t first = 0, end = 0;
        if (op == 57) { first = 4; end = n; }
        else if (op == 62 || op == 63) { first = 1; end = n < 3 ? n : 3; }
        else if (op == 64) { first = 1; end = n < 4 ? n : 4; }
        else if (op == 254) { first = 1; end = n; }
        else if (op == 117 || op == 121 || op == 122 || op == 123 || op == 124 || op == 68 || (op >= 401 && op <= 403)) { first = 3; end = n; }
        else if (op == 12) { first = 5; end = n; }
        else if (op >= 4096) { first = 1; end = n; }
        for (uint32_t j = first; j < end; j++) if (x[j] < c.bound && c.code[x[j]]) goto done;
    }
    out = malloc(((size_t)nw * 3 + 512) * sizeof *out);
    if (!out) { status = -2; goto done; }
    memcpy(out, w, 5 * sizeof *out); uint32_t count = 5, hole = 0;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535;
        if (op == 59 && c.code[x[2]]) continue; // changed Inputs follow the new type declarations
        if (op == 54 && !hole) { hole = count; count += 512; }
        if (op == 61 && n >= 4 && x[3] < c.bound && c.code[x[3]]) {
            uint32_t code = c.code[x[3]], oldkind, components;
            if (!vk_spv_vin_shape(&c, x[1], &oldkind, &components)) goto done;
            uint32_t actual = code == 4 ? 2 : code == 5 ? 3 : code;
            uint32_t value = vk_spv_vin_value(&c, actual, components), temporary = c.next++;
            memcpy(out + count, x, (size_t)n * sizeof *out); out[count + 1] = value; out[count + 2] = temporary; count += n;
            if (code >= 4) {
                uint32_t float_type = vk_spv_vin_value(&c, 1, components), converted = oldkind == 1 ? x[2] : c.next++;
                out[count++] = 4u << 16 | (code == 4 ? 112u : 111u); // OpConvertUToF / OpConvertSToF
                out[count++] = float_type; out[count++] = converted; out[count++] = temporary;
                temporary = converted;
                if (oldkind == 1) continue;
            }
            out[count++] = 4u << 16 | 124u; out[count++] = x[1]; out[count++] = x[2]; out[count++] = temporary;
        } else {
            memcpy(out + count, x, (size_t)n * sizeof *out);
            if (vk_spv_vin_alias(op) && c.code[x[2]]) out[count + 1] = c.ptr[x[2]];
            count += n;
        }
    }
    if (!hole || c.nt + changed * 4 > 512) goto done;
    memcpy(out + hole, c.types, c.nt * sizeof *out); uint32_t written = c.nt;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at;
        if ((x[0] & 65535) != 59 || !c.code[x[2]]) continue;
        memcpy(out + hole + written, x, 4 * sizeof *out); out[hole + written + 1] = c.ptr[x[2]]; written += 4;
    }
    memmove(out + hole + written, out + hole + 512, (size_t)(count - hole - 512) * sizeof *out);
    count -= 512 - written; out[3] = c.next;
    *result = out; *out_words = count; out = NULL; status = 1;
done:
    free(out); free(locations); free(c.defs); free(c.code); free(c.ptr); return status;
}
