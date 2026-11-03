// Physical phone controls and deterministic spacing, shared by drawing/touch.
// The tablet's existing geometry remains the reference at 100% button size.
#ifndef DM_ANDROID_OVERLAY_GEOMETRY_H
#define DM_ANDROID_OVERLAY_GEOMETRY_H
static inline float ov_geo_radius(const ov_ctl *c, const float b[4]) {
    float m = fminf(b[2], b[3]); return c->w == c->h || c->kind == OV_MENU ? m : m * 0.35f;
}
static inline float ov_geo_clearance(const ov_ctl *a, const float aa[4], const ov_ctl *b, const float bb[4]) {
    // Signed distance to the Minkowski sum of two rounded rectangles. Circle
    // controls may have overlapping bounding squares without touching visually.
    float r = ov_geo_radius(a, aa) + ov_geo_radius(b, bb);
    float x = fabsf(aa[0] - bb[0]) - aa[2] - bb[2] + r, y = fabsf(aa[1] - bb[1]) - aa[3] - bb[3] + r;
    return hypotf(fmaxf(x, 0), fmaxf(y, 0)) + fminf(fmaxf(x, y), 0) - r;
}
static inline void ov_geo_clamp(float b[4], const float safe[4], float margin) {
    for (int k = 0; k < 2; k++) {
        float lo = safe[k] + margin + b[2 + k], hi = safe[2 + k] - margin - b[2 + k];
        b[k] = lo <= hi ? fmaxf(lo, fminf(hi, b[k])) : (safe[k] + safe[2 + k]) * 0.5f;
    }
}
static inline void ov_geometry(const ov_ctl *layout, int n, int phone, float W, float H, float px_mm,
                               float legacy_scale, float percent, const float insets[4], const float custom[][3], float boxes[][4]) {
    float safe[4] = {insets[0], insets[1], W - insets[2], H - insets[3]};
    float margin = phone ? 2 * px_mm : 0, gap = 1.5f * px_mm;
    for (int i = 0; i < n; i++) {
        const ov_ctl *c = layout + i; float *b = boxes[i], scale = custom[i][2] * percent / 100;
        b[0] = custom[i][0] >= 0 ? custom[i][0] * W : c->ax * W + c->x * H;
        b[1] = custom[i][1] >= 0 ? custom[i][1] * H : c->y * H;
        if (phone) {
            float mm = c->kind == OV_STICK_L ? 18 : c->kind == OV_STICK_R || c->kind == OV_DPAD ? 16 :
                c->kind == OV_MENU ? 8 : c->w != c->h ? 6.5f : c->w < 0.12f ? 8 : 10;
            b[3] = mm * px_mm * scale * 0.5f; b[2] = b[3] * c->w / c->h;
        } else {
            // Keep the exact old operation order for default tablet geometry.
            b[2] = c->w * H * legacy_scale * custom[i][2] / 2;
            b[3] = c->h * H * legacy_scale * custom[i][2] / 2;
            if (percent != 100) { b[2] *= percent / 100; b[3] *= percent / 100; }
        }
        ov_geo_clamp(b, safe, margin);
    }
    if (!phone) return;
    // Separate the actual rounded footprints along their distance gradient.
    // Bounds are enforced after each push; physical sizes are never reduced.
    // This runs only when cached layout inputs change, not once per draw.
    for (int pass = 0; pass < 1024; pass++) {
        int moved = 0;
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
            float *a = boxes[i], *b = boxes[j], distance = ov_geo_clearance(layout + i, a, layout + j, b);
            if (distance >= gap - 0.01f) continue;
            float r = ov_geo_radius(layout + i, a) + ov_geo_radius(layout + j, b);
            float qx = fabsf(a[0] - b[0]) - a[2] - b[2] + r, qy = fabsf(a[1] - b[1]) - a[3] - b[3] + r;
            float gx = fmaxf(qx, 0), gy = fmaxf(qy, 0), length = hypotf(gx, gy);
            if (length > 0) { gx /= length; gy /= length; } else { gx = qx >= qy; gy = 1 - gx; }
            gx *= a[0] <= b[0] ? -1 : 1; gy *= a[1] <= b[1] ? -1 : 1;
            float amount = gap - distance + 0.02f, ax = a[0], ay = a[1];
            a[0] += gx * amount * 0.5f; a[1] += gy * amount * 0.5f; ov_geo_clamp(a, safe, margin);
            float actual = (a[0] - ax) * gx + (a[1] - ay) * gy;
            b[0] -= gx * (amount - actual); b[1] -= gy * (amount - actual); ov_geo_clamp(b, safe, margin); moved = 1;
        }
        if (!moved) break;
    }
}
#endif
