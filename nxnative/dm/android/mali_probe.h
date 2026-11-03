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
static inline int vk_mali_a_eligible(int enabled, uint32_t fmt, uint32_t features, uint32_t samples, uint32_t depth, uint32_t layers) {
    return enabled && fmt == VK_FORMAT_R32G32B32A32_SFLOAT && !(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && samples == 1 && depth == 1 && layers == 1;
}
#endif
