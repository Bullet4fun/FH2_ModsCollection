"""Draw the Spotify radio station logo in FH2's HUD style: 196x96 RGBA, transparent background,
Spotify icon + two-tone heavy italic wordmark + spaced RADIO tag, soft dark drop shadow."""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4                                  # supersampling
W, H = 196, 96
GREEN_LIGHT, GREEN_DARK = (30, 215, 96), (22, 170, 76)
FONT = r"C:\Windows\Fonts\seguibli.ttf"  # Segoe UI Black Italic


def spotify_icon(size):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((0, 0, size - 1, size - 1), fill=GREEN_LIGHT + (255,))
    cx = size / 2
    # three arcs, top longest and thickest, as on the Spotify icon
    for k, (top, half_w, thick) in enumerate(((0.27, 0.33, 0.095), (0.46, 0.28, 0.08), (0.63, 0.22, 0.068))):
        r = size * 0.55
        box = (cx - r, size * top, cx + r, size * top + 2 * r)
        span = 180 * half_w * size / (3.14159 * r) * 2
        start, end = 270 - span / 2, 270 + span / 2
        w = max(1, int(size * thick))
        d.arc(box, start, end, fill=(18, 18, 18, 255), width=w)
        import math
        for ang in (start, end):  # round caps
            a = math.radians(ang)
            px = cx + (r - w / 2) * math.cos(a)
            py = size * top + r + (r - w / 2) * math.sin(a)
            d.ellipse((px - w / 2, py - w / 2, px + w / 2, py + w / 2), fill=(18, 18, 18, 255))
    return im


def two_tone_text(text, font, light, dark):
    box = font.getbbox(text)
    tw, th = box[2] - box[0] + 8 * S, box[3] - box[1] + 8 * S
    mask = Image.new("L", (tw, th), 0)
    ImageDraw.Draw(mask).text((4 * S - box[0], 4 * S - box[1]), text, font=font, fill=255)
    fill = Image.new("RGBA", (tw, th), light + (255,))
    lower = Image.new("RGBA", (tw, th // 2 + 1), dark + (255,))
    fill.paste(lower, (0, th // 2))      # lighter top half, darker bottom half, like "pulse"
    out = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    out.paste(fill, (0, 0), mask)
    return out


def logo():
    canvas = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    icon = spotify_icon(56 * S)
    canvas.alpha_composite(icon, (6 * S, 22 * S))
    word = two_tone_text("spotify", ImageFont.truetype(FONT, 50 * S), GREEN_LIGHT, GREEN_DARK)
    scale = min(1.0, (W - 68) * S / word.width)
    word = word.resize((int(word.width * scale), int(word.height * scale)), Image.LANCZOS)
    word_x, word_y = 64 * S, 18 * S
    canvas.alpha_composite(word, (word_x, word_y))
    tag_font = ImageFont.truetype(FONT, 14 * S)
    d = ImageDraw.Draw(canvas)
    gap = 6 * S
    letters = "RADIO"                    # spaced capitals under the wordmark's right end, like "ARENA"
    total = sum(int(tag_font.getlength(ch)) for ch in letters) + gap * (len(letters) - 1)
    x = word_x + word.width - 4 * S - total
    for ch in letters:
        d.text((x, word_y + word.height - 2 * S), ch, font=tag_font, fill=(240, 240, 240, 255))
        x += int(tag_font.getlength(ch)) + gap
    # faint dark halo under everything
    alpha = canvas.getchannel("A")
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    shadow.putalpha(alpha.filter(ImageFilter.GaussianBlur(2 * S)).point(lambda v: int(v * 0.45)))
    final = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    final.alpha_composite(shadow, (0, S))  # thin halo like the stock icons, not an offset drop shadow
    final.alpha_composite(canvas)
    return final  # W*S x H*S master


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    master = logo()
    master.save(os.path.join(out, "spotify_logo_master_784x384.png"))
    im = master.resize((W, H), Image.LANCZOS)
    im.save(os.path.join(out, "spotify_logo.png"))
    preview = Image.new("RGBA", (W, H), (120, 120, 120, 255))
    preview.alpha_composite(im)
    preview.resize((W * 3, H * 3), Image.NEAREST).convert("RGB").save(os.path.join(out, "spotify_preview.png"))
