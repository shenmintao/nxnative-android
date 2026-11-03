// Version 1 bank: 128-byte header + sorted 128-byte entries + independent zstd
// frames. Validate the small index once and each image before upload; never
// expand a pack at launch or accept an unchecked decompressed allocation size.
#include "astc_bank.h"
#include <CommonCrypto/CommonDigest.h>
#include "../third_party/zstd/lib/zstd.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define ASTC_MAX_IMAGE (256u << 20)
_Static_assert(sizeof(dm_astc_entry) == 128, "ASTC wire entry");
static const uint8_t policy[32] = {0xec,0x87,0x34,0x45,0x1f,0xf4,0x78,0x58,0xbb,0xd0,0xe9,0xfd,0x9d,0xbc,0x99,0x97,0x52,0x93,0x6f,0x5f,0x8a,0xb2,0x5c,0x7e,0x87,0xfa,0x9f,0xea,0x31,0x79,0xbf,0xc4};
struct dm_astc_bank { FILE *file; uint64_t count; dm_astc_entry *entries; pthread_mutex_t mutex; };
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int seek(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (int64_t)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
uint64_t dm_astc_blocks(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers) {
    if (!w || w > 32768 || !h || h > 32768 || !levels || levels > 16 || !layers || layers > 2048) return 0;
    uint64_t n = 0;
    for (uint32_t m = 0; m < levels; m++) { uint32_t x = w >> m, y = h >> m; n += (uint64_t)(((x ? x : 1) + 3) / 4) * (((y ? y : 1) + 3) / 4); }
    n *= layers; return n <= ASTC_MAX_IMAGE / 16 ? n : 0;
}
uint32_t dm_astc_source_format(int f) {
    switch (f) { case 0x41: case 0x42: return 0x1a01; case 0x45: case 0x46: return 0x1a06;
    case 0x44: return 0x1c01; case 0x48: return 0x1c06; case 0x4b: return 0x1e01; default: return 0; }
}
void dm_astc_close(dm_astc_bank *b) { if (b) { if (b->file) fclose(b->file); free(b->entries); pthread_mutex_destroy(&b->mutex); free(b); } }
dm_astc_bank *dm_astc_open(const char *path, char *error, size_t error_size) {
    const char *why = "invalid ASTC bank"; dm_astc_bank *b = calloc(1, sizeof *b);
    if (!b) { if (error_size) snprintf(error, error_size, "ASTC index allocation failed"); return NULL; }
    pthread_mutex_init(&b->mutex, NULL); b->file = fopen(path, "rb");
    if (!b->file) { why = "ASTC bank unavailable"; goto bad; }
    struct stat st; uint8_t h[128], digest[32]; uint16_t little = 1;
    if (*(uint8_t *)&little != 1 || fstat(fileno(b->file), &st) || st.st_size < 128 || fread(h, 1, 128, b->file) != 128) goto bad;
    b->count = u64(h + 16); uint64_t start = u64(h + 24), size = (uint64_t)st.st_size;
    if (memcmp(h, "NXASTC01", 8) || u32(h + 8) != 1 || u32(h + 12) != 128 || b->count > 262144 || start != 128 + b->count * 128 || start > size || memcmp(h + 64, policy, 32)) goto bad;
    b->entries = malloc((size_t)b->count * 128 + 1);
    if (!b->entries || fread(b->entries, 128, (size_t)b->count, b->file) != b->count) goto bad;
    CC_SHA256(b->entries, (CC_LONG)(b->count * 128), digest);
    if (memcmp(h + 32, digest, 32)) { why = "ASTC index checksum mismatch"; goto bad; }
    uint64_t end = start;
    for (uint64_t i = 0; i < b->count; i++) {
        dm_astc_entry *e = &b->entries[i]; uint64_t n = dm_astc_blocks(e->w, e->h, e->levels, e->layers);
        int bc1 = e->format == 0x1a01 || e->format == 0x1a06;
        if (!n || (i && memcmp(e[-1].key, e->key, 32) >= 0) || (!bc1 && e->format != 0x1c01 && e->format != 0x1c06 && e->format != 0x1e01) ||
            (e->flag != 0 && e->flag != 2) || e->blocks != n || e->bytes != (e->flag ? n * 16 : 0) || e->source_bytes != n * (bc1 ? 8 : 16) ||
            e->offset != end || e->offset > size || e->length > size - e->offset || e->length > ASTC_MAX_IMAGE || !!e->flag != !!e->length) goto bad;
        end = (e->offset + e->length + 15) & ~15ull;
    }
    if (end != size) goto bad;
    return b;
bad:
    if (error_size) snprintf(error, error_size, "%s", why); dm_astc_close(b); return NULL;
}
const dm_astc_entry *dm_astc_find(dm_astc_bank *b, const uint8_t source_sha[32], uint32_t format, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers) {
    if (!b) return NULL;
    uint32_t shape[6] = {format, w, h, levels, layers, 1}; uint8_t key[32]; CC_SHA256_CTX c;
    CC_SHA256_Init(&c); CC_SHA256_Update(&c, source_sha, 32); CC_SHA256_Update(&c, shape, sizeof shape); CC_SHA256_Final(key, &c);
    uint64_t lo = 0, hi = b->count;
    while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; int d = memcmp(b->entries[m].key, key, 32); if (d < 0) lo = m + 1; else hi = m; }
    return lo < b->count && !memcmp(b->entries[lo].key, key, 32) ? &b->entries[lo] : NULL;
}
int dm_astc_read(dm_astc_bank *b, const dm_astc_entry *e, void *dst, size_t size) {
    if (!b || !e || e->flag != 2 || size != e->bytes || size > ASTC_MAX_IMAGE) return 0;
    uint8_t *frame = malloc((size_t)e->length); if (!frame) return 0;
    pthread_mutex_lock(&b->mutex);
    int ok = !seek(b->file, e->offset) && fread(frame, 1, (size_t)e->length, b->file) == e->length;
    pthread_mutex_unlock(&b->mutex);
    if (ok) ok = ZSTD_getFrameContentSize(frame, (size_t)e->length) == size && ZSTD_findFrameCompressedSize(frame, (size_t)e->length) == e->length;
    if (ok) { size_t n = ZSTD_decompress(dst, size, frame, (size_t)e->length); ok = !ZSTD_isError(n) && n == size; }
    free(frame);
    if (ok) { uint8_t sha[32]; CC_SHA256(dst, (CC_LONG)size, sha); ok = !memcmp(sha, e->sha, 32); }
    return ok;
}
