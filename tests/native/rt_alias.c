// Aliased MRTs must not store separate tiles to the same image. Exercise slot
// selection, including the destination used by clears of a trailing alias.
#include "rt_alias.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    vk_rt_subresource slots[8] = {
        {1, 0, 0, 1}, {2, 0, 0, 1}, {2, 0, 0, 1},
        {3, 0, 0, 1}, {3, 0, 0, 1}, {4, 0, 0, 1}
    };
    const int expected[8] = {0, 1, 1, 3, 3, 5, -1, -1};
    for (int i = 0; i < 8; i++) assert(vk_rt_first_slot(slots, i) == expected[i]);
    assert(vk_rt_first_slot(slots, -1) == -1);
    assert(vk_rt_first_slot(slots, 8) == -1);
    // Different levels, layers and layer ranges of one image are distinct views.
    slots[6] = (vk_rt_subresource){2, 1, 0, 1};
    assert(vk_rt_first_slot(slots, 6) == 6);
    slots[6] = (vk_rt_subresource){2, 0, 1, 1};
    assert(vk_rt_first_slot(slots, 6) == 6);
    slots[6] = (vk_rt_subresource){2, 0, 0, 2};
    assert(vk_rt_first_slot(slots, 6) == 6);
    // A trailing duplicate still maps to its retained slot after ncol shrinks.
    slots[7] = slots[1];
    assert(vk_rt_first_slot(slots, 7) == 1);
    // Storage sharing can give distinct texture objects / format views one image.
    slots[0].image = 0;
    slots[6] = slots[3];
    assert(vk_rt_first_slot(slots, 6) == 3);
    puts("render target alias selection passed");
    return 0;
}
