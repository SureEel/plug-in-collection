"""Builds Elver's touch skin (plugins/Elver/skins/) and its ui.json.

Look: a brushed-metal panel with inset plates, in four colour skins chosen by the Skin parameter
(palettes only, no poster imagery):
  Neon Rain   - dark teal-blue steel, neon orange frames, a magenta hint   (Blade Runner palette)
  Capsule Red - deep red-black metal, off-white frames, signal red          (Akira palette)
  Monolith    - cool grey brushed steel, deep-space blue frames             (2001 palette)
  Derelict    - near-black green metal, pale sickly green frames            (Alien palette, dark)
All pages share ONE background film strip (4 frames, one per skin, bound to Skin), so a skin change
recolours every page and the plugin keeps a single background image in memory. Controls and type are
neutral silver / gunmetal so they read on every skin. The Sure Eel logo is embossed faintly.

Layout: every page is four sections side by side (one Q-Link bank each, top to bottom) and a bottom
strip (CPU meter, voices, help). Rules learned on the MPC hardware (Sleepy Android, Hydra Eel):
  * nothing lies on top of a button: selector / key / action text is drawn INTO the button images;
  * action buttons and letter keys are plain on/off switches; the plugin acts on every change;
  * static text is bound to a real parameter; no conditional visibility;
  * help: two value labels (Help / Help 2) set by invisible buttons over names and meters.

usage (SDK root, after one native build so build/params.json exists; needs Pillow):
    python plugins/Elver/skin_src/make_skin.py
    python tools/build.py plugins/Elver
"""
import json
import os
import shutil
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
LOGO = os.path.join(HERE, "reference", "Sure eel logo.jpg")      # the Sure Eel logo: not MIT-licensed (see LICENSE)
FONT_FILE = r"C:\Windows\Fonts\bahnschrift.ttf"
W, H = 1280, 628

# skins: (name, metal, plate, accent, second accent)
SKINS = [
    ("Neon Rain",   (0x1A, 0x2A, 0x33), (0x0A, 0x12, 0x17), (0xFF, 0x7A, 0x2A), (0xE0, 0x3C, 0x9A)),
    ("Capsule Red", (0x40, 0x14, 0x14), (0x1A, 0x07, 0x08), (0xF0, 0xEA, 0xE0), (0xD8, 0x26, 0x2C)),
    ("Monolith",    (0x5C, 0x62, 0x68), (0x1C, 0x20, 0x25), (0x5A, 0x9C, 0xFF), (0xC8, 0xCE, 0xD4)),
    ("Derelict",    (0x16, 0x1C, 0x18), (0x06, 0x0A, 0x07), (0xB8, 0xE0, 0xA0), (0x50, 0x68, 0x48)),
]
SILVER, GUN, GUN_LINE, INK = (0xDA, 0xDE, 0xE2), (0x2A, 0x2D, 0x31), (0x9A, 0xA3, 0xAD), (0x16, 0x18, 0x1B)
TEXT, MUTED = "ffe6e9ec", "ff9aa3ad"
STATIC_HANDLE = "Parameter 0"
HEAD_FONT = "Nexa"
SEP = "\u00b7"
MORE = "^"                              # this row is taken by the control above (a tall selector)

PAGES = [
    ("MAIN", [
        ("xy pad " + SEP + " x blend " + SEP + " y pad", "PAD"),
        ("filters", ["A Cutoff", "A Reso", "B Cutoff", "B Reso"]),
        ("randomize", ["Category", MORE, MORE, "Randomize"]),
        ("patch", ["Undo", "Rand Amount", ("status", "Status", "status"), "Volume"]),
    ]),
    ("OSC A", [
        ("osc a1", ["A1 Wave", "A1 Level", "A1 Octave", "A1 Semi"]),
        ("osc a2", ["A2 Wave", "A2 Level", "A2 Octave", "A2 Semi"]),
        ("osc a3", ["A3 Wave", "A3 Level", "A3 Octave", "A3 Semi"]),
        ("unison a", ["A Unison", "A Detune", "A Spread", None]),
    ]),
    ("SHAPE A", [
        ("start " + SEP + " a", ["A Start From", "A Draw Mix", "A Mutate", "A Seed"]),
        ("spectrum " + SEP + " a", ["A Bright", "A Odd/Even", "A Formant", None]),
        ("shape " + SEP + " a", ["A Fold", "A Bend", "A Sync", None]),
        ("wave " + SEP + " a", "SCOPE:A"),
    ]),
    ("DRAW A", [
        ("draw " + SEP + " a " + SEP + " drag on the pad to paint the wave", "DRAWPAD:A"),
        ("", "SPAN"), ("", "SPAN"),
        ("draw tools " + SEP + " a", ["A Smooth", "A Copy Start", "A Clear Draw", "A Undo Draw"]),
    ]),
    ("OSC B", [
        ("osc b1", ["B1 Wave", "B1 Level", "B1 Octave", "B1 Semi"]),
        ("osc b2", ["B2 Wave", "B2 Level", "B2 Octave", "B2 Semi"]),
        ("osc b3", ["B3 Wave", "B3 Level", "B3 Octave", "B3 Semi"]),
        ("unison b", ["B Unison", "B Detune", "B Spread", None]),
    ]),
    ("SHAPE B", [
        ("start " + SEP + " b", ["B Start From", "B Draw Mix", "B Mutate", "B Seed"]),
        ("spectrum " + SEP + " b", ["B Bright", "B Odd/Even", "B Formant", None]),
        ("shape " + SEP + " b", ["B Fold", "B Bend", "B Sync", None]),
        ("wave " + SEP + " b", "SCOPE:B"),
    ]),
    ("DRAW B", [
        ("draw " + SEP + " b " + SEP + " drag on the pad to paint the wave", "DRAWPAD:B"),
        ("", "SPAN"), ("", "SPAN"),
        ("draw tools " + SEP + " b", ["B Smooth", "B Copy Start", "B Clear Draw", "B Undo Draw"]),
    ]),
    ("TUNE", [
        ("fine a", ["A1 Fine", "A2 Fine", "A3 Fine", None]),
        ("pulse width a", ["A1 PW", "A2 PW", "A3 PW", None]),
        ("fine b", ["B1 Fine", "B2 Fine", "B3 Fine", None]),
        ("pulse width b", ["B1 PW", "B2 PW", "B3 PW", None]),
    ]),
    ("FILTER", [
        ("filter a", ["A Filter", "A Cutoff", "A Reso", "A Env Amt"]),
        ("filter b", ["B Filter", "B Cutoff", "B Reso", "B Env Amt"]),
        ("tracking " + SEP + " routing", ["A Key Trk", "B Key Trk", "Routing", "Pad Y To"]),
        ("blend " + SEP + " pad", ["Blend", "Pad Y", None, None]),
    ]),
    ("ENV", [
        ("amp env a", ["A Attack", "A Decay", "A Sustain", "A Release"]),
        ("filter env a", ["A F.Attack", "A F.Decay", "A F.Sustain", "A F.Release"]),
        ("amp env b", ["B Attack", "B Decay", "B Sustain", "B Release"]),
        ("filter env b", ["B F.Attack", "B F.Decay", "B F.Sustain", "B F.Release"]),
    ]),
    ("MOD", [
        ("lfo 1", ["LFO1 Rate", "LFO1 Depth", "LFO1 Shape", "LFO1 To"]),
        ("lfo 2", ["LFO2 Rate", "LFO2 Depth", "LFO2 Shape", "LFO2 To"]),
        ("character", ["Drift", "Drive", "Grit", "Ring"]),
        ("global", ["Volume", "Output", "Voices", "Skin"]),
    ]),
    ("PRESET", [
        ("browse " + SEP + " category", ["Browse", MORE, MORE, "Load"]),
        ("preset", ["Preset", ("status", "Status", "status"), None, None]),
        ("randomize", ["Rand Amount", "Undo", "Lock Mod", "Lock Skin"]),
        ("locks", ["Lock A", "Lock B", "Lock Filter", "Lock Env"]),
    ]),
    ("SAVE", [
        ("name " + SEP + " letters", "KEYS0"),
        ("name " + SEP + " keys", "KEYS1"),
        ("save", ["Save", "Confirm", "Cancel", ("status", "Name", "name " + SEP + " | = cursor")]),
        ("save in", ["Save In", MORE, MORE, ("status", "Status", "status")]),
    ]),
]
TOGGLES = {"Lock A", "Lock B", "Lock Filter", "Lock Env", "Lock Mod", "Lock Skin"}
WAVES = ["sine", "tri", "saw", "sqr", "noise", "shape"]
SELECTORS = {"Pad Y To": ["cutoff", "morph", "reso", "drive"], "Routing": ["parallel", "series"],
             "Skin": ["neon", "red", "monolith", "derelict"]}
for s in "AB":
    for o in "123":
        SELECTORS["%s%s Wave" % (s, o)] = WAVES
    SELECTORS["%s Filter" % s] = ["lp24", "lp12", "bp", "hp", "notch"]
    SELECTORS["%s Smooth" % s] = ["smooth", "linear", "steps"]
for l in "12":
    SELECTORS["LFO%s Shape" % l] = ["sine", "tri", "saw", "sqr", "s&h"]
    SELECTORS["LFO%s To" % l] = ["pitch", "filter", "amp", "blend"]
CATS = ["ambient", "pads " + SEP + " winds", "keys " + SEP + " plucks", "leads " + SEP + " bass", "lo-fi psych", "industrial", "noise " + SEP + " chaos"]
BIG = {"Category": CATS, "Browse": CATS, "Save In": CATS}
ACTIONS = {"Randomize": "randomize", "Undo": "undo", "Load": "load", "Save": "name a preset", "Confirm": "confirm save", "Cancel": "cancel"}
for s in "AB":
    ACTIONS.update({s + " Copy Start": "copy start wave", s + " Clear Draw": "clear drawing", s + " Undo Draw": "undo stroke"})
KEY_CAPS = [chr(ord("A") + i) for i in range(26)] + [str(i) for i in range(10)] + ["space", "-", "del", "<", ">", "clear"]
KEY_NAMES = ["Key " + c for c in KEY_CAPS[:36]] + ["Key Space", "Key -", "Backspace", "Left", "Right", "Clear"]

HELP_FOR = {"Blend": 2, "Pad Y": 1, "Pad Y To": 3, "Routing": 13, "Voices": 20, "Drift": 17, "Drive": 18, "Grit": 18,
            "Ring": 19, "Volume": 21, "Output": 21, "Skin": 22, "Category": 23, "Randomize": 24, "Undo": 24,
            "Rand Amount": 25, "Browse": 27, "Preset": 27, "Load": 27, "Save": 28, "Confirm": 28, "Cancel": 28,
            "Save In": 28, "Name": 28, "Status": 24, "PAD": 1}
for s in "AB":
    for o in "123":
        HELP_FOR.update({"%s%s Wave" % (s, o): 4, "%s%s Level" % (s, o): 5, "%s%s Octave" % (s, o): 6,
                         "%s%s Semi" % (s, o): 6, "%s%s Fine" % (s, o): 6, "%s%s PW" % (s, o): 7})
    HELP_FOR.update({s + " Unison": 8, s + " Detune": 8, s + " Spread": 8, s + " Filter": 9, s + " Cutoff": 10,
                     s + " Reso": 10, s + " Env Amt": 11, s + " Key Trk": 12})
    for st in ("Attack", "Decay", "Sustain", "Release"):
        HELP_FOR[s + " " + st] = 14
        HELP_FOR[s + " F." + st] = 15
for l in "12":
    for k in ("Rate", "Depth", "Shape", "To"):
        HELP_FOR["LFO%s %s" % (l, k)] = 16
for t in TOGGLES:
    HELP_FOR[t] = 26
for s in "AB":
    HELP_FOR.update({s + " Start From": 31, s + " Bright": 32, s + " Odd/Even": 32, s + " Formant": 33, s + " Fold": 34,
                     s + " Bend": 34, s + " Sync": 34, s + " Mutate": 35, s + " Seed": 35, s + " Draw Mix": 36,
                     s + " Smooth": 37, s + " Copy Start": 37, s + " Clear Draw": 37, s + " Undo Draw": 37})
HELP_N = 39
PROGRAM_QLINKS = ["Blend", "Pad Y", "A Cutoff", "A Reso", "B Cutoff", "B Reso", "A Env Amt", "B Env Amt",
                  "Randomize", "Rand Amount", "Undo", "LFO1 Rate", "LFO1 Depth", "Drift", "Drive", "Volume"]

SEC_X0, SEC_PITCH, SEC_W, SEC_Y, SEC_H = 18, 312, 300, 6, 420
ROW_Y0, ROW_PITCH, CTL_W, CTL_H = 52, 92, 288, 88
STRIP_Y, STRIP_H, HELP_X = 436, 172, 654
LEDS, LED_PITCH, LED_W, LED_H = 24, 11, 8, 18
SEG_GAP, SEG_Y, SEG_H = 6, 36, 42
SEG_W = {2: 132, 3: 86, 4: 64, 5: 50, 6: 41}
# SHAPE page scope (section 3) and DRAW page field (sections 0..2)
SCOPE_X, SCOPE_Y, SCOPE_PITCH, SCOPE_W, SCOPE_H = 22, 64, 8, 6, 300
FIELD_W, FIELD_H = 3 * 312 - 12, 420
PTS_X, PTS_Y, PTS_PITCH, PTS_W, PTS_H = 14, 48, 14, 10, 360
BIG_W, BIG_H, BIG_GAP_X, BIG_GAP_Y, BIG_Y = 132, 50, 6, 6, 40
ACT_W, ACT_H = 200, 46
KEY_W, KEY_H, KEY_GX, KEY_GY, KEY_Y0 = 88, 46, 6, 6, 48
PAD = (SEC_X0 + 12, SEC_Y + 46, 276, 272)       # the XY pad (section 0, rows 0..2)
PUCK = 44


def slug(s):
    return "".join(ch if ch.isalnum() else "_" for ch in s.lower()).strip("_")


def font(size, variant="Regular"):
    f = ImageFont.truetype(FONT_FILE, size)
    try:
        f.set_variation_by_name(variant)
    except Exception:
        pass
    return f


# ------------------------------------------------------------------------------------------
# Artwork
# ------------------------------------------------------------------------------------------
def brushed(w, h, rgb, seed, strength=0.10):
    """horizontal brushed streaks: smooth along x, varied along y, plus a soft top-left light"""
    import random
    rnd = random.Random(seed)
    small = Image.new("L", (48, h))
    px = small.load()
    for y in range(h):
        row = rnd.gauss(0, 1) * 0.6
        for x in range(48):
            px[x, y] = max(0, min(255, int(128 + 60 * (row + rnd.gauss(0, 1) * 0.5))))
    streak = small.resize((w, h), Image.BILINEAR).filter(ImageFilter.GaussianBlur((6, 0.4)))
    light = Image.linear_gradient("L").rotate(45, expand=False).resize((w, h))      # 0 top-left .. 255
    # gain image: 128 = x1.0; streaks +-strength, light +9 % top-left .. -9 % bottom-right
    g1 = streak.point(lambda v: max(0, min(255, int(128 * (1.0 + strength * (v - 128) / 60.0)))))
    g2 = light.point(lambda v: max(0, min(255, int(128 + 128 * 0.18 * (0.5 - v / 255.0)))))
    gain = ImageChops.add(g1, g2, 1.0, -128)
    out = [gain.point([max(0, min(255, int(rgb[c] * g / 128.0 + 0.5))) for g in range(256)]) for c in range(3)]
    return Image.merge("RGB", out)


def plate(img, box, skin, accent_line=True, rad=12):
    _, metal, plate_rgb, acc, acc2 = skin
    x, y, w, h = box
    d = ImageDraw.Draw(img)
    inner = brushed(w, h, plate_rgb, seed=x * 7 + y, strength=0.06)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=rad, fill=255)
    img.paste(inner, (x, y), mask)
    # bevel: dark top-left (recessed), light bottom-right
    hi = tuple(min(255, int(c * 1.9 + 24)) for c in metal)
    lo = tuple(int(c * 0.35) for c in metal)
    d.arc((x, y, x + 2 * rad, y + 2 * rad), 180, 270, fill=lo, width=2)
    d.line((x + rad, y, x + w - rad, y), fill=lo, width=2)
    d.line((x, y + rad, x, y + h - rad), fill=lo, width=2)
    d.line((x + rad, y + h - 1, x + w - rad, y + h - 1), fill=hi, width=1)
    d.line((x + w - 1, y + rad, x + w - 1, y + h - rad), fill=hi, width=1)
    if accent_line:
        d.rounded_rectangle((x + 4, y + 4, x + w - 5, y + h - 5), radius=rad - 3, outline=acc, width=2)
    # rivets
    for rx, ry in ((x + 14, y + 14), (x + w - 15, y + 14), (x + 14, y + h - 15), (x + w - 15, y + h - 15)):
        d.ellipse((rx - 3, ry - 3, rx + 3, ry + 3), fill=lo)
        d.ellipse((rx - 2, ry - 3, rx + 2, ry + 1), fill=hi)
    return d


def background():
    """one film strip: 4 frames (skins) of 1280 x 628, stacked"""
    logo = None
    if os.path.isfile(LOGO):
        im = Image.open(LOGO).convert("L")
        w, h = im.size
        px = im.load()
        x0 = next(x for x in range(w) if px[x, h // 2] < 60)
        y0 = next(y for y in range(h) if px[w // 2, y] < 60)
        x1 = next(x for x in range(w - 1, 0, -1) if px[x, h // 2] < 60)
        y1 = next(y for y in range(h - 1, 0, -1) if px[w // 2, y] < 60)
        logo = im.crop((x0 + 2, y0 + 2, x1 - 1, y1 - 1)).resize((132, 132), Image.LANCZOS)
    strip = Image.new("RGB", (W, H * len(SKINS)))
    for k, skin in enumerate(SKINS):
        _, metal, _, acc, acc2 = skin
        img = brushed(W, H, metal, seed=11 + k)
        for s in range(4):
            x = SEC_X0 + s * SEC_PITCH
            d = plate(img, (x, SEC_Y, SEC_W, SEC_H), skin)
            d.line((x + 16, SEC_Y + 40, x + SEC_W - 16, SEC_Y + 40), fill=acc2, width=1)
        for x, w in ((18, 300), (336, 300), (HELP_X, 1262 - HELP_X)):
            plate(img, (x, STRIP_Y, w, STRIP_H), skin, accent_line=(x == HELP_X), rad=8)
        if logo is not None:
            # embossed: the logo's edges lift the metal by a few percent
            emb = logo.filter(ImageFilter.EMBOSS)
            lx, ly = 336 + 300 - 132 - 18, STRIP_Y + (STRIP_H - 132) // 2
            region = img.crop((lx, ly, lx + 132, ly + 132))
            orig = region.copy()
            rp, ep = region.load(), emb.load()
            for yy in range(132):
                for xx in range(132):
                    k2 = 1.0 + 0.9 * (ep[xx, yy] - 128) / 128.0
                    rp[xx, yy] = tuple(max(0, min(255, int(c * k2))) for c in rp[xx, yy])
            # a soft round mask: no square edge
            mask = Image.new("L", (132, 132), 0)
            ImageDraw.Draw(mask).ellipse((10, 10, 121, 121), fill=255)
            mask = mask.filter(ImageFilter.GaussianBlur(5))
            img.paste(Image.composite(region, orig, mask), (lx, ly))
        strip.paste(img, (0, k * H))
    return strip


def pad_field():
    """the XY pad's field: transparent with a fine grid, axis letters and a centre cross"""
    x, y, w, h = PAD
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=8, fill=(0, 0, 0, 90), outline=GUN_LINE + (255,), width=2)
    for i in range(1, 8):
        gx, gy = int(w * i / 8), int(h * i / 8)
        a = 110 if i == 4 else 45
        d.line((gx, 4, gx, h - 5), fill=GUN_LINE + (a,), width=1)
        d.line((4, gy, w - 5, gy), fill=GUN_LINE + (a,), width=1)
    f = font(20, "SemiBold")
    d.text((10, h // 2 - 12), "A", font=f, fill=SILVER + (200,))
    d.text((w - 24, h // 2 - 12), "B", font=f, fill=SILVER + (200,))
    fs = font(13, "Light")
    d.text((w // 2 + 6, 6), "y +", font=fs, fill=SILVER + (170,))
    d.text((w // 2 + 6, h - 22), "y -", font=fs, fill=SILVER + (170,))
    return im


def bar_strip(w, h, levels=33, colour=SILVER):
    """film strip: frame f draws a bar from the centre line to level f (f = 16: centre)"""
    im = Image.new("RGBA", (w, h * levels), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for f in range(levels):
        y0 = f * h
        mid = y0 + h // 2
        lv = y0 + int(round((1.0 - f / (levels - 1)) * (h - 4))) + 2
        d.rectangle((0, min(mid, lv), w - 1, max(mid, lv)), fill=colour + (200,))
        d.rectangle((0, lv - 1, w - 1, lv + 1), fill=(255, 255, 255, 255))
    return im


def draw_field():
    im = Image.new("RGBA", (FIELD_W, FIELD_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, FIELD_W - 1, FIELD_H - 1), radius=12, fill=(12, 14, 16, 255), outline=GUN_LINE + (255,), width=2)
    top, h = PTS_Y, PTS_H
    for k in range(9):
        y = top + int(k * (h - 1) / 8)
        d.line((PTS_X - 4, y, PTS_X + 64 * PTS_PITCH, y), fill=GUN_LINE + ((150,) if k == 4 else (45,)), width=1)
    for k in range(9):
        x = PTS_X - 2 + k * 8 * PTS_PITCH
        d.line((x, top, x, top + h), fill=GUN_LINE + (45,), width=1)
    return im


def puck():
    S = 4
    im = Image.new("RGBA", (PUCK * S, PUCK * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((2 * S, 2 * S, (PUCK - 2) * S, (PUCK - 2) * S), fill=GUN + (230,), outline=SILVER + (255,), width=3 * S)
    c = PUCK * S // 2
    d.ellipse((c - 5 * S, c - 5 * S, c + 5 * S, c + 5 * S), fill=SILVER + (255,))
    return im.resize((PUCK, PUCK), Image.LANCZOS)


def led_bar():
    w = LEDS * LED_PITCH
    im = Image.new("RGB", (w, LED_H * (LEDS + 1)), (0x10, 0x12, 0x14))
    d = ImageDraw.Draw(im)
    for lit in range(LEDS + 1):
        for i in range(LEDS):
            x0, y0 = i * LED_PITCH + 1, lit * LED_H + 2
            col = (SILVER if i < 16 else ((0xF0, 0xC0, 0x60) if i < 21 else (0xF0, 0x60, 0x50))) if i < lit else (0x2A, 0x2D, 0x31)
            d.rectangle((x0, y0, x0 + LED_W - 1, y0 + LED_H - 5), fill=col)
    return im


def button(w, h, text, lit, size=21):
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if lit:
        d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, fill=SILVER + (255,), outline=(0xFF, 0xFF, 0xFF, 255), width=2 * S)
        # a little brushed shine
        d.rectangle((3 * S, 3 * S, w * S - 3 * S, h * S // 2), fill=(0xE8, 0xEB, 0xEE, 255))
    else:
        d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, fill=GUN + (255,), outline=GUN_LINE + (255,), width=2 * S)
    while True:
        f = font(size * S, "SemiBold" if lit else "Regular")
        box = d.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= (w - 8) * S or size <= 10:
            break
        size -= 1
    tw, th = box[2] - box[0], box[3] - box[1]
    d.text(((w * S - tw) / 2 - box[0], (h * S - th) / 2 - box[1]), text, font=f, fill=INK + (255,) if lit else (0xE6, 0xE9, 0xEC, 255))
    return im.resize((w, h), Image.LANCZOS)


def hit_image(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 1))
    return im


def art_files():
    """{file: PIL image} for every rendered button"""
    out = {"scope_strip.png": bar_strip(SCOPE_W, SCOPE_H), "pt_strip.png": bar_strip(PTS_W, PTS_H),
           "draw_field.png": draw_field(), "draw_puck.png": puck().resize((30, 30), Image.LANCZOS)}
    done = set()
    for name, caps in SELECTORS.items():
        key = tuple(caps)
        for i, cap in enumerate(caps):
            for lit in (True, False):
                f = "sel_%s_%d_%s.png" % (slug("_".join(caps)), i, "on" if lit else "off")
                if f not in done:
                    out[f] = button(SEG_W[len(caps)], SEG_H, cap, lit, 19 if len(caps) <= 3 else 16)
                    done.add(f)
        del key
    for i, cap in enumerate(CATS):
        for lit in (True, False):
            out["cat_%d_%s.png" % (i, "on" if lit else "off")] = button(BIG_W, BIG_H, cap, lit, 18)
    for name, cap in ACTIONS.items():
        out["act_%s.png" % slug(name)] = button(ACT_W, ACT_H, cap, False)
    for i, cap in enumerate(KEY_CAPS):
        out["key_%d.png" % i] = button(KEY_W, KEY_H, cap, False, 22 if len(cap) == 1 else 17)
    return out


def sel_image(name, i, lit):
    if name in BIG:
        return "cat_%d_%s.png" % (i, "on" if lit else "off")
    return "sel_%s_%d_%s.png" % (slug("_".join(SELECTORS[name])), i, "on" if lit else "off")


# ------------------------------------------------------------------------------------------
# TUI pieces
# ------------------------------------------------------------------------------------------
def ts(height, colour, fnt="Roboto", style="Regular", just="left verticallyCentred", case="Original"):
    return obj(("version", 1),
               ("font", obj(("version", 1), ("name", fnt), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def label(name, bounds, kind, handle, style, text=None):
    pairs = [("version", 1), ("textStyle", style), ("type", kind), ("handleName", STATIC_HANDLE if kind == "Text" else handle)]
    if text is not None:
        pairs.append(("text", text))
    return G.entry(name, "Label", Obj(pairs), bounds, "No", "Show", "Always")


def caption(text, bounds, size=15, colour=MUTED):
    return label("cap " + text, bounds, "Text", None, ts(size, colour, HEAD_FONT, "Light", "left verticallyCentred", "Original"), text)


def image(name, file, bounds, frames=None, handle=None):
    if frames:
        data = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", file), ("numFrames", frames), ("handleName", handle))
    else:
        data = obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", file))
    return G.entry(name, "Image", data, bounds, "No", "Show", "Always")


HIT_SIZES = set()


def help_trigger(item, x, y, w, h, help_param):
    HIT_SIZES.add((w, h))
    img = "hit_%dx%d.png" % (w, h)
    data = obj(("version", 2), ("onImage", img), ("offImage", img), ("buttonId", item),
               ("numButtonsInGroup", HELP_N), ("handleName", help_param.handle), ("gestureBehaviour", "Instant"))
    return G.entry("help %d %d %d" % (item, x, y), "Button", data, "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always")


def focus(w, h):
    return G.entry("focus", "Focus", G.focus_data("ffe6e9ec"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def name_style():
    return ts(17, MUTED, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case")


def knob_def(image_file, value_size=24):
    kids = [focus(CTL_W, CTL_H),
            G.entry("knob", "Knob", G.knob_filmstrip_data(image_file, 127), "8 16 56 56", "No", "Show", "Always"),
            label("name", "78 8 204 30", "Name", "Data", name_style()),
            label("value", "78 42 204 36", "Value", "Data", ts(value_size, TEXT))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def selector_def(name, caps, w, h, cols, height):
    n = len(caps)
    kids = [focus(CTL_W, height),
            label("name", "12 4 140 28", "Name", "Data", name_style()),
            label("value", "140 4 136 28", "Value", "Data", ts(18, TEXT, "Roboto", "Regular", "right verticallyCentred"))]
    rows = (n + cols - 1) // cols
    for i in range(n):
        if name not in BIG:
            x, y = 12 + i * (w + SEG_GAP), SEG_Y
        else:
            c, r = divmod(i, rows)
            x, y = 12 + c * (w + BIG_GAP_X), BIG_Y + r * (h + BIG_GAP_Y)
        kids.append(G.entry("seg %d" % i, "Button", G.button_data(sel_image(name, i, True), sel_image(name, i, False), i, n),
                            "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always"))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def toggle_def():
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 8 190 30", "Name", "Data", name_style()),
            label("value", "12 42 190 36", "Value", "Data", ts(24, TEXT)),
            G.entry("toggle", "Button", G.button_data("elver_toggle_on.png", "elver_toggle_off.png", 1, 1), "210 28 64 32", "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def action_def(name):
    img = "act_%s.png" % slug(name)
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 4 270 26", "Name", "Data", name_style()),
            G.entry("button", "Button", G.button_data(img, img, 1, 1), "12 34 %d %d" % (ACT_W, ACT_H), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def key_def(i):
    img = "key_%d.png" % i
    kids = [G.entry("key", "Button", G.button_data(img, img, 1, 1), "0 0 %d %d" % (KEY_W, KEY_H), "No", "Show", "Always")]
    return G.component_definition([], kids, "00000000")


def slot(name, comp, bounds, p, focusable="Yes"):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, focusable, "Hide", "Always", remap=[("Data", p.handle)])


def find(o, key):
    for k, v in o:
        if k == key:
            return v
    raise KeyError(key)


def set_key(o, key, value):
    for i, (k, v) in enumerate(o):
        if k == key:
            o[i] = (k, value)
            return
    raise KeyError(key)


def build_tab(page_name, sections, by_name, help1, help2):
    kids = [image("panel", "elver_panel.png", "0 0 %d %d" % (W, H), len(SKINS), by_name["Skin"].handle)]
    trig = []
    for s, (header, rows) in enumerate(sections):
        x0 = SEC_X0 + s * SEC_PITCH
        if rows == "SPAN" or (isinstance(rows, str) and rows.startswith("DRAWPAD:")):
            header = header   # drawn below
        else:
            kids.append(label("section %d" % s, "%d %d %d 34" % (x0 + 28, SEC_Y + 4, SEC_W - 56), "Text", None,
                          ts(22, TEXT, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case"), header))
        if rows == "PAD":
            x, y, w, h = PAD
            kids.append(image("pad field", "pad_field.png", "%d %d %d %d" % (x, y, w, h)))
            kids.append(G.entry("xy pad", "XYPad", obj(("version", 1), ("xPositionHandle", by_name["Blend"].handle),
                                                       ("yPositionHandle", by_name["Pad Y"].handle), ("image", "puck.png")),
                                "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always"))
            kids.append(slot("Pad Y To", "sel_pad_y_to", "%d %d %d %d" % (x0 + 6, ROW_Y0 + 3 * ROW_PITCH, CTL_W, CTL_H), by_name["Pad Y To"]))
            trig.append((HELP_FOR["PAD"], x0 + 28, SEC_Y + 4, SEC_W - 56, 34))
            trig.append((HELP_FOR["Pad Y To"], x0 + 18, ROW_Y0 + 3 * ROW_PITCH + 2, 126, 30))
            continue
        if rows == "SPAN":
            continue
        if isinstance(rows, str) and rows.startswith("SCOPE:"):
            side = rows[-1]
            for j in range(32):
                kids.append(image("scope %d" % j, "scope_strip.png", "%d %d %d %d" % (x0 + SCOPE_X + j * SCOPE_PITCH, SEC_Y + SCOPE_Y, SCOPE_W, SCOPE_H),
                                  33, by_name["%s Scope %02d" % (side, j + 1)].handle))
            kids.append(caption("one cycle " + SEP + " what shape plays", "%d %d 260 22" % (x0 + 22, SEC_Y + SCOPE_Y + SCOPE_H + 12), 14))
            trig.append((38, x0 + 22, SEC_Y + SCOPE_Y, 260, SCOPE_H))
            continue
        if isinstance(rows, str) and rows.startswith("DRAWPAD:"):
            side = rows[-1]
            kids.append(image("draw field", "draw_field.png", "%d %d %d %d" % (x0, SEC_Y, FIELD_W, FIELD_H)))
            kids.append(label("section %d" % s, "%d %d %d 34" % (x0 + 20, SEC_Y + 6, FIELD_W - 40), "Text", None,
                              ts(22, TEXT, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case"), header))
            for j in range(64):
                kids.append(image("pt %d" % j, "pt_strip.png", "%d %d %d %d" % (x0 + PTS_X + j * PTS_PITCH, SEC_Y + PTS_Y, PTS_W, PTS_H),
                                  33, by_name["%s Pt %02d" % (side, j + 1)].handle))
            kids.append(G.entry("draw pad", "XYPad", obj(("version", 1), ("xPositionHandle", by_name[side + " Draw X"].handle),
                                                         ("yPositionHandle", by_name[side + " Draw Y"].handle), ("image", "draw_puck.png")),
                                "%d %d %d %d" % (x0 + PTS_X, SEC_Y + PTS_Y, 64 * PTS_PITCH, PTS_H), "No", "Show", "Always"))
            trig.append((36, x0 + 20, SEC_Y + 6, FIELD_W - 40, 34))
            continue
        if isinstance(rows, str) and rows.startswith("KEYS"):
            # alphabetical within each plate: A..U on the left; V..Z, space, digits, '-' and editing keys right
            order = KEY_NAMES[:21] if rows == "KEYS0" else KEY_NAMES[21:26] + ["Key Space"] + KEY_NAMES[26:36] + ["Key -", "Backspace", "Left", "Right", "Clear"]
            for idx, kname in enumerate(order):
                k = KEY_NAMES.index(kname)
                r, c = divmod(idx, 3)
                kx = x0 + 12 + c * (KEY_W + KEY_GX)
                ky = SEC_Y + KEY_Y0 + r * (KEY_H + KEY_GY)
                kids.append(slot(kname, "key_%d" % k, "%d %d %d %d" % (kx, ky, KEY_W, KEY_H), by_name[kname], "No"))
            continue
        for r, row in enumerate(rows):
            if row is None or row == MORE:
                continue
            x, y = x0 + 6, ROW_Y0 + r * ROW_PITCH
            if isinstance(row, tuple):
                _, pname, cap = row
                kids.append(caption(cap, "%d %d 270 26" % (x + 12, y + 4)))
                kids.append(label("%s value" % pname, "%d %d 276 50" % (x + 12, y + 32), "Value", by_name[pname].handle, ts(20, TEXT)))
                trig.append((HELP_FOR[pname], x + 12, y + 2, 270, 30))
                continue
            p = by_name[row]
            h = CTL_H
            if row in TOGGLES:
                comp, hx = "toggleC", (x + 12, y + 6, 180, 32)
            elif row in BIG:
                h = 3 * ROW_PITCH - 4
                comp, hx = "sel_" + slug(row), (x + 12, y + 2, 126, 30)
            elif row in SELECTORS:
                comp, hx = "sel_" + slug(row), (x + 12, y + 2, 126, 30)
            elif row in ACTIONS:
                comp, hx = "act_" + slug(row), (x + 12, y + 2, 270, 28)
            elif row == "Preset":
                comp, hx = "knobName", (x + 78, y + 6, 200, 32)
            else:
                comp, hx = "knobC", (x + 78, y + 6, 200, 32)
            kids.append(slot(row, comp, "%d %d %d %d" % (x, y, CTL_W, h), p))
            trig.append((HELP_FOR[row],) + hx)
    # bottom strip: CPU meter, voices, help
    kids.append(caption("cpu " + SEP + " elver's own load", "%d %d 270 26" % (32, STRIP_Y + 8)))
    kids.append(image("cpu meter", "led_bar.png", "%d %d %d %d" % (32, STRIP_Y + 48, LEDS * LED_PITCH, LED_H), LEDS + 1, by_name["CPU"].handle))
    kids.append(label("cpu value", "%d %d 270 40" % (32, STRIP_Y + 80), "Value", by_name["CPU"].handle, ts(26, TEXT)))
    kids.append(caption("0 . . . . . 25 %", "%d %d 270 22" % (32, STRIP_Y + 124), 13))
    trig.append((29, 18, STRIP_Y, 300, STRIP_H))
    kids.append(caption("voices " + SEP + " playing / limit", "%d %d 200 26" % (350, STRIP_Y + 8)))
    kids.append(label("voices value", "%d %d 150 44" % (350, STRIP_Y + 52), "Value", by_name["Voices Now"].handle, ts(30, TEXT)))
    kids.append(caption("elver " + SEP + " sure eel", "%d %d 140 22" % (350, STRIP_Y + 124), 13))
    trig.append((29, 336, STRIP_Y, 150, STRIP_H))
    hw = 1262 - HELP_X
    kids.append(caption("help " + SEP + " touch a name or a meter", "%d %d %d 26" % (HELP_X + 20, STRIP_Y + 8, hw - 40)))
    kids.append(label("help 1", "%d %d %d 36" % (HELP_X + 20, STRIP_Y + 48, hw - 40), "Value", help1.handle, ts(22, TEXT)))
    kids.append(label("help 2", "%d %d %d 36" % (HELP_X + 20, STRIP_Y + 90, hw - 40), "Value", help2.handle, ts(22, TEXT)))
    trig.append((0, HELP_X, STRIP_Y, hw, STRIP_H))
    for t in trig:
        kids.append(help_trigger(*t, help1))
    return G.component_definition([], kids, "ff1a2a33")


def write_ui_json():
    pages = []
    for name, sections in PAGES:
        controls = []
        for r in range(4):
            for c in range(4):
                rows = sections[c][1] if c < len(sections) else None
                if rows == "PAD":
                    row = ["Blend", "Pad Y", None, "Pad Y To"][r]
                elif isinstance(rows, str) and rows.startswith("DRAWPAD:"):
                    row = [rows[-1] + " Draw X", rows[-1] + " Draw Y", rows[-1] + " Draw Mix", None][r]
                elif rows is None or isinstance(rows, str):
                    row = None
                else:
                    row = rows[r]
                if row is None or row == MORE or isinstance(row, tuple):
                    controls.append("empty")
                elif row in TOGGLES or row in ACTIONS or (row in SELECTORS and len(SELECTORS[row]) <= 4):   # (wider selectors: listed as knobs; the page is ours)
                    controls.append({"param": row, "widget": "switch"})
                else:
                    controls.append({"param": row, "widget": "knob"})
        while controls and controls[-1] == "empty":
            controls.pop()
        pages.append({"name": name, "controls": controls})
    ui = {"kit": "mono", "pages": pages,
          "style": {"background": "ff1a2a33", "text": TEXT, "muted": MUTED, "accent": "ffe6e9ec"}}
    with open(os.path.join(PROJECT, "ui.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(ui, f, indent=2)
        f.write("\n")


def main():
    write_ui_json()
    build = os.path.join(PROJECT, "build")
    params = G.load_params(os.path.join(build, "params.json"))
    pages, style, knob_image, frames, kit = G.load_ui(os.path.join(PROJECT, "ui.json"), G.ParamTable(params))
    kit_dir, kit_assets = G.kit_dir_for(kit, build, None)
    tui = G.build_tui(pages, params, style, knob_image, frames, kit_assets)
    qlinks = G.build_qlinks(pages, params)
    by_name = {p.name: p for p in params}
    help1, help2 = by_name["Help"], by_name["Help 2"]

    page_data = find(tui, "pageData")
    defs = find(find(page_data, "componentDefinitions"), "localComponentDefinitions")
    defs.append(obj(("key", "knobC"), ("value", knob_def("elver_knob_small_uni.png"))))
    defs.append(obj(("key", "knobName"), ("value", knob_def("elver_knob_small_uni.png", 19))))
    defs.append(obj(("key", "toggleC"), ("value", toggle_def())))
    for name, caps in SELECTORS.items():
        defs.append(obj(("key", "sel_" + slug(name)), ("value", selector_def(name, caps, SEG_W[len(caps)], SEG_H, len(caps), CTL_H))))
    for name, caps in BIG.items():
        defs.append(obj(("key", "sel_" + slug(name)), ("value", selector_def(name, caps, BIG_W, BIG_H, 2, 3 * ROW_PITCH - 4))))
    for name in ACTIONS:
        defs.append(obj(("key", "act_" + slug(name)), ("value", action_def(name))))
    for i in range(len(KEY_CAPS)):
        defs.append(obj(("key", "key_%d" % i), ("value", key_def(i))))

    tabs = find(page_data, "tabs")
    assert [p.name for p in pages] == [n for n, _ in PAGES], "ui.json pages out of step"
    for (name, sections), tab in zip(PAGES, tabs):
        set_key(tab, "componentDefinition", build_tab(name, sections, by_name, help1, help2))
        set_key(tab, "qlinkBoundsData", ["%d %d %d %d" % (SEC_X0 + s * SEC_PITCH, SEC_Y, SEC_W, SEC_H) for s in range(4)])
    for entry in find(find(qlinks, "Screen Mode Q-Links"), "map"):
        set_key(entry, "Bank Direction", "Column")
    set_key(qlinks, "Program Mode Q-Links", Obj(("Q-Link %d" % (q + 1), by_name[n].index) for q, n in enumerate(PROGRAM_QLINKS)))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    art = art_files()
    for f, im in art.items():
        im.save(os.path.join(OUT, f), optimize=True)
    # the mono kit's knob and toggles, as they are (neutral silver on gunmetal)
    for kitf in ("knob_small_uni.png", "toggle_on.png", "toggle_off.png"):
        shutil.copy2(os.path.join(kit_dir, kitf), os.path.join(OUT, "elver_" + kitf))
    own = set(art) | {"elver_panel.png", "pad_field.png", "puck.png", "led_bar.png"} | {"hit_%dx%d.png" % s for s in HIT_SIZES} \
        | {"elver_" + k for k in ("knob_small_uni.png", "toggle_on.png", "toggle_off.png")}
    for img in sorted(G.referenced_images(tui, set()) - own):
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
    background().save(os.path.join(OUT, "elver_panel.png"), optimize=True)
    pad_field().save(os.path.join(OUT, "pad_field.png"), optimize=True)
    puck().save(os.path.join(OUT, "puck.png"), optimize=True)
    led_bar().save(os.path.join(OUT, "led_bar.png"), optimize=True)
    for w, h in sorted(HIT_SIZES):
        hit_image(w, h).save(os.path.join(OUT, "hit_%dx%d.png" % (w, h)))
    missing = sorted(i for i in G.referenced_images(tui, set()) if not os.path.isfile(os.path.join(OUT, i)))
    if missing:
        sys.exit("make_skin: referenced images missing: %s" % ", ".join(missing))
    with open(os.path.join(OUT, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(OUT, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    total = sum(os.path.getsize(os.path.join(OUT, n)) for n in os.listdir(OUT))
    print("make_skin: wrote %s (%d files, %.0f KB)" % (OUT, len(os.listdir(OUT)), total / 1024))


if __name__ == "__main__":
    main()
