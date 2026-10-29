"""Draw the Line-in Radio station logo in FH2's HUD style (as spotify-radio/hud_logo/make_logo.py):
196x96 RGBA, transparent background, an RCA ("tulip") plug icon + heavy italic wordmark + spaced
RADIO tag, soft dark halo. Usage: make_logo.py [variant a|b|c|d|all] [output folder].
The mod ships variant A: spotify-radio/hud_logo/make_swatch.py line_in_logo_a_master_784x384.png
HUD_RadioIcon_LineIn.swatchbin"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4                                   # supersampling
W, H = 196, 96
FONT = r"C:\Windows\Fonts\seguibli.ttf"  # Segoe UI Black Italic
RED, RED_DARK = (226, 38, 44), (170, 22, 30)
WHITE, WHITE_DARK = (244, 244, 244), (196, 196, 200)
METAL, METAL_DARK, INK = (210, 214, 220), (130, 136, 146), (24, 24, 26)


def rca_plug(size, body, body_dark, angle=0, cable=True):
    """An RCA plug pointing up: coloured grip, metal collar, centre pin, cable out the bottom."""
    w, h = size * 0.42, size
    im = Image.new("RGBA", (int(size * 1.2), int(size * 1.2)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, top = im.width / 2, im.height * 0.05
    pin_w, pin_h = w * 0.16, h * 0.22
    d.rounded_rectangle((cx - pin_w / 2, top, cx + pin_w / 2, top + pin_h), radius=pin_w / 2, fill=METAL)
    collar_top = top + pin_h * 0.8
    collar_h = h * 0.2
    d.rectangle((cx - w * 0.38, collar_top, cx + w * 0.38, collar_top + collar_h), fill=METAL)
    for i in range(1, 4):                # the collar's slits
        x = cx - w * 0.38 + i * w * 0.19
        d.line((x, collar_top, x, collar_top + collar_h), fill=METAL_DARK, width=max(1, int(size * 0.03)))
    grip_top = collar_top + collar_h
    grip_h = h * 0.38
    d.rounded_rectangle((cx - w / 2, grip_top, cx + w / 2, grip_top + grip_h), radius=w * 0.18, fill=body)
    d.rectangle((cx, grip_top + w * 0.1, cx + w / 2 - w * 0.05, grip_top + grip_h - w * 0.1), fill=body_dark)
    for i in range(3):                   # grip ridges
        y = grip_top + grip_h * (0.25 + i * 0.22)
        d.line((cx - w * 0.42, y, cx + w * 0.42, y), fill=body_dark, width=max(1, int(size * 0.035)))
    if cable:
        boot_top = grip_top + grip_h
        d.polygon([(cx - w * 0.32, boot_top), (cx + w * 0.32, boot_top), (cx + w * 0.14, boot_top + h * 0.12),
                   (cx - w * 0.14, boot_top + h * 0.12)], fill=INK)
    return im.rotate(angle, resample=Image.BICUBIC, expand=False) if angle else im


def two_tone_text(text, font, light, dark):
    box = font.getbbox(text)
    tw, th = box[2] - box[0] + 8 * S, box[3] - box[1] + 8 * S
    mask = Image.new("L", (tw, th), 0)
    ImageDraw.Draw(mask).text((4 * S - box[0], 4 * S - box[1]), text, font=font, fill=255)
    fill = Image.new("RGBA", (tw, th), light + (255,))
    fill.paste(Image.new("RGBA", (tw, th // 2 + 1), dark + (255,)), (0, th // 2))
    out = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    out.paste(fill, (0, 0), mask)
    return out


def fit(image, max_w):
    scale = min(1.0, max_w / image.width)
    return image.resize((int(image.width * scale), int(image.height * scale)), Image.LANCZOS)


def tag(canvas, text, right, y, colour=(240, 240, 240, 255), size=14):
    font = ImageFont.truetype(FONT, size * S)
    d = ImageDraw.Draw(canvas)
    gap = 6 * S
    total = sum(int(font.getlength(ch)) for ch in text) + gap * (len(text) - 1)
    x = right - total
    for ch in text:
        d.text((x, y), ch, font=font, fill=colour)
        x += int(font.getlength(ch)) + gap


def halo(canvas):
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    shadow.putalpha(canvas.getchannel("A").filter(ImageFilter.GaussianBlur(2 * S)).point(lambda v: int(v * 0.45)))
    final = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    final.alpha_composite(shadow, (0, S))
    final.alpha_composite(canvas)
    return final


def variant_a():
    """Two crossed plugs, red and white, + red/white "line-in" wordmark."""
    c = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    c.alpha_composite(rca_plug(54 * S, WHITE, WHITE_DARK, angle=18), (-2 * S, 14 * S))
    c.alpha_composite(rca_plug(54 * S, RED, RED_DARK, angle=-18), (12 * S, 14 * S))
    word = fit(two_tone_text("line-in", ImageFont.truetype(FONT, 50 * S), WHITE, WHITE_DARK), (W - 74) * S)
    c.alpha_composite(word, (70 * S, 18 * S))
    tag(c, "RADIO", 70 * S + word.width - 4 * S, 18 * S + word.height - 2 * S, RED + (255,))
    return halo(c)


def variant_b():
    """One red plug pointing into the wordmark, a sound wave under it."""
    c = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    plug = rca_plug(56 * S, RED, RED_DARK, angle=-90, cable=False)   # pointing right, into the text
    c.alpha_composite(plug, (-6 * S, 6 * S))
    d = ImageDraw.Draw(c)
    word = fit(two_tone_text("LINE IN", ImageFont.truetype(FONT, 46 * S), RED, RED_DARK), (W - 66) * S)
    c.alpha_composite(word, (62 * S, 16 * S))
    wave_y = 16 * S + word.height + 4 * S
    wave = [(x, wave_y + 4 * S * math.sin((x - 62 * S) / (4 * S))) for x in range(62 * S, W * S - 6 * S, S)]
    d.line(wave, fill=WHITE + (255,), width=3 * S)
    return halo(c)


def variant_c():
    """A round jack panel: red and white sockets side by side, like the back of an amp."""
    c = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    d.rounded_rectangle((4 * S, 24 * S, 62 * S, 72 * S), radius=10 * S, fill=INK + (255,))
    for cx, colour in ((19 * S, WHITE), (47 * S, RED)):
        d.ellipse((cx - 11 * S, 37 * S, cx + 11 * S, 59 * S), fill=colour + (255,))
        d.ellipse((cx - 7 * S, 41 * S, cx + 7 * S, 55 * S), fill=METAL + (255,))
        d.ellipse((cx - 3 * S, 45 * S, cx + 3 * S, 51 * S), fill=INK + (255,))
    small = ImageFont.truetype(FONT, 7 * S)
    d.text((15 * S, 26 * S), "L", font=small, fill=WHITE + (255,))
    d.text((44 * S, 26 * S), "R", font=small, fill=RED + (255,))
    word = fit(two_tone_text("line in", ImageFont.truetype(FONT, 50 * S), WHITE, WHITE_DARK), (W - 72) * S)
    c.alpha_composite(word, (68 * S, 18 * S))
    tag(c, "RADIO", 68 * S + word.width - 4 * S, 18 * S + word.height - 2 * S, RED + (255,))
    return halo(c)


def variant_d():
    """Wordmark only: the plugs stand in for the I's of "LINE IN"."""
    c = Image.new("RGBA", (W * 2 * S, H * S), (0, 0, 0, 0))  # drawn wide, then fitted into the box
    font = ImageFont.truetype(FONT, 52 * S)
    x, y = 4 * S, 10 * S
    for part, kind in (("L", "t"), ("I", "w"), ("NE", "t"), (" ", "s"), ("I", "r"), ("N", "t")):
        if kind in "wr":
            body, dark = (WHITE, WHITE_DARK) if kind == "w" else (RED, RED_DARK)
            plug = rca_plug(50 * S, body, dark, angle=-12)
            c.alpha_composite(plug, (x - 18 * S, y + 2 * S))
            x += 18 * S
        elif kind == "s":
            x += 10 * S
        else:
            word = two_tone_text(part, font, WHITE, WHITE_DARK)
            c.alpha_composite(word, (x, y))
            x += word.width - 9 * S
    tag(c, "RADIO", x + 4 * S, 66 * S, RED + (255,))
    c = fit(c.crop(c.getbbox()), (W - 4) * S)
    full = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    full.alpha_composite(c, ((W * S - c.width) // 2, (H * S - c.height) // 2))
    c = full
    return halo(c)


VARIANTS = {"a": variant_a, "b": variant_b, "c": variant_c, "d": variant_d}


def preview(images, out):
    """Each variant at 2x on the game's dark HUD panel and on a bright sky, side by side."""
    tile_w, tile_h = W * 2, H * 2
    sheet = Image.new("RGB", (tile_w * 2 + 30, (tile_h + 34) * len(images) + 10), (16, 16, 18))
    d = ImageDraw.Draw(sheet)
    label = ImageFont.truetype(FONT, 18)
    for row, (name, im) in enumerate(images):
        y = 10 + row * (tile_h + 34)
        d.text((10, y), name.upper(), font=label, fill=(230, 230, 230))
        for col, bg in enumerate(((34, 38, 46), (150, 178, 205))):
            tile = Image.new("RGBA", (W, H), bg + (255,))
            tile.alpha_composite(im)
            sheet.paste(tile.resize((tile_w, tile_h), Image.LANCZOS).convert("RGB"), (10 + col * (tile_w + 10), y + 24))
    sheet.save(os.path.join(out, "line_in_logo_variants.png"))


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
    images = []
    for name, make in VARIANTS.items():
        if which in ("all", name):
            master = make()
            master.save(os.path.join(out, f"line_in_logo_{name}_master_784x384.png"))  # make_swatch.py input
            master.resize((W, H), Image.LANCZOS).save(os.path.join(out, f"line_in_logo_{name}.png"))
            images.append((name, master.resize((W, H), Image.LANCZOS)))
    preview(images, out)
