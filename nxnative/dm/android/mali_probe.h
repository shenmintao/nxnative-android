// Mali investigation selectors (debug builds only; release has no selector or format change), and the one Mali default they established: robustBufferAccess.
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
// Core robustBufferAccess on an ARM (Mali) device that reports it, release builds included. Without it the BotW Plateau world reads out of
// range and the GPU faults: thousands of Mali GROUP_ERROR_FATAL READ translation faults and a black world (V2405A, Immortalis-G925 r49p1;
// 2026-11-04 the same APK renders the world with it and stays black without it). Which read is out of range is not identified. Other
// vendors keep it off. DM_VK_X_ROBUST=0 (start-up, debug builds): off on Mali too, for comparisons
static inline int vk_mali_robust(uint32_t vendor, uint32_t supported) {
    if (vendor != 0x13B5 || !supported) return 0;
#ifndef DM_RELEASE
    const char *e = getenv("DM_VK_X_ROBUST"); if (e && !strcmp(e, "0")) return 0;
#endif
    return 1;
}
static inline int vk_mali_a_eligible(int enabled, uint32_t fmt, uint32_t features, uint32_t samples, uint32_t depth, uint32_t layers) {
    return enabled && fmt == VK_FORMAT_R32G32B32A32_SFLOAT && !(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && samples == 1 && depth == 1 && layers == 1;
}
#endif
