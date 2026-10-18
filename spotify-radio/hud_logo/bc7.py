"""Minimal BC7 encoder: every block in mode 6 (one subset, RGBA 7-bit endpoints + p-bit, 4-bit
indices). Endpoints from the block's principal axis, refined by least squares. Good enough for
flat-colour logos; quality is checked by decoding with Pillow."""
import numpy as np

W6 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64])


def _quantize(endpoint):  # float RGBA 0..255 -> (7-bit RGBA, p-bit) minimising error
    best = None
    for p in (0, 1):
        q = np.clip(np.round((endpoint - p) / 2), 0, 127).astype(int)
        err = np.abs(((q << 1) | p) - endpoint).sum()
        if best is None or err < best[0]:
            best = (err, q, p)
    return best[1], best[2]


def _palette(q0, p0, q1, p1):
    e0 = ((q0 << 1) | p0).astype(int)
    e1 = ((q1 << 1) | p1).astype(int)
    return ((64 - W6)[:, None] * e0 + W6[:, None] * e1 + 32) >> 6  # 16 x 4


def _encode_block(px):  # px: 16 x 4 float
    if (px[:, 3] == 0).all():
        px = np.zeros_like(px)
    mean = px.mean(axis=0)
    centred = px - mean
    cov = centred.T @ centred
    axis = np.linalg.eigh(cov)[1][:, -1] if np.abs(cov).sum() > 1e-6 else np.array([1.0, 1, 1, 1]) / 2
    t = centred @ axis
    lo, hi = mean + axis * t.min(), mean + axis * t.max()
    best = None
    for _ in range(3):
        q0, p0 = _quantize(np.clip(lo, 0, 255))
        q1, p1 = _quantize(np.clip(hi, 0, 255))
        pal = _palette(q0, p0, q1, p1)
        idx = np.abs(px[:, None, :] - pal[None, :, :]).sum(axis=2).argmin(axis=1)
        err = np.abs(px - pal[idx]).sum()
        if best is None or err < best[0]:
            best = (err, q0, p0, q1, p1, idx.copy())
        w = W6[idx] / 64.0                       # least-squares refit of the two endpoints
        a = np.stack([1 - w, w], axis=1)
        if np.linalg.matrix_rank(a) < 2:
            break
        sol = np.linalg.lstsq(a, px, rcond=None)[0]
        lo, hi = sol[0], sol[1]
    _, q0, p0, q1, p1, idx = best
    if idx[0] >= 8:                              # anchor index must fit in 3 bits: swap endpoints
        q0, p0, q1, p1 = q1, p1, q0, p0
        idx = 15 - idx
    bits, n = 0, 0

    def put(value, width):
        nonlocal bits, n
        bits |= (int(value) & ((1 << width) - 1)) << n
        n += width

    put(1 << 6, 7)                               # mode 6
    for c in range(4):
        put(q0[c], 7); put(q1[c], 7)
    put(p0, 1); put(p1, 1)
    put(idx[0], 3)
    for i in idx[1:]:
        put(i, 4)
    assert n == 128
    return bits.to_bytes(16, "little")


def encode(rgba):  # PIL RGBA image, size multiple of 4 -> BC7 bytes (row-major blocks)
    a = np.asarray(rgba).astype(float)
    h, w = a.shape[:2]
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            out += _encode_block(a[by:by + 4, bx:bx + 4].reshape(16, 4))
    return bytes(out)
