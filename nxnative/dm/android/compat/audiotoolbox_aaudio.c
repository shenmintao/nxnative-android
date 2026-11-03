// Android port: an AudioUnit output unit on AAudio (dm/android/compat/include/AudioToolbox/AudioToolbox.h).
// The stream asks for low latency and the unit's own format (interleaved 16-bit or float PCM at its rate and channel count); AAudio's data callback
// calls the unit's render callback with one AudioBuffer over AAudio's buffer, so the host's pull code (hle_audio.c pull_device) is unchanged.
// Disconnection is signalled to a per-unit control thread. It stops/closes/reopens
// outside both callbacks (Android AAudio/Oboe guidance); the host watchdog remains a fallback.
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <errno.h>

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int have_fmt;
    AURenderCallbackStruct cb;
    int32_t frames_hint;
    AAudioStream *st; // stream APIs are serialized under control; callbacks never take it
    _Atomic(AAudioStream *) active;
    pthread_mutex_t control; pthread_t worker; sem_t wake;
    _Atomic int quitting, wanted;
    _Atomic uint64_t last_callback, fault_at, resumed_at, reconnects, quick_reconnects, disconnects;
    uint64_t retry_at; unsigned retry_ms; int awaiting, gap_reported;
    _Atomic uint64_t sample_time;
    uint64_t stop_time;    // sample_time at the unit's last stop: unchanged at the next start = the stream stopped calling back (a stall), so it is opened again
    _Atomic int failed;    // the error callback came (disconnected): the next start opens a new stream
    _Atomic int held, in_cb;   // held: stopped, the stream calls back with silence and never into the host (AudioOutputUnitStop); in_cb: data callbacks running
    _Atomic uint64_t held_frames;  // silence played since the stop: past HOLD_MAX the stream stops itself
};
static struct dm_audio_component g_output;
// 100ms is an error-budget threshold, not an inaudibility claim: twenty 5ms
// callback quanta, half the existing 200ms watchdog. Measure last-to-first data
// callback spacing conservatively; device buffering/audibility need a phone test.
enum { RECONNECT_GAP_MS = 100, RECONNECT_TICK_MS = 20, RECONNECT_BACKOFF_MS = 25, RECONNECT_BACKOFF_MAX_MS = 1000 };
static uint64_t audio_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void *recover_stream(void *arg);

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return kAudio_UnimplementedError;
    AudioUnit u = calloc(1, sizeof *u); if (!u) return -1;
    atomic_store(&u->held, 1);
    if (pthread_mutex_init(&u->control, NULL)) { free(u); return -1; }
    if (sem_init(&u->wake, 0, 0)) { pthread_mutex_destroy(&u->control); free(u); return -1; }
    if (pthread_create(&u->worker, NULL, recover_stream, u)) { sem_destroy(&u->wake); pthread_mutex_destroy(&u->control); free(u); return -1; }
    *out = u; return noErr;
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    switch (id) {
    case kAudioUnitProperty_StreamFormat:
        if (size < sizeof u->fmt) return -50;
        memcpy(&u->fmt, data, sizeof u->fmt);
        if (u->fmt.mFormatID != kAudioFormatLinearPCM || (u->fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return kAudioUnitErr_FormatNotSupported;
        if (!(u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) && u->fmt.mBitsPerChannel != 16) return kAudioUnitErr_FormatNotSupported;
        u->have_fmt = 1; return noErr;
    case kAudioUnitProperty_SetRenderCallback: if (size < sizeof u->cb) return -50; memcpy(&u->cb, data, sizeof u->cb); return noErr;
    case kAudioDevicePropertyBufferFrameSize: if (size >= 4) u->frames_hint = (int32_t)*(const UInt32 *)data; return noErr;
    default: return noErr;
    }
}
OSStatus AudioUnitInitialize(AudioUnit u) { return u->have_fmt && u->cb.inputProc ? noErr : -50; }

enum { HOLD_MAX_S = 60 };   // a stop longer than this (the app in the background) ends the stream: the next start opens a new one
static aaudio_data_callback_result_t on_data(AAudioStream *st, void *ud, void *audio, int32_t frames) {
    AudioUnit u = ud;
    atomic_fetch_add(&u->in_cb, 1);
    if (atomic_load(&u->held) || !atomic_load(&u->wanted) || st != atomic_load(&u->active)) {   // stopped: silence, and the host's output (which it may have freed) is not touched
        memset(audio, 0, (size_t)frames * u->fmt.mBytesPerFrame); atomic_fetch_add(&u->sample_time, (uint64_t)frames);
        int end = atomic_fetch_add(&u->held_frames, (uint64_t)frames) + (uint64_t)frames > (uint64_t)(u->fmt.mSampleRate * HOLD_MAX_S) || atomic_load(&u->quitting);
        atomic_fetch_sub(&u->in_cb, 1); (void)st; return end ? AAUDIO_CALLBACK_RESULT_STOP : AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint64_t stamp = audio_now(), zero = 0;
    atomic_store(&u->last_callback, stamp);
    if (atomic_load(&u->fault_at)) atomic_compare_exchange_strong(&u->resumed_at, &zero, stamp);
    AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)frames * u->fmt.mBytesPerFrame, audio}}};
    AudioUnitRenderActionFlags fl = 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    AudioTimeStamp ts = {.mSampleTime = (Float64)atomic_fetch_add(&u->sample_time, (uint64_t)frames), .mHostTime = (UInt64)t.tv_sec * 1000000000ull + (UInt64)t.tv_nsec};
    if (u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl)) memset(audio, 0, (size_t)frames * u->fmt.mBytesPerFrame);
    atomic_fetch_sub(&u->in_cb, 1); (void)st; return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
// Real-time callbacks only publish atomics and signal a semaphore: no allocation,
// logging, mutex, stream API, stop, close or reopen on either callback thread.
static void request_recovery(AudioUnit u, aaudio_result_t error) {
    int empty = 0;
    int fresh = atomic_compare_exchange_strong(&u->failed, &empty, error ? error : AAUDIO_ERROR_DISCONNECTED);
    if (fresh) atomic_fetch_add(&u->disconnects, 1);
    if (atomic_load(&u->wanted)) {
        uint64_t first = atomic_load(&u->last_callback), zero = 0;
        if (!first) first = audio_now();
        atomic_compare_exchange_strong(&u->fault_at, &zero, first);
        if (fresh) sem_post(&u->wake);
    }
}
static void on_error(AAudioStream *st, void *ud, aaudio_result_t err) {
    AudioUnit u = ud;
    if (st == atomic_load(&u->active) && !atomic_load(&u->quitting)) request_recovery(u, err);
}
// requestStop only asks: the data callback may still be running after it returns, and the host frees the output it pulls from once the unit is stopped
// (hle_audio.c au_close; games close and open outputs when the operation mode changes). So ending a stream (uninitialize, dispose, opening a new one) waits
// until it is stopped, and a stream that does not get there within a second is closed
static void close_stream(AudioUnit u) { if (u->st) { atomic_store(&u->active, NULL); AAudioStream_close(u->st); u->st = NULL; } }
static void stop_stream(AudioUnit u) {
    if (!u->st) return;
    aaudio_stream_state_t s = AAUDIO_STREAM_STATE_UNKNOWN;
    if (AAudioStream_requestStop(u->st) == AAUDIO_OK) {
        s = AAudioStream_getState(u->st);
        for (int i = 0; i < 10 && s != AAUDIO_STREAM_STATE_STOPPED && s != AAUDIO_STREAM_STATE_DISCONNECTED && s != AAUDIO_STREAM_STATE_CLOSED; i++)
            if (AAudioStream_waitForStateChange(u->st, s, &s, 100000000) != AAUDIO_OK) break;
    }
    if (s != AAUDIO_STREAM_STATE_STOPPED) close_stream(u);
}

// Call only under control, never from a callback. A stale callback sees active
// cleared/held and cannot enter the host while an old stream is being closed.
static OSStatus open_stream(AudioUnit u) {
    atomic_store(&u->held, 1); stop_stream(u); close_stream(u);
    atomic_store(&u->resumed_at, 0); // after old callbacks quiesce, including failed opens
    atomic_store(&u->failed, 0);
    AAudioStreamBuilder *b; aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)u->fmt.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)u->fmt.mChannelsPerFrame);
    AAudioStreamBuilder_setFormat(b, u->fmt.mFormatFlags & kAudioFormatFlagIsFloat ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    // 5 ms callbacks unless the host asks otherwise, as the iOS front end sets its I/O buffer (docs/agents/03-bringup.md "Crackling on iOS only"): left to itself AAudio
    // on the Legion Y700 called back with 6-960 frames, more than Tales of Eternia Remastered's CRI output keeps queued (about 2 blocks), so 2600 of 2000 callbacks
    // underran and 11% of the sound was zero padding; with 240 frames, 0 underruns
    AAudioStreamBuilder_setFramesPerDataCallback(b, u->frames_hint > 0 ? u->frames_hint : (int32_t)(u->fmt.mSampleRate / 200));
    AAudioStreamBuilder_setDataCallback(b, on_data, u);
    AAudioStreamBuilder_setErrorCallback(b, on_error, u);
    r = AAudioStreamBuilder_openStream(b, &u->st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { u->st = NULL; return r; }
    // the device's own rate and channel count must be ours: AAudio converts when they differ (it reports the requested format back)
    atomic_store(&u->active, u->st);
    r = AAudioStream_requestStart(u->st);
    if (r != AAUDIO_OK) { close_stream(u); return r; }
    return noErr;
}

static void reconnect_result(AudioUnit u, uint64_t now) {
    uint64_t first = atomic_load(&u->fault_at);
    if (!first) return;
    uint64_t resumed = atomic_load(&u->resumed_at);
    double ms = ((resumed >= first ? resumed : now) - first) / 1e6;
    if (ms > RECONNECT_GAP_MS && !u->gap_reported) {
        u->gap_reported = 1;
        fprintf(stderr, "[dm] [RED] AAudio %p: callback gap exceeds %dms during reconnect (%.1fms)\n", (void *)u, RECONNECT_GAP_MS, ms);
    }
    if (u->awaiting && resumed >= first && !atomic_load(&u->failed)) {
        uint64_t n = atomic_fetch_add(&u->reconnects, 1) + 1;
        if (ms <= RECONNECT_GAP_MS) atomic_fetch_add(&u->quick_reconnects, 1);
        fprintf(stderr, "[dm] [WARN] AAudio %p: reconnect %llu received data, callback gap %.1fms, disconnects %llu, quick recoveries %llu\n", (void *)u,
            (unsigned long long)n, (resumed - first) / 1e6, (unsigned long long)atomic_load(&u->disconnects), (unsigned long long)atomic_load(&u->quick_reconnects));
        atomic_store(&u->fault_at, 0); u->awaiting = u->gap_reported = 0; u->retry_ms = 0; u->retry_at = 0;
    }
}
static void *recover_stream(void *arg) {
    AudioUnit u = arg;
    while (!atomic_load(&u->quitting)) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_nsec += RECONNECT_TICK_MS * 1000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        while (sem_timedwait(&u->wake, &until) && errno == EINTR) {}
        pthread_mutex_lock(&u->control);
        uint64_t now = audio_now();
        if (!atomic_load(&u->quitting) && atomic_load(&u->wanted)) {
            if (u->st) {
                aaudio_stream_state_t state = AAudioStream_getState(u->st);
                if (state == AAUDIO_STREAM_STATE_DISCONNECTED || state == AAUDIO_STREAM_STATE_CLOSED || state == AAUDIO_STREAM_STATE_STOPPED)
                    request_recovery(u, AAUDIO_ERROR_DISCONNECTED);
            }
            if (atomic_load(&u->failed) && u->awaiting) {
                u->awaiting = 0; u->retry_ms = u->retry_ms ? u->retry_ms * 2 : RECONNECT_BACKOFF_MS;
                if (u->retry_ms > RECONNECT_BACKOFF_MAX_MS) u->retry_ms = RECONNECT_BACKOFF_MAX_MS;
                u->retry_at = now + u->retry_ms * 1000000ull;
                fprintf(stderr, "[dm] [RED] AAudio %p: reopened stream disconnected before recovery completed, retry in %ums\n", (void *)u, u->retry_ms);
            }
            if (atomic_load(&u->failed) && now >= u->retry_at) {
                // On error_callback failure is already published. Preserve the
                // original gap across repeated disconnect/open failures.
                if (!atomic_load(&u->fault_at)) request_recovery(u, atomic_load(&u->failed));
                OSStatus status = open_stream(u);
                if (status) {
                    atomic_store(&u->failed, status); u->awaiting = 0;
                    u->retry_ms = u->retry_ms ? u->retry_ms < RECONNECT_BACKOFF_MAX_MS ? u->retry_ms * 2 : RECONNECT_BACKOFF_MAX_MS : RECONNECT_BACKOFF_MS;
                    if (u->retry_ms > RECONNECT_BACKOFF_MAX_MS) u->retry_ms = RECONNECT_BACKOFF_MAX_MS;
                    u->retry_at = audio_now() + u->retry_ms * 1000000ull;
                    fprintf(stderr, "[dm] [RED] AAudio %p: reconnect failed (%d), retry in %ums\n", (void *)u, (int)status, u->retry_ms);
                } else {
                    u->awaiting = 1;
                    if (atomic_load(&u->wanted)) atomic_store(&u->held, 0);
                }
            }
            reconnect_result(u, audio_now());
        }
        pthread_mutex_unlock(&u->control);
    }
    return NULL;
}
OSStatus AudioOutputUnitStart(AudioUnit u) {
    pthread_mutex_lock(&u->control);
    // Preserve a live track through ordinary menu/background holds: repeatedly
    // creating mixer tracks changed loudness/underruns on the existing tablet.
    int reuse = u->st && !atomic_load(&u->failed) && AAudioStream_getState(u->st) == AAUDIO_STREAM_STATE_STARTED &&
        (!atomic_load(&u->held) || atomic_load(&u->sample_time) != u->stop_time);
    OSStatus status = reuse ? noErr : open_stream(u);
    if (!status) { atomic_store(&u->wanted, 1); atomic_store(&u->held, 0); }
    pthread_mutex_unlock(&u->control); return status;
}
// A stop holds the stream (silence, see AudioOutputUnitStart): once it returns no callback is inside the host's render callback, and none enters it until
// the next start (held is set before in_cb is read, and each callback counts itself in before it reads held)
OSStatus AudioOutputUnitStop(AudioUnit u) {
    atomic_store(&u->wanted, 0); atomic_store(&u->held, 1);
    pthread_mutex_lock(&u->control);
    atomic_store(&u->held_frames, 0);
    while (atomic_load(&u->in_cb)) sched_yield();
    u->stop_time = atomic_load(&u->sample_time);
    atomic_store(&u->fault_at, 0); u->awaiting = u->gap_reported = 0; u->retry_at = u->retry_ms = 0;
    pthread_mutex_unlock(&u->control); return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) {
    AudioOutputUnitStop(u); pthread_mutex_lock(&u->control); stop_stream(u); close_stream(u); pthread_mutex_unlock(&u->control); return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) {
    if (!u) return noErr;
    atomic_store(&u->wanted, 0); atomic_store(&u->held, 1); atomic_store(&u->quitting, 1); sem_post(&u->wake);
    pthread_join(u->worker, NULL); // callbacks never wait for this thread or take control
    pthread_mutex_lock(&u->control); stop_stream(u); close_stream(u); pthread_mutex_unlock(&u->control);
    sem_destroy(&u->wake); pthread_mutex_destroy(&u->control); free(u); return noErr;
}
// AudioConverter (AAC -> PCM) is on MediaCodec: dm/android/compat/audioconverter_mediacodec.c
