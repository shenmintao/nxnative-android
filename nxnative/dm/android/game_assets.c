// Materialize the signed APK's game.pack before the host starts. Each content key has its own
// directory: an interrupted update cannot expose a mixture of old and new modules/RomFS.
// Saves and user settings stay outside these managed directories.
#include "game_assets.h"
#include <android/log.h>
#include <android/asset_manager_jni.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>
#include <zlib.h>

#define PACK_HEADER 56
#define PACK_PREFIX "nxnative-data-"
#define COPY_CHUNK (256 * 1024)
typedef struct { char *name; uint64_t size, offset; uint32_t crc; int copy; } pack_file;

static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int asset_read(AAsset *asset, void *buffer, size_t size) {
    unsigned char *out = buffer;
    while (size) { int n = AAsset_read(asset, out, size > COPY_CHUNK ? COPY_CHUNK : size); if (n <= 0) return -1; out += n; size -= (size_t)n; }
    return 0;
}
static int hex_key(const char *p) {
    if (strlen(p) != 64) return 0;
    for (int i = 0; i < 64; ++i) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}
static int relative_name(const char *p) {
    if (!*p || *p == '/' || strchr(p, '\\') || !strncmp(p, ".nxnative", 9)) return 0;
    for (const char *part = p; *part;) {
        const char *end = strchr(part, '/'); size_t n = end ? (size_t)(end - part) : strlen(part);
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break; part = end + 1; if (!*part) return 0;
    }
    return 1;
}
static int path_join(char *out, size_t cap, const char *root, const char *name) {
    int n = snprintf(out, cap, "%s/%s", root, name); return n < 0 || (size_t)n >= cap ? -1 : 0;
}
static int directory(const char *path) {
    struct stat st;
    if (!lstat(path, &st)) return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) ? 0 : -1;
    return errno == ENOENT ? mkdir(path, 0700) : -1;
}
static int parents(const char *root, const char *relative) {
    char path[PATH_MAX];
    if (path_join(path, sizeof path, root, relative)) return -1;
    for (char *p = path + strlen(root) + 1; *p; ++p) if (*p == '/') {
        *p = 0; int rc = directory(path); *p = '/'; if (rc) return -1;
    }
    return 0;
}
static int has_size(const char *path, uint64_t size) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size == size;
}
static int matches(const char *path, const pack_file *entry, unsigned char *buffer) {
    if (!has_size(path, entry->size)) return 0;
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    uint32_t crc = 0; size_t n;
    while ((n = fread(buffer, 1, COPY_CHUNK, file))) crc = (uint32_t)crc32(crc, buffer, (uInt)n);
    int ok = !ferror(file) && crc == entry->crc; fclose(file); return ok;
}
static int ready_key(const char *path, const unsigned char *key) {
    unsigned char have[32];
    if (!has_size(path, sizeof have)) return 0;
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    int ok = fread(have, 1, sizeof have, file) == sizeof have && !memcmp(have, key, sizeof have); fclose(file); return ok;
}
static int write_atomic(const char *tmp, const char *path, const void *data, size_t size) {
    FILE *file = fopen(tmp, "wb"); if (!file) return -1;
    int ok = fwrite(data, 1, size, file) == size && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
static int remove_tree(const char *path) {
    struct stat st; if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path);
    DIR *dir = opendir(path); if (!dir) return -1;
    struct dirent *item; int result = 0; char child[PATH_MAX];
    while ((item = readdir(dir))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (path_join(child, sizeof child, path, item->d_name) || remove_tree(child)) result = -1;
    }
    closedir(dir); if (rmdir(path)) result = -1; return result;
}
static void remove_old(const char *root, const char *current) {
    DIR *dir = opendir(root); if (!dir) return;
    struct dirent *item; char path[PATH_MAX];
    while ((item = readdir(dir))) {
        if (strncmp(item->d_name, PACK_PREFIX, strlen(PACK_PREFIX)) || !hex_key(item->d_name + strlen(PACK_PREFIX)) || !strcmp(item->d_name, current)) continue;
        if (!path_join(path, sizeof path, root, item->d_name) && remove_tree(path))
            __android_log_print(ANDROID_LOG_WARN, "dmhost", "old embedded data kept: %s", path);
    }
    closedir(dir);
}

static int prepare_internal(AAssetManager *manager, const char *root, char *data_root, size_t capacity,
                           dm_assets_progress progress, void *ctx, char *error, size_t error_capacity) {
    AAsset *asset = AAssetManager_open(manager, "nxnative/game.pack", AASSET_MODE_STREAMING);
    unsigned char header[PACK_HEADER], *index = NULL, *buffer = NULL;
    pack_file *files = NULL; uint32_t count = 0, allocated = 0; int result = -1;
    char target[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX], ready[PATH_MAX], folder[sizeof PACK_PREFIX + 64];
    const char *why = "Cannot prepare game data";
    if (!asset) {
        AAsset *manifest = AAssetManager_open(manager, "nxnative/manifest.json", AASSET_MODE_STREAMING);
        if (manifest) { AAsset_close(manifest); why = "Embedded game data is missing from the APK"; goto done; }
        if (strlen(root) >= capacity) { why = "Game data path is too long"; goto done; }
        strcpy(data_root, root); return 0;
    }
    if (asset_read(asset, header, sizeof header) || memcmp(header, "NXPACK01", 8)) { why = "Invalid embedded game data header"; goto done; }
    count = u32(header + 8); uint32_t index_size = u32(header + 12); uint64_t total = u64(header + 16);
    uint64_t length = (uint64_t)AAsset_getLength64(asset);
    if (!count || count > index_size / 17 || length < PACK_HEADER + (uint64_t)index_size || total != length - PACK_HEADER - index_size) {
        why = "Incomplete embedded game data"; goto done;
    }
    index = malloc(index_size); files = calloc(count, sizeof *files); buffer = malloc(COPY_CHUNK);
    if (!index || !files || !buffer) { why = "Not enough memory to prepare game data"; goto done; }
    if (asset_read(asset, index, index_size)) { why = "Cannot read embedded game data index"; goto done; }
    size_t cursor = 0; uint64_t offset = PACK_HEADER + (uint64_t)index_size;
    for (uint32_t i = 0; i < count; ++i) {
        if (index_size - cursor < 16) { why = "Invalid game data index record"; goto done; }
        uint32_t n = u32(index + cursor); files[i].size = u64(index + cursor + 4); files[i].crc = u32(index + cursor + 12); cursor += 16;
        if (!n || n >= PATH_MAX || n > index_size - cursor || files[i].size > length - offset || memchr(index + cursor, 0, n)) {
            why = "Invalid game data name or length"; goto done;
        }
        files[i].name = strndup((char *)index + cursor, n); allocated = i + 1;
        if (!files[i].name) { why = "Not enough memory to prepare game data"; goto done; }
        if (!relative_name(files[i].name) || (i && strcmp(files[i - 1].name, files[i].name) >= 0)) {
            why = "Invalid game data path or ordering"; goto done;
        }
        files[i].offset = offset; offset += files[i].size; cursor += n;
    }
    if (cursor != index_size || offset != length) { why = "Game data index size does not match its contents"; goto done; }
    free(index); index = NULL;
    memcpy(folder, PACK_PREFIX, strlen(PACK_PREFIX));
    for (int i = 0; i < 32; ++i) snprintf(folder + strlen(PACK_PREFIX) + i * 2, 3, "%02x", header[24 + i]);
    if (directory(root) || path_join(target, sizeof target, root, folder) || directory(target) || strlen(target) >= capacity ||
        path_join(ready, sizeof ready, target, ".nxnative-ready") || path_join(tmp, sizeof tmp, target, ".nxnative-copy")) {
        why = "Cannot create the game data directory"; goto done;
    }
    int complete = ready_key(ready, header + 24); uint64_t need = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (path_join(path, sizeof path, target, files[i].name) || parents(target, files[i].name)) { why = "Cannot create game data folders"; goto done; }
        files[i].copy = !(complete ? has_size(path, files[i].size) : matches(path, files + i, buffer));
        if (files[i].copy) need += files[i].size;
    }
    struct statvfs disk;
    if (!statvfs(target, &disk) && need > (uint64_t)disk.f_bavail * disk.f_frsize) { why = "Not enough storage: free space for the game's unpacked data and launch again"; goto done; }
    uint64_t copied = 0;
    if (progress) progress(0, total, ctx);
    for (uint32_t i = 0; i < count; ++i) {
        if (!files[i].copy) { copied += files[i].size; if (progress) progress(copied, total, ctx); continue; }
        if (AAsset_seek64(asset, (off64_t)files[i].offset, SEEK_SET) != (off64_t)files[i].offset || path_join(path, sizeof path, target, files[i].name)) {
            why = "Cannot seek to embedded game data"; goto done;
        }
        FILE *file = fopen(tmp, "wb"); if (!file) { why = "Cannot write game data; check free storage"; goto done; }
        uint64_t left = files[i].size; uint32_t crc = 0; int failed = 0;
        while (left) {
            size_t n = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;
            if (asset_read(asset, buffer, n) || fwrite(buffer, 1, n, file) != n) { failed = 1; break; }
            crc = (uint32_t)crc32(crc, buffer, (uInt)n); left -= n; copied += n;
            if (progress) progress(copied, total, ctx);
        }
        if (fflush(file) || fsync(fileno(file))) failed = 1;
        if (fclose(file)) failed = 1;
        if (failed || crc != files[i].crc || rename(tmp, path)) { unlink(tmp); why = "Game data copy failed or did not match its checksum; launch again to retry"; goto done; }
    }
    if (write_atomic(tmp, ready, header + 24, 32)) { why = "Cannot finish preparing game data"; goto done; }
    // First-launch defaults are copied once. Updates never overwrite player settings or saves.
    char settings[PATH_MAX];
    if (path_join(settings, sizeof settings, root, "settings.txt") || path_join(path, sizeof path, target, "settings.txt")) { why = "Game settings path is too long"; goto done; }
    struct stat st;
    if (lstat(settings, &st) && errno == ENOENT) {
        FILE *file = fopen(path, "rb");
        if (file) {
            FILE *out = fopen(tmp, "wb"); int failed = out == NULL; size_t size;
            if (out) {
                while ((size = fread(buffer, 1, COPY_CHUNK, file))) if (fwrite(buffer, 1, size, out) != size) { failed = 1; break; }
                if (ferror(file) || fflush(out) || fsync(fileno(out))) failed = 1;
                if (fclose(out)) failed = 1;
            }
            fclose(file);
            if (failed || rename(tmp, settings)) { unlink(tmp); why = "Cannot prepare game settings"; goto done; }
        }
    }
    if (progress) progress(total, total, ctx);
    strcpy(data_root, target); remove_old(root, folder); result = 1;
done:
    if (result < 0 && error_capacity) snprintf(error, error_capacity, "%s", why);
    if (asset) AAsset_close(asset);
    for (uint32_t i = 0; i < allocated; ++i) free(files[i].name);
    free(files); free(index); free(buffer); return result;
}

int dm_game_assets_prepare(AAssetManager *manager, const char *root, char *data_root, size_t capacity,
                           dm_assets_progress progress, void *ctx, char *error, size_t error_capacity) {
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    int result = prepare_internal(manager, root, data_root, capacity, progress, ctx, error, error_capacity);
    pthread_mutex_unlock(&mutex);
    return result;
}

typedef struct { JNIEnv *env; jobject activity; jmethodID progress; uint64_t last; } java_progress;
static void notify_java(uint64_t done, uint64_t total, void *opaque) {
    java_progress *context = opaque; struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    if (done && done != total && ms - context->last < 100) return;
    context->last = ms;
    (*context->env)->CallVoidMethod(context->env, context->activity, context->progress, (jlong)done, (jlong)total);
    if ((*context->env)->ExceptionCheck(context->env)) (*context->env)->ExceptionClear(context->env);
}
JNIEXPORT jstring JNICALL Java_org_nxnative_GameActivity_prepareGameData(JNIEnv *env, jobject activity, jobject assets, jstring folder) {
    AAssetManager *manager = AAssetManager_fromJava(env, assets);
    if (!manager) return (*env)->NewStringUTF(env, "Cannot open the APK game assets");
    const char *root = (*env)->GetStringUTFChars(env, folder, NULL);
    if (!root) return NULL;
    jclass type = (*env)->GetObjectClass(env, activity);
    java_progress context = {env, activity, (*env)->GetMethodID(env, type, "onGameDataProgress", "(JJ)V"), 0};
    char output[PATH_MAX], error[512] = {0};
    int result = dm_game_assets_prepare(manager, root, output, sizeof output, context.progress ? notify_java : NULL, &context, error, sizeof error);
    (*env)->ReleaseStringUTFChars(env, folder, root); (*env)->DeleteLocalRef(env, type);
    if (result < 0) { __android_log_print(ANDROID_LOG_ERROR, "dmhost", "[RED] %s", error); return (*env)->NewStringUTF(env, error); }
    return NULL;
}
