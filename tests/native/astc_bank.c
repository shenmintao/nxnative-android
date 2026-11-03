// Exercise the production Android reader, including SHA-256 and zstd.
#include "astc_bank.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 2); char error[128]; dm_astc_bank *b = dm_astc_open(argv[1], error, sizeof error);
    if (!b) return 2;
    assert(dm_astc_source_format(0x41) == 0x1a01 && dm_astc_source_format(0x42) == 0x1a01);
    assert(dm_astc_source_format(0x45) == 0x1a06 && dm_astc_source_format(0x46) == 0x1a06);
    assert(dm_astc_source_format(0x49) == 0 && dm_astc_source_format(0x4c) == 0);
    unsigned shapes[3][6] = {{0x1a01, 7, 9, 3, 2, 0}, {0x1c06, 16, 16, 1, 1, 1}, {0x1e01, 4, 4, 1, 1, 0}};
    for (int i = 0; i < 3; i++) {
        unsigned *s = shapes[i]; uint64_t n = dm_astc_blocks(s[1], s[2], s[3], s[4]); assert(n);
        size_t bytes = n * (s[0] >> 8 == 0x1a ? 8 : 16); void *src = malloc(bytes); memset(src, s[0] & 255, bytes);
        uint8_t sha[32]; CC_SHA256(src, (CC_LONG)bytes, sha); free(src);
        const dm_astc_entry *e = dm_astc_find(b, sha, s[0], s[1], s[2], s[3], s[4]); if (!e) return 3;
        assert(e->flag == (s[5] ? 0 : 2));
        void *out = malloc(n * 16);
        int ok = dm_astc_read(b, e, out, n * 16); free(out);
        if (ok != !s[5]) return 4;
        sha[0] ^= 1; assert(!dm_astc_find(b, sha, s[0], s[1], s[2], s[3], s[4]));
    }
    assert(!dm_astc_blocks(0, 4, 1, 1)); assert(!dm_astc_blocks(32768, 32768, 1, 1));
    dm_astc_close(b); return 0;
}
