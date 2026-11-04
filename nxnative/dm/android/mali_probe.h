// User-directed black-output investigation only. Release has no selector or format change.
#ifndef DM_ANDROID_MALI_PROBE_H
#define DM_ANDROID_MALI_PROBE_H
static inline int vk_mali_a_mode(void) {
#ifdef DM_RELEASE
    return 0;
#else
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_VK_X_MALI_A"); mode = e && atoi(e) == 1; } return mode;
#endif
}
static inline int vk_mali_u_mode(void) {
#ifdef DM_RELEASE
    return 0;
#else
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_VK_X_SAMPLE_CAPS"); mode = e ? !strcmp(e, "nearest") ? 2 : !strcmp(e, "log") ? 1 : 0 : 0; } return mode;
#endif
}
// DM_VK_X_ROBUST=1 (start-up): core robustBufferAccess on an ARM (Mali) device that reports it. The Plateau world logged thousands of
// Mali GROUP_ERROR_FATAL READ translation faults (V2405A, Immortalis-G925 r49p1); this tests whether they are out-of-range buffer reads.
static inline int vk_mali_robust(uint32_t vendor, uint32_t supported) {
#ifdef DM_RELEASE
    (void)vendor; (void)supported; return 0;
#else
    const char *e = getenv("DM_VK_X_ROBUST"); return e && atoi(e) == 1 && vendor == 0x13B5 && supported;
#endif
}
static inline int vk_mali_a_eligible(int enabled, uint32_t fmt, uint32_t features, uint32_t samples, uint32_t depth, uint32_t layers) {
    return enabled && fmt == VK_FORMAT_R32G32B32A32_SFLOAT && !(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && samples == 1 && depth == 1 && layers == 1;
}
#endif
