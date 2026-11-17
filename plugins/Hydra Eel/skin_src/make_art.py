"""Hydra Eel artwork from the Sure Eel logo (skin_src/reference/): black -> rust brown, white ->
baby blue. Writes assets/hydra_logo.png (the recoloured logo) and assets/bg_hydra.png (the
1280x628 page background: the whole logo, its lines in a deeper blue so it reads as a backdrop).
Needs Pillow. Run once from the SDK root:  python "plugins/Hydra Eel/skin_src/make_art.py"
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RUST, BLUE, DEEP = (0x6E, 0x32, 0x14), (0x89, 0xCF, 0xF0), (0x4A, 0x80, 0x98)


def tint(gray, lo, hi):
    lut = []
    for c in range(3):
        lut += [int(lo[c] + (hi[c] - lo[c]) * v / 255.0 + 0.5) for v in range(256)]
    return Image.merge("RGB", (gray, gray, gray)).point(lut)


im = Image.open(os.path.join(HERE, "reference", "Sure eel logo.jpg")).convert("L")
w, h = im.size
px = im.load()
x0 = next(x for x in range(w) if px[x, h // 2] < 60)
y0 = next(y for y in range(h) if px[w // 2, y] < 60)
x1 = next(x for x in range(w - 1, 0, -1) if px[x, h // 2] < 60)
y1 = next(y for y in range(h - 1, 0, -1) if px[w // 2, y] < 60)
sq = im.crop((x0 + 2, y0 + 2, x1 - 1, y1 - 1))
tint(sq.resize((1024, 1024), Image.LANCZOS), RUST, BLUE).save(os.path.join(HERE, "assets", "hydra_logo.png"), optimize=True)
bg = Image.new("L", (1280, 628), 0)
bg.paste(sq.resize((628, 628), Image.LANCZOS), ((1280 - 628) // 2, 0))
tint(bg, RUST, DEEP).save(os.path.join(HERE, "assets", "bg_hydra.png"), optimize=True)
print("make_art: wrote assets/hydra_logo.png and assets/bg_hydra.png")
