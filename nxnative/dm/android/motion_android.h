// Android SensorEvent -> the Switch screen frame. Pure helpers, tested without a device.
// Android acceleration is specific force in m/s^2 (+g face up); HID uses gravity in G (-1 face up).
// Display.getRotation() describes the graphics rotation, opposite to the device's physical turn:
// https://developer.android.com/reference/android/view/Display#getRotation()
#pragma once
#include <math.h>
#include <stdint.h>

static inline void motion_android_axes(int rotation, const float in[3], float out[3]) {
    switch (rotation) {
    case 1: out[0] = -in[1]; out[1] = in[0]; break;  // device turned counter-clockwise
    case 2: out[0] = -in[0]; out[1] = -in[1]; break;
    case 3: out[0] = in[1]; out[1] = -in[0]; break;
    default: out[0] = in[0]; out[1] = in[1]; break;
    }
    out[2] = in[2];
}
static inline int motion_android_vectors(int rotation, const float raw_acc[3], const float raw_gyro[3], float acc[3], float gyro[3]) {
    for (int i = 0; i < 3; i++) if (!isfinite(raw_acc[i]) || !isfinite(raw_gyro[i])) return 0;
    motion_android_axes(rotation, raw_acc, acc);
    motion_android_axes(rotation, raw_gyro, gyro);  // rad/s; motion_fusion.h converts to revolutions/s
    for (int i = 0; i < 3; i++) acc[i] /= -9.80665f;
    return 1;
}
// Sensor timestamps include suspend (BOOTTIME); now_ns() does not (MONOTONIC).
// Preserve the sample's age, rather than integrating several queued samples at their arrival times.
static inline uint64_t motion_android_time(uint64_t now, uint64_t boot, int64_t sample) {
    if (sample <= 0) return 0;
    uint64_t age = (uint64_t)sample < boot ? boot - (uint64_t)sample : 0;
    return age < now ? now - age : 0;
}
