"""PNG -> FH2 HUD radio icon (.swatchbin).
Usage: python make_swatch.py logo.png HUD_RadioIcon_Spotify.swatchbin

Input: a normal (straight-alpha) RGBA PNG at 196x96, or larger with the same 49:24 proportions
(e.g. the 784x384 master). The game's icons store premultiplied alpha, so colour is
premultiplied here (also before any downscale, which avoids dark fringes). The result is read
back exactly like the game's own icons and checked against the input before it is written."""
import os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bc7 import encode
from swatch import HEADER_SIZE, SURFACE, untile, tile

LOGO = (196, 96)
# Header of the game's own HUD_RadioIcon_Pulse.swatchbin (Grub/TXCB/TXCH, BC7, 196x96 in 256x128)
HEADER = bytes.fromhex(
    "62757247010001005300000053800000424358540200010028000000530000000080000000800000"
    "484358543002080000000000000000000000000000000001c40060000100c400600001000101002e0100b8")
assert len(HEADER) == HEADER_SIZE


def premultiplied(image):
    a = np.asarray(image.convert("RGBA")).astype(np.float64)
    a[..., :3] *= a[..., 3:4] / 255.0
    return a


def build(src_path, out_path):
    image = Image.open(src_path).convert("RGBA")
    w, h = image.size
    if abs(w * LOGO[1] - h * LOGO[0]) > max(w, h) // 50:
        sys.exit(f"{src_path}: {w}x{h} is not 196x96 or the same proportions")
    pm = premultiplied(image)
    if (w, h) != LOGO:  # downscale each premultiplied channel
        pm = np.stack([np.asarray(Image.fromarray(pm[..., c].astype(np.float32), "F").resize(LOGO, Image.LANCZOS))
                       for c in range(4)], axis=-1)
        pm[..., 3] = np.clip(pm[..., 3], 0, 255)
        pm[..., :3] = np.minimum(np.clip(pm[..., :3], 0, 255), pm[..., 3:4])
    surface = np.zeros((SURFACE[1], SURFACE[0], 4))
    surface[..., 3] = 255                               # stock files: opaque black outside the logo
    surface[:LOGO[1], :LOGO[0]] = pm
    surface = np.round(surface).astype(np.uint8)
    data = HEADER + tile(encode(Image.fromarray(surface, "RGBA")))

    back = np.asarray(Image.frombytes("RGBA", SURFACE, untile(data[HEADER_SIZE:]), "bcn", (7,))).astype(int)
    src = surface.astype(int)
    over = lambda x: x[..., :3] + 120 * (1 - x[..., 3:] / 255)    # premultiplied "over" mid grey
    err = np.abs(over(src) - over(back))[:LOGO[1], :LOGO[0]].max()
    brighter = (back[..., :3].max(axis=-1) > back[..., 3] + 6)[:LOGO[1], :LOGO[0]].mean()
    if err > 64 or brighter > 0.001:
        sys.exit(f"encoding check failed (visible error {err}, too-bright pixels {brighter:.2%}); nothing written")
    open(out_path, "wb").write(data)
    print(f"{out_path}: {len(data)} bytes, visible max error {int(err)}/255, premultiplied ok")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
