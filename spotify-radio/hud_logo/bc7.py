"""Minimal BC7 encoder: mode 6 (one subset, RGBA 7-bit endpoints + p-bit, 4-bit indices), and mode 7
(two subsets, RGBA 5-bit endpoints + p-bits, 2-bit indices) for blocks where three colours meet,
such as a red plug against its silver collar. Endpoints from each subset's principal axis, refined
by least squares. Good enough for flat-colour logos; quality is checked by decoding with Pillow."""
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
    return bits.to_bytes(16, "little"), best[0]


# BC7's two-subset partitions (bit i set: pixel i is in subset 1) and subset 1's anchor pixel.
# Read back from Pillow's BC7 decoder; they match the format's tables.
PARTITIONS2 = [0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800,
               0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce,
               0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc,
               0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
               0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718,
               0xccf0, 0x0fcc, 0x7744, 0xee22]
ANCHOR2 = [15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8,
           8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15,
           2, 2, 15]
W7 = np.array([0, 21, 43, 64])


def _quantize5(endpoint):  # float RGBA 0..255 -> (5-bit RGBA, p-bit, the 8-bit values they decode to)
    best = None
    for p in (0, 1):
        q = np.clip(np.round((endpoint * 63 / 255 - p) / 2), 0, 31).astype(int)
        e6 = (q << 1) | p
        e8 = (e6 << 2) | (e6 >> 4)
        err = np.abs(e8 - endpoint).sum()
        if best is None or err < best[0]:
            best = (err, q, p, e8)
    return best[1], best[2], best[3]


def _fit_subset(px):  # n x 4 -> (error, q0, p0, q1, p1, indices)
    mean = px.mean(axis=0)
    centred = px - mean
    cov = centred.T @ centred
    axis = np.linalg.eigh(cov)[1][:, -1] if np.abs(cov).sum() > 1e-6 else np.array([1.0, 1, 1, 1]) / 2
    t = centred @ axis
    lo, hi = mean + axis * t.min(), mean + axis * t.max()
    best = None
    for _ in range(3):
        q0, p0, e0 = _quantize5(np.clip(lo, 0, 255))
        q1, p1, e1 = _quantize5(np.clip(hi, 0, 255))
        pal = ((64 - W7)[:, None] * e0 + W7[:, None] * e1 + 32) >> 6
        idx = np.abs(px[:, None, :] - pal[None, :, :]).sum(axis=2).argmin(axis=1)
        err = np.abs(px - pal[idx]).sum()
        if best is None or err < best[0]:
            best = (err, q0, p0, q1, p1, idx.copy())
        w = W7[idx] / 64.0
        a = np.stack([1 - w, w], axis=1)
        if np.linalg.matrix_rank(a) < 2:
            break
        sol = np.linalg.lstsq(a, px, rcond=None)[0]
        lo, hi = sol[0], sol[1]
    return best


def _encode_block7(px):  # the best two-subset split: (bytes, error)
    best = None
    for part, mask in enumerate(PARTITIONS2):
        subset = np.array([(mask >> i) & 1 for i in range(16)])
        fits = [_fit_subset(px[subset == s]) for s in (0, 1)]
        err = fits[0][0] + fits[1][0]
        if best is None or err < best[0]:
            best = (err, part, subset, fits)
    err, part, subset, fits = best
    ends, indices = [], np.zeros(16, int)
    for s, anchor in ((0, 0), (1, ANCHOR2[part])):
        _, q0, p0, q1, p1, idx = fits[s]
        where = np.flatnonzero(subset == s)
        if idx[list(where).index(anchor)] >= 2:  # an anchor index has one bit fewer: swap the endpoints
            q0, p0, q1, p1 = q1, p1, q0, p0
            idx = 3 - idx
        indices[where] = idx
        ends.append(((q0, p0), (q1, p1)))
    bits, n = 0, 0

    def put(value, width):
        nonlocal bits, n
        bits |= (int(value) & ((1 << width) - 1)) << n
        n += width

    put(1 << 7, 8)                               # mode 7
    put(part, 6)
    for c in range(4):
        for s in (0, 1):
            put(ends[s][0][0][c], 5); put(ends[s][1][0][c], 5)
    for s in (0, 1):
        put(ends[s][0][1], 1); put(ends[s][1][1], 1)
    for i in range(16):
        put(indices[i], 1 if i in (0, ANCHOR2[part]) else 2)
    assert n == 128
    return bits.to_bytes(16, "little"), err


def encode(rgba):  # PIL RGBA image, size multiple of 4 -> BC7 bytes (row-major blocks)
    a = np.asarray(rgba).astype(float)
    h, w = a.shape[:2]
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            px = a[by:by + 4, bx:bx + 4].reshape(16, 4)
            block, err = _encode_block(px)
            if err > 16 * 4 * 4:                 # more than ~4 per channel off: try two subsets
                block7, err7 = _encode_block7(px)
                if err7 < err:
                    block = block7
            out += block
    return bytes(out)
