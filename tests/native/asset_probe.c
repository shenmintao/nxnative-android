// Isolated on-device check of the real APK asset reader, without game code or keys.
#include "game_assets.h"
#include <android/native_activity.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/window.h>

static ANativeActivity *activity;
static JNIEnv *thread_env;
static jmethodID progress_method;
static _Atomic int window_ready;
static int drain_input(int fd, int events, void *context) {
    (void)fd; (void)events;
    AInputQueue *queue = context; AInputEvent *event;
    while (AInputQueue_getEvent(queue, &event) >= 0) {
        if (!AInputQueue_preDispatchEvent(queue, event)) AInputQueue_finishEvent(queue, event, 0);
    }
    return 1;
}
static void input_created(ANativeActivity *act, AInputQueue *queue) {
    (void)act; AInputQueue_attachLooper(queue, ALooper_forThread(), 1, drain_input, queue);
}
static void input_destroyed(ANativeActivity *act, AInputQueue *queue) {
    (void)act; AInputQueue_detachLooper(queue);
}
static void window_created(ANativeActivity *act, ANativeWindow *window) {
    (void)act;
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer;
    if (!ANativeWindow_lock(window, &buffer, NULL)) {
        for (int y = 0; y < buffer.height; ++y) for (int x = 0; x < buffer.width; ++x) ((uint32_t *)buffer.bits)[y * buffer.stride + x] = 0xff282020;
        ANativeWindow_unlockAndPost(window);
    }
    atomic_store(&window_ready, 1);
}
static void progress(uint64_t done, uint64_t total, void *ctx) {
    (void)ctx;
    if (!progress_method) return;
    (*thread_env)->CallVoidMethod(thread_env, activity->clazz, progress_method, (jlong)done, (jlong)total);
    if ((*thread_env)->ExceptionCheck(thread_env)) (*thread_env)->ExceptionClear(thread_env);
}
static void *run(void *ctx) {
    (void)ctx;
    (*activity->vm)->AttachCurrentThread(activity->vm, &thread_env, NULL);
    jclass type = (*thread_env)->GetObjectClass(thread_env, activity->clazz);
    progress_method = (*thread_env)->GetMethodID(thread_env, type, "onGameDataProgress", "(JJ)V");
    if ((*thread_env)->ExceptionCheck(thread_env)) (*thread_env)->ExceptionClear(thread_env);
    jmethodID ready = (*thread_env)->GetMethodID(thread_env, type, "onGameDataReady", "(Ljava/lang/String;)V");
    if ((*thread_env)->ExceptionCheck(thread_env)) (*thread_env)->ExceptionClear(thread_env);
    char root[4096], error[512] = {0}, report[4096];
    progress(0, 0, NULL);
    int result = dm_game_assets_prepare(activity->assetManager, activity->externalDataPath, root, sizeof root, progress, NULL, error, sizeof error);
    char path[4096], settings[128] = {0}; unsigned long crc = 0, keyboard_crc = 0; unsigned long long bytes = 0; int deleted = 0, keyboard_bytes = -1;
    if (result >= 0) {
        snprintf(path, sizeof path, "%s/RomFS/probe.bin", root);
        FILE *data = fopen(path, "rb");
        if (data) {
            unsigned char buffer[16384]; size_t n;
            while ((n = fread(buffer, 1, sizeof buffer, data))) { crc = crc32(crc, buffer, (uInt)n); bytes += n; }
            fclose(data);
        }
        snprintf(path, sizeof path, "%s/settings.txt", activity->externalDataPath);
        FILE *cfg = fopen(path, "r"); if (cfg) { fgets(settings, sizeof settings, cfg); fclose(cfg); settings[strcspn(settings, "\r\n")] = 0; }
        // Simulate a player's changed setting, which an APK update must preserve.
        cfg = fopen(path, "w"); if (cfg) { fputs("overlay=0\nrumble=1\n", cfg); fclose(cfg); }
        snprintf(path, sizeof path, "%s/probe-action.txt", root);
        FILE *action = fopen(path, "r"); char mode[32] = {0}; if (action) { fgets(mode, sizeof mode, action); fclose(action); }
        mode[strcspn(mode, "\r\n")] = 0;
        char marker[4096]; snprintf(marker, sizeof marker, "%s/probe-deletion-tested", activity->externalDataPath);
        if (!strcmp(mode, "delete-once") && access(marker, F_OK)) {
            snprintf(path, sizeof path, "%s/RomFS/probe.bin", root);
            if (!unlink(path)) { FILE *flag = fopen(marker, "w"); if (flag) { fputs("test", flag); fclose(flag); deleted = 1; } }
        }
        if (!strcmp(mode, "keyboard")) {
            for (int i = 0; i < 500 && !atomic_load(&window_ready); ++i) usleep(10000);
            jmethodID keyboard = (*thread_env)->GetMethodID(thread_env, type, "requestText", "([B[BI)[B");
            if ((*thread_env)->ExceptionCheck(thread_env)) (*thread_env)->ExceptionClear(thread_env);
            if (keyboard) {
                const char title[] = "Keyboard verification", initial[] = "\xe6\xb5\x8b\xe8\xaf\x95\xf0\x9f\x98\x80";
                jbyteArray caption = (*thread_env)->NewByteArray(thread_env, sizeof title - 1), text = (*thread_env)->NewByteArray(thread_env, sizeof initial - 1);
                (*thread_env)->SetByteArrayRegion(thread_env, caption, 0, sizeof title - 1, (const jbyte *)title);
                (*thread_env)->SetByteArrayRegion(thread_env, text, 0, sizeof initial - 1, (const jbyte *)initial);
                jbyteArray reply = (*thread_env)->CallObjectMethod(thread_env, activity->clazz, keyboard, caption, text, 32);
                if (reply) {
                    keyboard_bytes = (*thread_env)->GetArrayLength(thread_env, reply);
                    jbyte *value = (*thread_env)->GetByteArrayElements(thread_env, reply, NULL);
                    keyboard_crc = crc32(0, (const unsigned char *)value, keyboard_bytes);
                    (*thread_env)->ReleaseByteArrayElements(thread_env, reply, value, JNI_ABORT);
                    (*thread_env)->DeleteLocalRef(thread_env, reply);
                }
                (*thread_env)->DeleteLocalRef(thread_env, caption); (*thread_env)->DeleteLocalRef(thread_env, text);
            }
        }
    }
    snprintf(report, sizeof report, "%s/asset-probe.txt", activity->externalDataPath);
    FILE *file = fopen(report, "w");
    if (file) { fprintf(file, "result=%d\nroot=%s\nerror=%s\nbytes=%llu\ncrc=%lu\nsettings_before=%s\ndeleted=%d\nkeyboard_bytes=%d\nkeyboard_crc=%lu\n", result, result >= 0 ? root : "", error, bytes, crc, settings, deleted, keyboard_bytes, keyboard_crc); fclose(file); }
    jstring message = result < 0 ? (*thread_env)->NewStringUTF(thread_env, error) : NULL;
    if (ready) (*thread_env)->CallVoidMethod(thread_env, activity->clazz, ready, message);
    if (message) (*thread_env)->DeleteLocalRef(thread_env, message);
    (*thread_env)->DeleteLocalRef(thread_env, type);
    (*activity->vm)->DetachCurrentThread(activity->vm);
    return NULL;
}
static void destroy(ANativeActivity *act) { (void)act; _exit(0); }
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *act, void *state, size_t size) {
    (void)state; (void)size;
    activity = act; act->callbacks->onDestroy = destroy; act->callbacks->onNativeWindowCreated = window_created;
    act->callbacks->onInputQueueCreated = input_created; act->callbacks->onInputQueueDestroyed = input_destroyed;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    pthread_t worker; if (!pthread_create(&worker, NULL, run, NULL)) pthread_detach(worker);
}
