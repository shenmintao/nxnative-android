// Derive sampler comparison from the actual SPIR-V image instructions. The
// guest may bind one sampler to both ordinary and depth-comparison lookups.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { VK_SAMPLE_PLAIN = 1, VK_SAMPLE_COMPARE = 2, VK_SAMPLE_UNKNOWN = 4 };
typedef struct { uint32_t at, binding; uint8_t visiting; } vk_spv_sample_id;
typedef struct {
    const uint32_t *w; uint32_t nw, bound;
    const uint16_t *bindings; size_t nbindings;
    uint8_t *use; vk_spv_sample_id *ids;
} vk_spv_sample_context;

static inline int vk_spv_sample_root(vk_spv_sample_context *c, uint32_t id, uint8_t use, unsigned depth) {
    if (!id || id >= c->bound || depth > 256) return 2;
    vk_spv_sample_id *m = &c->ids[id];
    if (m->visiting) return 0;
    if (!m->at) return 2;
    const uint32_t *w = c->w + m->at;
    uint32_t n = w[0] >> 16, op = w[0] & 65535;
    m->visiting = 1; int result = 2;
    if (op == 59 && n >= 4) {
        for (size_t i = 0; i < c->nbindings; i++) if (c->bindings[i] == m->binding) {
            c->use[i] |= use; result = 1;
        }
    } else if ((op == 61 || op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 100) && n >= 4) {
        result = vk_spv_sample_root(c, w[3], use, depth + 1);
    } else if (op == 86 && n >= 5) {
        result = vk_spv_sample_root(c, w[3], use, depth + 1) | vk_spv_sample_root(c, w[4], use, depth + 1);
    } else if (op == 169 && n >= 6) {
        result = vk_spv_sample_root(c, w[4], use, depth + 1) | vk_spv_sample_root(c, w[5], use, depth + 1);
    } else if (op == 245 && n >= 5) {
        result = 0;
        for (uint32_t i = 3; i + 1 < n; i += 2) result |= vk_spv_sample_root(c, w[i], use, depth + 1);
    }
    m->visiting = 0; return result;
}

// 1: exact descriptor roots, 2: unknown roots (retain the guest state), 0:
// malformed input/allocation. Flags are indexed by reflection entry, not slot.
static inline int vk_spv_sampler_use(const uint32_t *w, uint32_t nw, const uint16_t *bindings,
                                     size_t nbindings, uint8_t *use) {
    memset(use, VK_SAMPLE_UNKNOWN, nbindings);
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 24)) return 0;
    vk_spv_sample_context c = {w, nw, w[3], bindings, nbindings, use, NULL};
    c.ids = calloc(c.bound, sizeof *c.ids);
    if (!c.ids) return 0;
    for (uint32_t i = 0; i < c.bound; i++) c.ids[i].binding = UINT32_MAX;
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) { free(c.ids); return 0; }
        if (op == 71 && n == 4 && w[i + 2] == 33 && w[i + 1] < c.bound) c.ids[w[i + 1]].binding = w[i + 3];
        if ((op == 55 || op == 57 || op == 59 || op == 61 || op == 65 || op == 66 || op == 67 || op == 70 ||
             op == 83 || op == 86 || op == 100 || op == 169 || op == 245) && n >= 3 && w[i + 2] < c.bound)
            c.ids[w[i + 2]].at = i;
        i += n;
    }
    memset(use, 0, nbindings); int result = 1;
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        int compare = op == 89 || op == 90 || op == 93 || op == 94 || op == 97 ||
                      op == 307 || op == 308 || op == 311 || op == 312 || op == 315;
        int plain = op == 87 || op == 88 || op == 91 || op == 92 || op == 96 ||
                    op == 305 || op == 306 || op == 309 || op == 310 || op == 314;
        if (compare || plain) {
            if (n < 4) result = 2;
            else if (vk_spv_sample_root(&c, w[i + 3], compare ? VK_SAMPLE_COMPARE : VK_SAMPLE_PLAIN, 0) != 1) result = 2;
        }
        i += n;
    }
    if (result == 2) for (size_t i = 0; i < nbindings; i++) use[i] |= VK_SAMPLE_UNKNOWN;
    free(c.ids); return result;
}
