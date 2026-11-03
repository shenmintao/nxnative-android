"""Per-image gates, counting only valid texels, with explicit BC5 channel semantics."""
import math


def quality(reference, decoded, mask, normal=False):
    import numpy as np
    a = reference[mask].astype(np.float32) / 255
    b = decoded[mask].astype(np.float32) / 255
    mse = np.mean((a - b) ** 2, axis=0, dtype=np.float64)
    psnr = lambda v: 10 * math.log10(1 / v) if v else float('inf')
    result = dict(rgb=psnr(float(mse[:3].mean())), alpha=psnr(float(mse[3])), rg=psnr(float(mse[:2].mean())))
    result['exception'] = result['rgb'] < 38 or result['alpha'] < 38 or (normal and result['rg'] < 38)
    if normal:
        def vectors(p):
            xy = p[:, :2] * 2 - 1
            v = np.concatenate((xy, np.sqrt(np.maximum(0, 1 - np.sum(xy * xy, axis=1)))[:, None]), axis=1)
            return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-10)
        angles = np.degrees(np.arccos(np.clip(np.sum(vectors(a) * vectors(b), axis=1), -1, 1)))
        result.update(angular_median=float(np.median(angles)), angular_p95=float(np.percentile(angles, 95)))
        # Angular error is reported, not assigned an unapproved numerical threshold.
        if not np.isfinite(angles).all(): result['exception'] = True
    return result


def masks_and_edges(pixels, subresources):
    import numpy as np
    mask = np.zeros(pixels.shape[:3], bool)
    for r in subresources:
        w, h, off, n = r['width'], r['height'], r['offset'], r['blocks']
        bx, by = (w + 3) // 4, (h + 3) // 4
        v = pixels[off:off + n].reshape(by, bx, 4, 4, 4)
        m = mask[off:off + n].reshape(by, bx, 4, 4)
        for y in range(4):
            for x in range(4): m[:, :, y, x] = (np.arange(by)[:, None] * 4 + y < h) & (np.arange(bx)[None, :] * 4 + x < w)
        if w % 4: v[:, -1, :, w % 4:, :] = v[:, -1, :, w % 4 - 1:w % 4, :]
        if h % 4: v[-1, :, h % 4:, :, :] = v[-1, :, h % 4 - 1:h % 4, :, :]
    return mask
