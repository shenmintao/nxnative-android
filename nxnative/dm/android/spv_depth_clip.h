// GL's -w..w clip depth on Vulkan devices without EXT_depth_clip_control.
// Same coordinate convention as nxnative dmsh_build.py vs_depth (07ca3efc).
// Run on the last pre-rasterization stage, after the other module rewrites.
#ifndef DM_ANDROID_SPV_DEPTH_CLIP_H
#define DM_ANDROID_SPV_DEPTH_CLIP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned positions, captured_positions; } vk_spv_depth_info;
static inline int vk_spv_dc_dec(const uint32_t *w, uint32_t end, uint32_t id, uint32_t member, uint32_t dec, uint32_t *value) {
    for (uint32_t i = 5; i < end; i += w[i] >> 16) {
        const uint32_t *x = w + i; uint32_t n = x[0] >> 16, op = x[0] & 65535;
        if (member == UINT32_MAX && op == 71 && n == 4 && x[1] == id && x[2] == dec) { *value = x[3]; return 1; }
        if (member != UINT32_MAX && op == 72 && n == 5 && x[1] == id && x[2] == member && x[3] == dec) { *value = x[4]; return 1; }
    }
    return 0;
}
// Conservatively reserve every occupied output location, including arrays and
// matrices, before choosing a location for an XFB-only Position shadow.
static inline uint32_t vk_spv_dc_slots(const uint32_t *w, const uint32_t *defs, uint32_t bound, uint32_t type, unsigned depth) {
    if (!type || type >= bound || !defs[type] || depth > 12) return 33;
    const uint32_t *x = w + defs[type]; uint32_t op = x[0] & 65535;
    if (op == 21 || op == 22) return 1;
    if (op == 23) {
        uint32_t s = x[2] < bound ? defs[x[2]] : 0;
        return s && x[3] <= 4 ? (w[s + 2] * x[3] + 127) / 128 : 33;
    }
    if (op == 24 || op == 28) {
        uint32_t count = x[3];
        if (op == 28) { uint32_t c = count < bound ? defs[count] : 0; if (!c || (w[c] & 65535) != 43 || w[c] >> 16 != 4) return 33; count = w[c + 3]; }
        uint32_t slots = vk_spv_dc_slots(w, defs, bound, x[2], depth + 1);
        return count <= 32 && slots <= 32 && count * slots <= 32 ? count * slots : 33;
    }
    if (op == 30) { uint32_t count = 0; for (uint32_t j = 2; j < x[0] >> 16; j++) { count += vk_spv_dc_slots(w, defs, bound, x[j], depth + 1); if (count > 32) return 33; } return count; }
    return 33;
}
typedef struct { uint32_t w[512], n; } vk_spv_dc_words;
#define VK_DC_ADD(b, ...) do { const uint32_t vk_dc_emit[] = {__VA_ARGS__}; memcpy((b).w + (b).n, vk_dc_emit, sizeof vk_dc_emit); (b).n += sizeof vk_dc_emit / 4; } while (0)

// 1 patched; 0 disabled/not applicable; -1 unsupported/malformed; -2 no memory;
// -3 no spare output location for lossless transform-feedback capture.
static inline int vk_spv_depth_clip(const uint32_t *w, uint32_t nw, int enabled, int stage,
                                    uint32_t **out, uint32_t *out_words, vk_spv_depth_info *info) {
    if (!out || !out_words) return -1;
    *out = NULL; *out_words = nw; if (info) memset(info, 0, sizeof *info);
    if (!enabled || !(stage == 0 || stage == 2)) return 0; // NVN vertex / geometry; tessellation is not in this path
    if (!w || nw < 5 || w[0] != 0x07230203 || !w[3] || (uint64_t)w[3] > (uint64_t)nw * 16 + 4096) return -1;
    uint32_t bound = w[3], fn = nw, annot = nw, entry = 0, ep = 0, inttype = 0;
    uint32_t *defs = calloc(bound, sizeof *defs); if (!defs) return -2;
    int result = -1, xfb = 0; uint32_t events = 0;
    for (uint32_t at = 5; at < nw;) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535, id = 0;
        if (!n || n > nw - at) goto done;
        if (op >= 19 && op <= 39) { if (n < 2) goto done; id = x[1]; if (annot == nw) annot = at; }
        if (op == 43 || op == 59 || op == 54) { if (n < 3) goto done; id = x[2]; }
        if (id) { if (id >= bound || defs[id]) goto done; defs[id] = at; }
        if (op == 21 && n == 4 && x[2] == 32) inttype = x[1];
        if (op == 54 && fn == nw) fn = at;
        if (op == 15 && n >= 5 && x[1] == (stage == 0 ? 0u : 3u)) {
            const char *s = (const char *)(x + 3), *end = memchr(s, 0, (size_t)(n - 3) * 4);
            if (!end) goto done;
            if (end - s == 4 && !memcmp(s, "main", 4)) { if (entry) goto done; entry = x[2]; ep = at; }
        }
        if (op == 16 && n >= 3 && x[2] == 11) xfb = 1;
        at += n;
    }
    if (!entry) { result = 0; goto done; }
    if (fn == nw || annot == nw || entry >= bound || !defs[entry]) goto done;
    uint32_t interfaces = 3; while (interfaces < w[ep] >> 16 && !memchr(w + ep + interfaces, 0, 4)) interfaces++;
    if (interfaces == w[ep] >> 16) goto done; interfaces++;
    struct { uint32_t var, block, member, vec, scalar, ptr, index, half, shadow, location, buffer, stride, offset, stream; int capture, clear_var; } t[8];
    memset(t, 0, sizeof t); unsigned nt = 0, captures = 0; uint32_t occupied = 0;
    for (uint32_t at = 5; at < fn; at += w[at] >> 16) {
        const uint32_t *x = w + at; if ((x[0] & 65535) != 59 || x[0] >> 16 < 4 || x[3] != 3) continue;
        uint32_t p = x[1] < bound ? defs[x[1]] : 0; if (!p || (w[p] & 65535) != 32 || w[p] >> 16 != 4) goto done;
        uint32_t type = w[p + 3], d = type < bound ? defs[type] : 0; if (!d) goto done;
        uint32_t loc;
        if (vk_spv_dc_dec(w, fn, x[2], UINT32_MAX, 30, &loc)) {
            uint32_t slots = vk_spv_dc_slots(w, defs, bound, type, 0);
            if (loc >= 32 || slots > 32 - loc) occupied = UINT32_MAX;
            else for (uint32_t q = 0; q < slots; q++) occupied |= 1u << (loc + q);
        } else if ((w[d] & 65535) == 30) {
            for (uint32_t m = 0; m + 2 < w[d] >> 16; m++) if (vk_spv_dc_dec(w, fn, type, m, 30, &loc)) {
                uint32_t slots = vk_spv_dc_slots(w, defs, bound, w[d + 2 + m], 0);
                if (loc >= 32 || slots > 32 - loc) occupied = UINT32_MAX;
                else for (uint32_t q = 0; q < slots; q++) occupied |= 1u << (loc + q);
            }
        }
        int present = 0; for (uint32_t j = interfaces; j < w[ep] >> 16; j++) present |= w[ep + j] == x[2];
        if (!present) continue;
        uint32_t member = UINT32_MAX, builtin;
        if (vk_spv_dc_dec(w, fn, x[2], UINT32_MAX, 11, &builtin) && builtin == 0) { }
        else if ((w[d] & 65535) == 30) {
            for (uint32_t m = 0; m + 2 < w[d] >> 16; m++) if (vk_spv_dc_dec(w, fn, type, m, 11, &builtin) && builtin == 0) { member = m; break; }
            if (member == UINT32_MAX) continue;
        } else continue;
        uint32_t vec = member == UINT32_MAX ? type : w[d + 2 + member], v = vec < bound ? defs[vec] : 0;
        if (!v || (w[v] & 65535) != 23 || w[v] >> 16 != 4 || w[v + 3] != 4 || nt == 8) goto done;
        uint32_t scalar = w[v + 2], f = scalar < bound ? defs[scalar] : 0;
        if (!f || (w[f] & 65535) != 22 || w[f] >> 16 != 3 || w[f + 2] != 32) goto done;
        t[nt].var = x[2]; t[nt].block = type; t[nt].member = member; t[nt].vec = vec; t[nt].scalar = scalar;
        uint32_t owner = member == UINT32_MAX ? x[2] : type;
        if (xfb && vk_spv_dc_dec(w, fn, owner, member, 35, &t[nt].offset)) {
            if (!vk_spv_dc_dec(w, fn, x[2], UINT32_MAX, 36, &t[nt].buffer) || !vk_spv_dc_dec(w, fn, x[2], UINT32_MAX, 37, &t[nt].stride)) goto done;
            t[nt].capture = t[nt].clear_var = 1; captures++;
            if (member != UINT32_MAX) for (uint32_t m = 0; m + 2 < w[d] >> 16; m++) {
                uint32_t off; if (m != member && vk_spv_dc_dec(w, fn, type, m, 35, &off)) t[nt].clear_var = 0;
            }
        }
        if (!vk_spv_dc_dec(w, fn, owner, member, 29, &t[nt].stream)) vk_spv_dc_dec(w, fn, x[2], UINT32_MAX, 29, &t[nt].stream);
        nt++;
    }
    if (!nt) { result = 0; goto done; }
    int in_entry = 0;
    for (uint32_t at = fn; at < nw; at += w[at] >> 16) {
        uint32_t op = w[at] & 65535;
        if (op == 54) in_entry = w[at + 2] == entry; else if (op == 56) in_entry = 0;
        if (stage == 0 ? op == 253 && in_entry : op == 218 || op == 220) events++;
    }
    if (!events) { result = 0; goto done; }
    if ((uint64_t)bound + (uint64_t)events * nt * 8 + 64 > UINT32_MAX) goto done;
    uint32_t next = bound; vk_spv_dc_words a = {0}, decl = {0};
    if (!inttype) { inttype = next++; VK_DC_ADD(decl, 4u << 16 | 21, inttype, 32, 0); }
    for (unsigned j = 0; j < nt; j++) {
        for (uint32_t at = 5; at < fn; at += w[at] >> 16) {
            const uint32_t *x = w + at; uint32_t op = x[0] & 65535, n = x[0] >> 16;
            if (op == 32 && n == 4 && x[2] == 3 && x[3] == t[j].vec) t[j].ptr = x[1];
            if (op == 43 && n == 4 && x[1] == t[j].scalar && x[3] == 0x3f000000) t[j].half = x[2];
        }
        if (!t[j].ptr) { t[j].ptr = next++; VK_DC_ADD(decl, 4u << 16 | 32, t[j].ptr, 3, t[j].vec); }
        if (t[j].member != UINT32_MAX) { t[j].index = next++; VK_DC_ADD(decl, 4u << 16 | 43, inttype, t[j].index, t[j].member); }
        if (!t[j].half) { t[j].half = next++; VK_DC_ADD(decl, 4u << 16 | 43, t[j].scalar, t[j].half, 0x3f000000); }
        if (t[j].capture) {
            uint32_t l = 0; while (l < 32 && (occupied >> l & 1)) l++;
            if (l == 32) { result = -3; goto done; } occupied |= 1u << l;
            t[j].location = l; t[j].shadow = next++;
            VK_DC_ADD(a, 4u << 16 | 71, t[j].shadow, 30, l);
            VK_DC_ADD(a, 4u << 16 | 71, t[j].shadow, 36, t[j].buffer);
            VK_DC_ADD(a, 4u << 16 | 71, t[j].shadow, 37, t[j].stride);
            VK_DC_ADD(a, 4u << 16 | 71, t[j].shadow, 35, t[j].offset);
            if (stage == 2 && t[j].stream) VK_DC_ADD(a, 4u << 16 | 71, t[j].shadow, 29, t[j].stream);
            VK_DC_ADD(decl, 4u << 16 | 59, t[j].ptr, t[j].shadow, 3);
        }
    }
    uint64_t capacity = (uint64_t)nw + a.n + decl.n + captures + (uint64_t)events * nt * 48;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / 4) goto done;
    uint32_t *o = malloc((size_t)capacity * 4); if (!o) { result = -2; goto done; }
    uint32_t used = 5; memcpy(o, w, 20); in_entry = 0;
#define VK_DC_OUT(...) do { const uint32_t vk_dc_emit[] = {__VA_ARGS__}; memcpy(o + used, vk_dc_emit, sizeof vk_dc_emit); used += sizeof vk_dc_emit / 4; } while (0)
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        const uint32_t *x = w + at; uint32_t n = x[0] >> 16, op = x[0] & 65535;
        if (at == annot) { memcpy(o + used, a.w, a.n * 4); used += a.n; }
        if (at == fn) { memcpy(o + used, decl.w, decl.n * 4); used += decl.n; }
        int drop = 0;
        for (unsigned j = 0; j < nt; j++) if (t[j].capture) {
            if (op == 71 && n == 4 && x[1] == t[j].var && ((t[j].member == UINT32_MAX && x[2] == 35) || (t[j].clear_var && (x[2] == 36 || x[2] == 37)))) drop = 1;
            if (op == 72 && n == 5 && x[1] == t[j].block && x[2] == t[j].member && x[3] == 35) drop = 1;
        }
        if (drop) continue;
        if (at == ep) {
            o[used++] = (n + captures) << 16 | 15; memcpy(o + used, x + 1, (n - 1) * 4); used += n - 1;
            for (unsigned j = 0; j < nt; j++) if (t[j].capture) o[used++] = t[j].shadow;
            continue;
        }
        if (op == 54) in_entry = x[2] == entry; else if (op == 56) in_entry = 0;
        int transform = stage == 0 ? op == 253 && in_entry : op == 218 || op == 220;
        uint32_t saved[8] = {0}, pointers[8] = {0};
        if (transform) for (unsigned j = 0; j < nt; j++) {
            uint32_t ptr = t[j].var;
            if (t[j].member != UINT32_MAX) { ptr = next++; VK_DC_OUT(5u << 16 | 65, t[j].ptr, ptr, t[j].var, t[j].index); }
            uint32_t original = next++, z = next++, v = next++, sum = next++, depth = next++, position = next++;
            saved[j] = original; pointers[j] = ptr;
            VK_DC_OUT(4u << 16 | 61, t[j].vec, original, ptr);
            if (t[j].capture) VK_DC_OUT(3u << 16 | 62, t[j].shadow, original);
            VK_DC_OUT(5u << 16 | 81, t[j].scalar, z, original, 2);
            VK_DC_OUT(5u << 16 | 81, t[j].scalar, v, original, 3);
            VK_DC_OUT(5u << 16 | 129, t[j].scalar, sum, z, v);
            VK_DC_OUT(5u << 16 | 133, t[j].scalar, depth, sum, t[j].half);
            VK_DC_OUT(6u << 16 | 82, t[j].vec, position, depth, original, 2);
            VK_DC_OUT(3u << 16 | 62, ptr, position);
        }
        memcpy(o + used, x, n * 4); used += n;
        // EmitVertex makes outputs undefined. Restore the guest Position anyway:
        // a later emission or helper must not observe our coordinate conversion.
        if (transform && stage == 2) for (unsigned j = 0; j < nt; j++) VK_DC_OUT(3u << 16 | 62, pointers[j], saved[j]);
    }
#undef VK_DC_OUT
    o[3] = next; *out = o; *out_words = used;
    if (info) { info->positions = nt; info->captured_positions = captures; }
    result = 1;
done:
    free(defs); return result;
}
#undef VK_DC_ADD
#endif
