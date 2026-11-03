// This flag is module-local: PPE2 already contains clip_neg1 and stays 1000B.
#ifndef DM_ANDROID_VK_DEPTH_VARIANT_H
#define DM_ANDROID_VK_DEPTH_VARIANT_H
static inline int vk_depth_variant_stage(int negative_one, int extension, int geometry, int stage) {
    return negative_one && !extension && stage == (geometry ? 2 : 0);
}
#endif
