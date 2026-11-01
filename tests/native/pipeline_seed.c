#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#define lstat stat
#define fsync _commit
#endif
#define COPY_CHUNK 262144
#define AASSET_MODE_STREAMING 2
typedef struct { unsigned char data[40]; int offset, limit; int64_t size; } AAsset;
typedef struct { AAsset asset; int missing, opened, closed; } AAssetManager;
static AAssetManager manager;
static AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) {
    assert(!strcmp(name, "nxnative/vk_pipelines.bin") && mode == 2);
    m->opened++; m->asset.offset = 0;
    return m->missing ? NULL : &m->asset;
}
static int64_t AAsset_getLength64(AAsset *a) { return a->size; }
static int AAsset_read(AAsset *a, void *out, size_t size) {
    int n = a->limit - a->offset;
    if (n > 3) n = 3; /* Exercise short successful reads too. */
    if ((size_t)n > size) n = (int)size;
    if (n > 0) { memcpy(out, a->data + a->offset, (size_t)n); a->offset += n; }
    return n;
}
static void AAsset_close(AAsset *a) { assert(a == &manager.asset); manager.closed++; }
/* PRODUCTION HELPERS */
int main(void) {
    const char *dst = "vk_pipelines.bin";
    uint32_t header[2] = {0x31455050u, 16};
    memcpy(manager.asset.data, header, 8); manager.asset.size = manager.asset.limit = 40;
    memset(manager.asset.data + 8, 0x93, 32);
    manager.missing = 1;
    assert(dm_pipeline_keys_seed(&manager, ".") == 0 && access(dst, F_OK));
    manager.missing = 0; manager.asset.limit = 17;
    assert(dm_pipeline_keys_seed(&manager, ".") == -1 && access(dst, F_OK));
    assert(access("vk_pipelines.seed.tmp", F_OK));
    manager.asset.limit = 40; manager.asset.data[0] = 0;
    assert(dm_pipeline_keys_seed(&manager, ".") == -1 && access(dst, F_OK));
    manager.asset.data[0] = 'P'; manager.asset.size = 39;
    assert(dm_pipeline_keys_seed(&manager, ".") == -1 && access(dst, F_OK));
    manager.asset.size = 40;
    assert(dm_pipeline_keys_seed(&manager, "missing-parent") == -1 && access(dst, F_OK));
    assert(dm_pipeline_keys_seed(&manager, ".") == 1);
    unsigned char actual[40]; FILE *file = fopen(dst, "rb"); assert(file);
    assert(fread(actual, 1, 40, file) == 40 && fgetc(file) == EOF); fclose(file);
    assert(!memcmp(actual, manager.asset.data, 40));
    file = fopen(dst, "wb"); assert(file); fputs("player keys", file); fclose(file);
    int opened = manager.opened;
    assert(dm_pipeline_keys_seed(&manager, ".") == 0 && manager.opened == opened);
    file = fopen(dst, "rb"); assert(file); memset(actual, 0, 40);
    assert(fread(actual, 1, 40, file) == 11); fclose(file);
    assert(!memcmp(actual, "player keys", 11));
    assert(manager.closed == manager.opened - 1);
    return 0;
}
