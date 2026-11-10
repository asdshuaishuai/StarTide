#!/usr/bin/env python3
"""Generate the StarTide app icon.

A four-point star over two tide lines, on a night-to-teal gradient,
inside the macOS rounded-square mask.
"""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# --- background: vertical gradient from deep night to tide teal ----------
top = (14, 42, 58)       # #0e2a3a
bottom = (16, 125, 120)  # #107d78
grad = Image.new("RGB", (1, S))
gp = grad.load()
for y in range(S):
    t = y / (S - 1)
    gp[0, y] = (
        int(top[0] + (bottom[0] - top[0]) * t),
        int(top[1] + (bottom[1] - top[1]) * t),
        int(top[2] + (bottom[2] - top[2]) * t),
    )
bg = grad.resize((S, S)).convert("RGBA")

# --- rounded square mask -------------------------------------------------
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=228, fill=255)
img.paste(bg, (0, 0), mask)

d = ImageDraw.Draw(img)

# --- four point star -----------------------------------------------------
cx, cy = S * 0.5, S * 0.375
outer, inner = S * 0.21, S * 0.047
pts = []
for i in range(8):
    ang = math.pi / 2 * (i / 2) - math.pi / 2
    r = outer if i % 2 == 0 else inner
    pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))

star = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sd = ImageDraw.Draw(star)
sd.polygon(pts, fill=(247, 241, 227, 255))

# clean radial glow: blur the star's alpha and use it as a mask
from PIL import ImageChops
glow_alpha = star.filter(ImageFilter.GaussianBlur(46)).split()[3]
glow_alpha = glow_alpha.point(lambda v: (v * 45) // 100)
glow = Image.new("RGBA", (S, S), (247, 241, 227, 0))
glow.putalpha(glow_alpha)
img.alpha_composite(glow)
img.alpha_composite(star)

# --- tide lines ----------------------------------------------------------
def wave(y0, amp, period, phase, width, color):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    r = width / 2
    for x in range(-40, S + 40, 3):
        y = y0 + amp * math.sin(2 * math.pi * (x / period) + phase)
        ld.ellipse([x - r, y - r, x + r, y + r], fill=color)
    return layer

img.alpha_composite(wave(S * 0.665, 26, 340, 0.4, 22, (247, 241, 227, 235)))
img.alpha_composite(wave(S * 0.745, 22, 300, 2.1, 16, (247, 241, 227, 130)))

# --- subtle inner glow on the mask edge ---------------------------------
edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ed = ImageDraw.Draw(edge)
ed.rounded_rectangle([10, 10, S - 11, S - 11], radius=218, outline=(255, 255, 255, 26), width=3)
edge = edge.filter(ImageFilter.GaussianBlur(6))
img.alpha_composite(edge)

# re-apply mask so every layer stays inside the rounded square
final = Image.new("RGBA", (S, S), (0, 0, 0, 0))
final.paste(img, (0, 0), mask)

out = "/Volumes/data/dev/coding/macTools/skillctl/gui/resources/AppIcon.png"
final.save(out, "PNG")

# --- iconset -------------------------------------------------------------
import os
iconset = "/Volumes/data/dev/coding/macTools/skillctl/gui/resources/AppIcon.iconset"
os.makedirs(iconset, exist_ok=True)
for size in (16, 32, 64, 128, 256, 512, 1024):
    img2 = final.resize((size, size), Image.LANCZOS)
    img2.save(f"{iconset}/icon_{size}x{size}.png")
    if size <= 512:
        img2.resize((size * 2, size * 2), Image.LANCZOS).save(
            f"{iconset}/icon_{size}x{size}@2x.png"
        )
print("icon written:", out)
