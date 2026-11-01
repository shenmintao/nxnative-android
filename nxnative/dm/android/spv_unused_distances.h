// Remove unused trailing ClipDistance/CullDistance members of an Output block.
// The remaining member indices and all executable instructions stay unchanged.
// Whole-block accesses, pointer aliases, shared non-Output types and unknown
// access-chain indices conservatively keep the original interface.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t vk_spv_result_id(const uint32_t *x) {
    uint32_t op = x[0] & 0xffff, n = x[0] >> 16;
    if ((op == 30 || op == 32) && n >= 2) return x[1];
    if ((op == 43 || op == 59) && n >= 3) return x[2];
    return 0;
}
static inline int vk_spv_unused_distance(const uint32_t *w, uint32_t nw, const uint32_t *defs,
                                        uint32_t structure, uint32_t member) {
    uint32_t variables[32], nvariables = 0;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        uint32_t n = w[at] >> 16, op = w[at] & 0xffff; const uint32_t *x = w + at;
        // Do not alter a type shared with an input block, array, local object,
        // composite constant or a function interface.
        if (op == 32 && n >= 4 && x[3] == structure && x[2] != 3) return 0;
        if ((op == 28 || op == 29) && n >= 3 && x[2] == structure) return 0;
        if (op == 30 && x[1] != structure) for (uint32_t j = 2; j < n; j++) if (x[j] == structure) return 0;
        if ((op == 44 || op == 45 || op == 46 || op == 48 || op == 49 || op == 50 || op == 51 || op == 52 || op == 54 || op == 55 || op == 57) && n >= 2 && x[1] == structure) return 0;
        if (op == 33) for (uint32_t j = 2; j < n; j++) if (x[j] == structure) return 0;
        if (op == 75) for (uint32_t j = 2; j + 1 < n; j += 2) if (x[j] == structure) return 0;
        if (op == 72 && n >= 4 && x[1] == structure && x[2] == member && (x[3] == 35 || x[3] == 36 || x[3] == 37)) return 0;
        if (op == 59 && n >= 4 && x[1] < w[3]) {
            uint32_t d = defs[x[1]];
            if (d && (w[d] & 0xffff) == 32 && w[d + 3] == structure) {
                if (x[3] != 3 || nvariables == 32) return 0;
                variables[nvariables++] = x[2];
            }
        }
    }
    if (!nvariables) return 0;
    for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
        uint32_t n = w[at] >> 16, op = w[at] & 0xffff; const uint32_t *x = w + at;
        if (op == 5 || op == 6 || op == 15 || op == 16 || op == 17 || op == 59 || op == 71 || op == 72) continue; // names, interfaces, execution modes, capabilities, declarations, decorations
        for (uint32_t v = 0; v < nvariables; v++) {
            uint32_t variable = variables[v];
            if ((op == 65 || op == 66) && n >= 5 && x[3] == variable) {
                uint32_t c = x[4] < w[3] ? defs[x[4]] : 0;
                if (!c || (w[c] & 0xffff) != 43 || (w[c] >> 16) != 4 || w[c + 3] >= member) return 0;
                continue;
            }
            // Ignore literal fields, including a GLSL instruction number that
            // happens to equal the output variable's numeric ID.
            uint32_t end = n;
            if ((op == 21 || op == 22) && end > 2) end = 2; // scalar widths and signedness are literals
            if ((op == 43 || op == 50) && end > 3) end = 3; // scalar constant values are literals
            if (op == 61 && end > 4) end = 4; // memory operands
            if ((op == 62 || op == 63) && end > 3) end = 3;
            if (op == 64 && end > 4) end = 4;
            if (op == 79 && end > 5) end = 5; // shuffle component literals
            if (op == 81 && end > 4) end = 4;
            if (op == 82 && end > 5) end = 5;
            if (op == 246 && end > 3) end = 3; // merge labels, followed by literal controls
            if (op == 247 && end > 2) end = 2;
            if (op == 8 || op == 317 || op == 249 || op == 250 || op == 251 || (op >= 218 && op <= 221)) continue;
            for (uint32_t j = 1; j < end; j++) {
                if ((op == 12 && j == 4) || (op == 54 && j == 3)) continue;
                if (x[j] == variable) return 0;
            }
        }
    }
    return 1;
}

// NULL means unchanged, malformed or out of memory. No IDs are renumbered.
static inline uint32_t *vk_spv_trim_unused_distances(const uint32_t *source, uint32_t nw, uint32_t *out_words) {
    if (!source || nw < 5 || source[0] != 0x07230203 || !source[3] ||
        (uint64_t)source[3] > (uint64_t)nw * 16 + 4096) return NULL;
    uint32_t *defs = calloc(source[3], sizeof *defs), *w = malloc((size_t)nw * sizeof *w);
    if (!defs || !w) { free(defs); free(w); return NULL; }
    memcpy(w, source, (size_t)nw * sizeof *w); int changed = 0;
    for (;;) {
        memset(defs, 0, (size_t)w[3] * sizeof *defs);
        for (uint32_t at = 5; at < nw;) {
            uint32_t n = w[at] >> 16; if (!n || n > nw - at) { free(defs); free(w); return NULL; }
            uint32_t id = vk_spv_result_id(w + at);
            if (id) { if (id >= w[3]) { free(defs); free(w); return NULL; } defs[id] = at; }
            at += n;
        }
        uint32_t structure = 0, member = 0;
        for (uint32_t at = 5; at < nw; at += w[at] >> 16) {
            uint32_t n = w[at] >> 16;
            if ((w[at] & 0xffff) != 72 || n < 5 || w[at + 3] != 11 || (w[at + 4] != 3 && w[at + 4] != 4)) continue;
            uint32_t id = w[at + 1], m = w[at + 2], d = id < w[3] ? defs[id] : 0;
            if (!d || (w[d] & 0xffff) != 30 || (w[d] >> 16) != m + 3) continue;
            if (vk_spv_unused_distance(w, nw, defs, id, m)) { structure = id; member = m; break; }
        }
        if (!structure) break;
        uint32_t out = 5;
        for (uint32_t at = 5; at < nw;) {
            uint32_t n = w[at] >> 16, op = w[at] & 0xffff;
            if ((op == 6 || op == 72) && n >= 3 && w[at + 1] == structure && w[at + 2] == member) { at += n; continue; }
            uint32_t keep = op == 30 && w[at + 1] == structure ? n - 1 : n;
            memmove(w + out, w + at, (size_t)keep * sizeof *w);
            w[out] = keep << 16 | op; out += keep; at += n;
        }
        nw = out; changed = 1;
    }
    free(defs); if (!changed) { free(w); return NULL; }
    *out_words = nw; return w;
}
