"""FH2 HUD radio icon .swatchbin (Grub/TXCB/TXCH): 0x53-byte header, then a 256x128 BC7 surface
(64x32 blocks, 16 bytes each) in the Xbox One tiled order. Logo area: 196x96 at the top left.

Tiled block index i -> block (x, y), worked out by decoding the game's own icons:
  i bits 0-3  -> x0 y0 x1 y1                 (one 256-byte piece = 4x4 blocks)
  i bits 4-10 -> piece bits p0..p6, and
     x2 = p5   x3 = p0^p1^p3^p4   x4 = p1^p3   x5 = p2   y2 = p6   y3 = p4   y4 = p3"""
from PIL import Image

HEADER_SIZE = 0x53
SURFACE = (256, 128)
BW, BH = 64, 32


def block_xy(i):
    x = (i & 1) | ((i >> 1) & 2)
    y = ((i >> 1) & 1) | ((i >> 2) & 2)
    p = [(i >> (4 + b)) & 1 for b in range(7)]
    x |= (p[5] << 2) | ((p[0] ^ p[1] ^ p[3] ^ p[4]) << 3) | ((p[1] ^ p[3]) << 4) | (p[2] << 5)
    y |= (p[6] << 2) | (p[4] << 3) | (p[3] << 4)
    return x, y


ORDER = [block_xy(i) for i in range(BW * BH)]
assert len(set(ORDER)) == BW * BH, "layout is not a bijection"


def untile(tiled):
    linear = bytearray(len(tiled))
    for i, (x, y) in enumerate(ORDER):
        o = (y * BW + x) * 16
        linear[o:o + 16] = tiled[i * 16:i * 16 + 16]
    return bytes(linear)


def tile(linear):
    tiled = bytearray(len(linear))
    for i, (x, y) in enumerate(ORDER):
        o = (y * BW + x) * 16
        tiled[i * 16:i * 16 + 16] = linear[o:o + 16]
    return bytes(tiled)


def read(path):
    data = open(path, "rb").read()
    assert len(data) == HEADER_SIZE + 0x8000 and data[:4] == b"burG", "unexpected swatchbin"
    return data[:HEADER_SIZE], data[HEADER_SIZE:]


def decode(path):
    _, tiled = read(path)
    return Image.frombytes("RGBA", SURFACE, untile(tiled), "bcn", (7,))
