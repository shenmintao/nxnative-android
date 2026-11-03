// Investigation only: all switches compile out of DM_RELEASE. No fallback is
// selected by default. C disables unsupported blending solely as a reference.
#pragma once
#include <stdlib.h>
#include <string.h>
static inline int vk_fp32_probe_mode(void) {
#ifdef DM_RELEASE
    return 0;
#else
    const char *e = getenv("DM_VK_X_FP32_BLEND");
    return e && !strcmp(e, "C") ? 3 : e && !strcmp(e, "log") ? 1 : 0;
#endif
}
