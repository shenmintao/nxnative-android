#include <assert.h>
#include <stdio.h>
#include "spv_sampler_use.h"

static uint32_t words[512], count;
#define I(op, ...) do { uint32_t a[] = {__VA_ARGS__}; unsigned n = sizeof a / sizeof *a; \
    words[count++] = ((n + 1) << 16) | (op); memcpy(words + count, a, sizeof a); count += n; } while (0)
static void reset(void) {
    memset(words, 0, sizeof words); words[0] = 0x07230203; words[3] = 256; count = 5;
    I(71, 10, 33, 3); I(71, 11, 33, 7);
    I(59, 1, 10, 0); I(59, 1, 11, 0);
    I(61, 2, 20, 10); I(61, 2, 21, 11);
}
static const uint16_t bindings[] = {7, 3, 100};
static uint8_t use[3];
static int inspect(void) { return vk_spv_sampler_use(words, count, bindings, 3, use); }

int main(void) {
    const unsigned plain[] = {87, 88, 91, 92, 96, 305, 306, 309, 310, 314};
    const unsigned compare[] = {89, 90, 93, 94, 97, 307, 308, 311, 312, 315};
    for (unsigned i = 0; i < 10; i++) {
        reset(); I(plain[i], 4, 30, 20, 0, 0);
        assert(inspect() == 1 && use[0] == 0 && use[1] == VK_SAMPLE_PLAIN && use[2] == 0);
        reset(); I(compare[i], 4, 30, 21, 0, 0);
        assert(inspect() == 1 && use[0] == VK_SAMPLE_COMPARE && use[1] == 0 && use[2] == 0);
    }
    // Separate image and sampler roots, and non-consecutive reflection slots.
    reset(); I(86, 3, 22, 20, 21); I(89, 4, 30, 22, 0, 0);
    assert(inspect() == 1 && use[0] == 2 && use[1] == 2 && use[2] == 0);
    // Descriptor arrays and SSA copies retain their original binding.
    reset(); I(65, 1, 22, 10, 99); I(61, 2, 23, 22); I(83, 2, 24, 23); I(87, 4, 30, 24, 0);
    assert(inspect() == 1 && use[0] == 0 && use[1] == 1);
    // Both branches of a select, and a loop phi's known incoming root.
    reset(); I(169, 2, 22, 100, 20, 21); I(90, 4, 30, 22, 0, 0);
    assert(inspect() == 1 && use[0] == 2 && use[1] == 2);
    reset(); I(245, 2, 22, 20, 200, 22, 201); I(97, 4, 30, 22, 0, 0);
    assert(inspect() == 1 && use[0] == 0 && use[1] == 2);
    // A mixed binding is exposed as mixed; it must never become plain-only.
    reset(); I(87, 4, 30, 20, 0); I(89, 4, 31, 20, 0, 0);
    assert(inspect() == 1 && use[1] == 3);
    // Fetches and queries do not decide the comparison state of a sampler.
    reset(); I(95, 4, 30, 20, 0); I(105, 4, 31, 20, 0);
    assert(inspect() == 1 && use[0] == 0 && use[1] == 0);
    // Unknown roots invalidate even a separate binding's inferred plain use.
    reset(); I(87, 4, 30, 20, 0); I(57, 2, 22, 101); I(89, 4, 31, 22, 0, 0);
    assert(inspect() == 2 && use[0] == 4 && use[1] == 5 && use[2] == 4);
    reset(); I(245, 2, 22, 22, 200); I(89, 4, 30, 22, 0, 0);
    assert(inspect() == 2 && (use[1] & VK_SAMPLE_UNKNOWN));
    reset(); I(89, 4, 30, 999, 0, 0);
    assert(inspect() == 2 && use[0] == 4);
    reset(); words[count++] = (100u << 16) | 61;
    assert(inspect() == 0 && use[0] == 4 && use[1] == 4);
    reset(); words[3] = 0; assert(inspect() == 0);
    puts("PASS: plain, comparison, arrays, SSA roots, mixed and unknown sampling");
    return 0;
}
