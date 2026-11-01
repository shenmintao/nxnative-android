// Conservative static extents for uniform blocks shaped as one std140 vec4 array.
// Unknown layouts, dynamic indices and escaping pointers leave the original range.
// See SPIR-V 1.6 OpAccessChain and Vulkan's descriptor buffer range rules.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t def, binding, stride, offset0, block, owner, limit, elem, extent, bad;
} vk_spv_ub_node;

static inline const uint32_t *vk_spv_ub_def(const uint32_t *w, const vk_spv_ub_node *nodes, uint32_t bound, uint32_t id) {
    return id < bound && nodes[id].def ? w + nodes[id].def : NULL;
}
static inline int vk_spv_ub_const(const uint32_t *w, const vk_spv_ub_node *nodes, uint32_t bound, uint32_t id, uint32_t *value) {
    const uint32_t *d = vk_spv_ub_def(w, nodes, bound, id);
    if (!d || d[0] != (4u << 16 | 43)) return 0;   // OpConstant, one literal word
    const uint32_t *t = vk_spv_ub_def(w, nodes, bound, d[1]);
    if (!t || (t[0] & 0xffff) != 21 || t[2] != 32) return 0;
    *value = d[3]; return 1;
}
static inline void vk_spv_uniform_extents(const uint32_t *w, uint32_t nw, uint32_t *out, uint32_t nout) {
    memset(out, 0, (size_t)nout * sizeof *out);
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > 262144) return;
    uint32_t bound = w[3], fn = nw;
    vk_spv_ub_node *nodes = calloc(bound, sizeof *nodes);
    uint32_t *counts = calloc(nout, sizeof *counts);
    if (!nodes || !counts) { free(nodes); free(counts); return; }
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) goto done;
        uint32_t id = 0;
        if (((op == 21 || op == 23 || op == 28 || op == 32) && n >= 4) || (op == 22 && n >= 3) || (op == 30 && n >= 2)) id = w[i + 1];
        else if ((op == 43 || op == 59) && n >= 4) id = w[i + 2];
        if (id && id < bound) nodes[id].def = i;
        if (op == 71 && n >= 3 && w[i + 1] < bound) {
            vk_spv_ub_node *v = &nodes[w[i + 1]];
            if (w[i + 2] == 2) v->block = 1;
            if (n == 4 && w[i + 2] == 6) v->stride = w[i + 3];
            if (n == 4 && w[i + 2] == 33 && w[i + 3] < nout) v->binding = w[i + 3] + 1;
        }
        if (op == 72 && n == 5 && w[i + 1] < bound && w[i + 2] == 0 && w[i + 3] == 35 && w[i + 4] == 0) nodes[w[i + 1]].offset0 = 1;
        if (op == 54 && fn == nw) fn = i;
        i += n;
    }
    for (uint32_t id = 1; id < bound; id++) {
        vk_spv_ub_node *v = &nodes[id]; const uint32_t *d = vk_spv_ub_def(w, nodes, bound, id);
        if (!d || (d[0] & 0xffff) != 59 || d[3] != 2 || !v->binding) continue;   // OpVariable Uniform
        counts[v->binding - 1]++;
        if (d[0] >> 16 != 4) continue;
        const uint32_t *p = vk_spv_ub_def(w, nodes, bound, d[1]);
        if (!p || p[0] != (4u << 16 | 32) || p[2] != 2) continue;
        const uint32_t *s = vk_spv_ub_def(w, nodes, bound, p[3]);
        if (!s || s[0] != (3u << 16 | 30) || !nodes[p[3]].block || !nodes[p[3]].offset0) continue;
        const uint32_t *a = vk_spv_ub_def(w, nodes, bound, s[2]);
        if (!a || a[0] != (4u << 16 | 28) || nodes[s[2]].stride != 16) continue;
        uint32_t len;
        if (!vk_spv_ub_const(w, nodes, bound, a[3], &len) || !len) continue;
        const uint32_t *vec = vk_spv_ub_def(w, nodes, bound, a[2]);
        if (!vec || vec[0] != (4u << 16 | 23) || vec[3] != 4) continue;
        const uint32_t *scalar = vk_spv_ub_def(w, nodes, bound, vec[2]);
        if (!scalar || ((scalar[0] & 0xffff) != 21 && (scalar[0] & 0xffff) != 22) || scalar[2] != 32) continue;
        v->owner = id; v->limit = len; v->elem = a[2];
    }
    // Record all direct access-chain result pointers first; the next pass catches
    // their uses even in loop backedges or functions placed earlier in the module.
    for (uint32_t i = fn; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if ((op == 65 || op == 66) && n >= 4 && w[i + 3] < bound && nodes[w[i + 3]].owner) {
            uint32_t root = nodes[w[i + 3]].owner, member, index, component = 0;
            vk_spv_ub_node *v = &nodes[root];
            if (w[i + 2] < bound) nodes[w[i + 2]].owner = root; else { v->bad = 1; i += n; continue; }
            const uint32_t *pt = vk_spv_ub_def(w, nodes, bound, w[i + 1]);
            const uint32_t *vec = vk_spv_ub_def(w, nodes, bound, v->elem);
            int ok = (n == 6 || n == 7) && w[i + 3] == root && pt && pt[0] == (4u << 16 | 32) && pt[2] == 2 &&
                vk_spv_ub_const(w, nodes, bound, w[i + 4], &member) && member == 0 &&
                vk_spv_ub_const(w, nodes, bound, w[i + 5], &index) && index < v->limit && index < UINT32_MAX / 16 &&
                (n == 6 ? pt[3] == v->elem : pt[3] == vec[2] && vk_spv_ub_const(w, nodes, bound, w[i + 6], &component) && component < 4);
            if (!ok) v->bad = 1;
            else if ((index + 1) * 16 > v->extent) v->extent = (index + 1) * 16;
        }
        i += n;
    }
    for (uint32_t i = fn; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        for (uint32_t j = 1; j < n; j++) {
            uint32_t id = w[i + j]; if (id >= bound || !nodes[id].owner) continue;
            uint32_t root = nodes[id].owner;
            if ((op == 65 || op == 66) && ((j == 2 && id != root) || (j == 3 && id == root))) continue;
            if (op == 61 && j == 3 && id != root) continue;   // OpLoad through the direct array-element pointer
            nodes[root].bad = 1;   // includes dynamic/nested addressing and pointer copies, phi nodes or function arguments
        }
        i += n;
    }
    for (uint32_t id = 1; id < bound; id++) {
        const vk_spv_ub_node *v = &nodes[id];
        if (v->owner == id && !v->bad && v->extent && counts[v->binding - 1] == 1) out[v->binding - 1] = v->extent;
    }
done:
    free(nodes); free(counts);
}
