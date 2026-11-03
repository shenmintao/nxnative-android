#pragma once
#include <stdint.h>
typedef struct AAudioStream AAudioStream;
typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef int32_t aaudio_result_t, aaudio_stream_state_t, aaudio_data_callback_result_t;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *, void *, void *, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream *, void *, aaudio_result_t);
enum { AAUDIO_OK=0, AAUDIO_ERROR_DISCONNECTED=-899, AAUDIO_CALLBACK_RESULT_CONTINUE=0, AAUDIO_CALLBACK_RESULT_STOP=1,
 AAUDIO_STREAM_STATE_UNKNOWN=0, AAUDIO_STREAM_STATE_STARTED=4, AAUDIO_STREAM_STATE_STOPPED=10, AAUDIO_STREAM_STATE_CLOSED=12,
 AAUDIO_STREAM_STATE_DISCONNECTED=13, AAUDIO_DIRECTION_OUTPUT=0, AAUDIO_FORMAT_PCM_FLOAT=2, AAUDIO_FORMAT_PCM_I16=1,
 AAUDIO_PERFORMANCE_MODE_LOW_LATENCY=12, AAUDIO_SHARING_MODE_SHARED=1, AAUDIO_USAGE_GAME=14, AAUDIO_CONTENT_TYPE_MUSIC=2 };
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *, AAudioStream_dataCallback, void *);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *, AAudioStream_errorCallback, void *);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *, AAudioStream **);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *);
aaudio_result_t AAudioStream_requestStart(AAudioStream *);
aaudio_result_t AAudioStream_requestStop(AAudioStream *);
aaudio_result_t AAudioStream_close(AAudioStream *);
aaudio_stream_state_t AAudioStream_getState(AAudioStream *);
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *, aaudio_stream_state_t, aaudio_stream_state_t *, int64_t);
