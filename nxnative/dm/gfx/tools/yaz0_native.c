// Bounds-checked Yaz0 decoder used only by desktop resource preparation.
// No CRT, allocation, system calls or game-specific assumptions are needed.
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
typedef unsigned long long u64;
EXPORT long long dm_yaz0_decompress(const unsigned char *src, u64 size, unsigned char *dst, u64 capacity) {
    if (size < 16 || src[0] != 'Y' || src[1] != 'a' || src[2] != 'z' || src[3] != '0') return -1;
    u64 expected = (u64)src[4] << 24 | (u64)src[5] << 16 | (u64)src[6] << 8 | src[7];
    if (expected > capacity) return -1;
    u64 input = 16, output = 0;
    while (output < expected) {
        if (input >= size) return -1;
        unsigned group = src[input++];
        for (unsigned mask = 0x80; mask && output < expected; mask >>= 1) {
            if (group & mask) {
                if (input >= size) return -1;
                dst[output++] = src[input++];
            } else {
                if (size - input < 2) return -1;
                unsigned first = src[input++], second = src[input++];
                u64 distance = ((first & 15) << 8 | second) + 1, length = first >> 4;
                if (length) length += 2;
                else { if (input >= size) return -1; length = (u64)src[input++] + 0x12; }
                if (distance > output) return -1;
                if (length > expected - output) length = expected - output;
                while (length--) { dst[output] = dst[output - distance]; ++output; }
            }
        }
    }
    return (long long)output;
}
