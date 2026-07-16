"""Rebuild the AKI Corporation mark from the archived 101px GIF and emit app icons."""
from PIL import Image, ImageDraw, ImageFilter
import os

SRC = os.path.join(os.path.dirname(__file__), "akicorporation.gif")
OUT = os.path.dirname(__file__)

BLUE = (34, 144, 207)
YELLOW = (246, 219, 0)
RED = (231, 87, 59)
WHITE = (255, 255, 255)
# draw order: blue at the back, yellow bar over it, red A in front
LAYERS = [BLUE, YELLOW, RED]

im = Image.open(SRC).convert("RGB")
mark = im.crop((0, 0, 101, 62))

# trim to the mark's bounding box (non-near-white pixels)
px = mark.load()
xs, ys = [], []
for y in range(mark.height):
    for x in range(mark.width):
        r, g, b = px[x, y]
        if not (r > 235 and g > 235 and b > 235):
            xs.append(x); ys.append(y)
mark = mark.crop((min(xs), min(ys), max(xs) + 1, max(ys) + 1))
w, h = mark.size
print("mark bbox:", mark.size)

def classify(p):
    best, bd = None, 1e9
    for c in [WHITE] + LAYERS:
        d = sum((a - b) ** 2 for a, b in zip(p, c))
        if d < bd:
            bd, best = d, c
    return best

px = mark.load()
masks = {c: Image.new("F", (w, h), 0.0) for c in LAYERS}
mpx = {c: masks[c].load() for c in LAYERS}
for y in range(h):
    for x in range(w):
        c = classify(px[x, y])
        if c != WHITE:
            mpx[c][x, y] = 1.0

SCALE = 12  # 101px -> ~1200px working res
W, H = w * SCALE, h * SCALE
mark_big = Image.new("RGBA", (W, H), (0, 0, 0, 0))
for c in LAYERS:
    big = masks[c].resize((W, H), Image.LANCZOS)
    # threshold to a hard-edged but smoothed alpha
    a = big.point(lambda v: v * 255).convert("L")
    a = a.point(lambda v: 255 if v >= 128 else 0)
    a = a.filter(ImageFilter.GaussianBlur(SCALE * 0.38))
    a = a.point(lambda v: max(0, min(255, (v - 124) * 14)))
    layer = Image.new("RGBA", (W, H), c + (255,))
    mark_big.paste(layer, (0, 0), a)
mark_big.save(os.path.join(OUT, "mark_big.png"))

def make_icon(size, bg="dark"):
    S = size
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = S * 0.22
    if bg == "dark":
        fill = (18, 18, 26, 255)
    else:
        fill = (248, 248, 245, 255)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=fill)
    # fit mark into ~68% of the tile
    target = S * 0.68
    mw, mh = mark_big.size
    k = min(target / mw, target / mh)
    m = mark_big.resize((max(1, int(mw * k)), max(1, int(mh * k))), Image.LANCZOS)
    img.alpha_composite(m, ((S - m.width) // 2, (S - m.height) // 2))
    return img

master = make_icon(1024, "dark")
master.save(os.path.join(OUT, "icon_dark_1024.png"))
make_icon(1024, "light").save(os.path.join(OUT, "icon_light_1024.png"))

sizes = [256, 128, 64, 48, 32, 24, 16]
frames = [master.resize((s, s), Image.LANCZOS) for s in sizes]
frames[0].save(os.path.join(OUT, "app.ico"), format="ICO",
               append_images=frames[1:],
               sizes=[(s, s) for s in sizes])
master.resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, "app.png"))
print("wrote app.ico + previews")
