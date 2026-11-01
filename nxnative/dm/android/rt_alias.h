// A render pass may attach an image subresource only once. Keep the first colour
// slot, as the Metal backend does; clears of later aliases address that slot too.
#ifndef DM_ANDROID_RT_ALIAS_H
#define DM_ANDROID_RT_ALIAS_H
#include <stdint.h>

typedef struct {
    uint64_t image;
    uint32_t level, layer, layers;
} vk_rt_subresource;

static inline int vk_rt_first_slot(const vk_rt_subresource slots[8], int at) {
    if (at < 0 || at >= 8 || !slots[at].image) return -1;
    for (int i = 0; i < at; i++)
        if (slots[i].image == slots[at].image && slots[i].level == slots[at].level &&
            slots[i].layer == slots[at].layer && slots[i].layers == slots[at].layers) return i;
    return at;
}
#endif
