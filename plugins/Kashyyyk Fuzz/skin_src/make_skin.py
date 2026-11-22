"""Builds Kashyyyk Fuzz's hand-written skin (plugins/Kashyyyk Fuzz/skins/) for the MPC touch screen.

PEDAL: the pedal graphic. FUZZ / TONE / LEVEL big knobs and BLEND; the circuit knobs SAG, STAGES, TOPOLOGY, FEEDBACK
below them; the circuit board shows the parts in their slots (transistor pair, diodes, input cap) as pictures that
swap in place (film strips bound to the slot parameters); tapping a picture steps to the next part (invisible button
on a "Next" action parameter). Right (global, not in presets): Guitar Vol, Vol Follow (Off / Auto) and LEARN, Input,
Output, the volume / status / CPU / circuit readouts.
PARTS: one row of picture buttons per slot (radio groups on the slot parameters), the selected part lit.
CIRCUIT: the circuit family, stage count (+ Dual Mix), the later stages' transistor (Q2), coupling caps, resistor type
and the Buffer (global), in the PARTS style.
Q-Links ("Column" banking, one meaning per column): PEDAL (1) Fuzz Tone Level Blend (2) Sag Stages Topology Feedback
(3) the four parts (4) Guitar Vol, Vol Follow, Input, Output. PARTS (1) the four parts (2) Tone Stack. CIRCUIT
(1) Topology Stages Dual Mix (2) Q2 Coupling Resistors Buffer. SAVE (1) Name Pos, Letter.
SAVE: RANDOM / UNDO, the name editor (Name Pos and Letter knobs) with SAVE / CANCEL, the patch status.
The Sure Eel logo sits on the pedal between the title and the LED (the logo is not MIT-licensed: see LICENSE).

Rules learned on the MPC hardware: nothing on top of a button except invisible hit areas; button text drawn into
the images; static text bound to the never-changing "Panel" parameter; no conditional visibility; no pop-up menus
(the menu overlay showed an empty list on the MPC); Q-Links in columns with "Column" banking and focus rectangles
(proven with Elver and Sargasso). Plain ASCII in every label.

usage (from the SDK root, after one build so build/params.json exists; needs Pillow):
    python "plugins/Kashyyyk Fuzz/skin_src/make_skin.py"
    python tools/build.py "plugins/Kashyyyk Fuzz"
"""
import math
import os
import random
import shutil
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
LOGO = os.path.join(HERE, "reference", "Sure eel logo.jpg")
FONT_FILE = r"C:\Windows\Fonts\bahnschrift.ttf"
INK, MUTED, CREAM = "ffefe6d2", "ffa89f8a", "fff3e9c8"
LED_GREEN_TXT, LED_AMBER_TXT = "ff7ff59a", "ffffc04d"
P = {}

TRANSISTORS = ["OC44", "2N2613", "AC107", "2N3904", "2N5088"]
DIODES = ["1N34A", "1N914", "1N6263", "D9B", "LED"]
ARRS = ["PAIR", "SINGLE", "2+1 ASYM", "NONE"]
CAPS = ["1n5", "10n", "47n", "100uF"]
TONES = ["MUFF", "FLAT", "MID BOOST", "TILT", "BYPASS"]
TOPOS = ["FACE", "BENDER", "MUFF", "OP-AMP", "OCTAVE", "DUAL"]
STAGE_N = ["1", "2", "3"]
Q2S = ["SAME"] + TRANSISTORS
COUPLINGS = ["10n FILM", "47n FILM", "100n FILM", "1u ELEC", "10u ELEC", "100u ELEC"]
RESISTORS = ["METAL FILM", "CARBON COMP"]
BUFFERS = ["OFF", "ON"]

# geometry (PEDAL)
PEDAL = (40, 18, 830, 594)
BIGK, BIGF = 118, 64                       # big knob film strip: 118 px, 64 frames
BOARD = (80, 312, 750, 226)                # the circuit board window
SLOT_W, SLOT_H = 200, 110
SLOTS = [("Q1 Q2", "Transistors", "Next Q", 110), ("D1 D2", "Diodes", "Next D", 350), ("C IN", "Input Cap", "Next C", 590)]
RX = 900                                   # right column
KNOB_W, KNOB_H, KNOB_SIZE = 300, 84, 56


def font(size, variant="SemiBold"):
    f = ImageFont.truetype(FONT_FILE, size)
    try:
        f.set_variation_by_name(variant)
    except Exception:
        pass
    return f


# ---------------------------------------------------------------------------------------------------------------
# Part pictures (drawn, no outside art). Each draws into a W x H RGBA image at 3x and scales down.
# ---------------------------------------------------------------------------------------------------------------
def canvas(w, h, S=3):
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im), S


def finish(im, w, h):
    return im.resize((w, h), Image.LANCZOS)


def draw_transistor(d, S, cx, top, name):
    """One transistor at (cx, top): metal can (germanium) or TO-92 plastic (silicon), three legs."""
    ge = name in ("OC44", "2N2613", "AC107")
    for k in (-1, 0, 1):                                            # legs
        d.line([((cx + 9 * k) * S, (top + 34) * S), ((cx + 9 * k) * S, (top + 62) * S)], fill=(190, 192, 196, 255), width=2 * S)
    if name == "OC44":       # black-painted glass body with the red collector dot (the classic look)
        d.rounded_rectangle([(cx - 16) * S, top * S, (cx + 16) * S, (top + 36) * S], radius=6 * S, fill=(22, 22, 24, 255), outline=(70, 70, 74, 255), width=S)
        d.ellipse([(cx - 13) * S, (top + 26) * S, (cx - 7) * S, (top + 32) * S], fill=(220, 40, 40, 255))
    elif ge:                 # metal can with a tab
        col = (196, 198, 204) if name == "2N2613" else (170, 150, 110)
        d.ellipse([(cx - 17) * S, (top - 2) * S, (cx + 17) * S, (top + 10) * S], fill=col + (255,))
        d.rectangle([(cx - 17) * S, (top + 4) * S, (cx + 17) * S, (top + 32) * S], fill=tuple(int(v * 0.85) for v in col) + (255,))
        d.ellipse([(cx - 17) * S, (top + 26) * S, (cx + 17) * S, (top + 38) * S], fill=tuple(int(v * 0.7) for v in col) + (255,))
        d.polygon([((cx + 14) * S, (top + 28) * S), ((cx + 22) * S, (top + 34) * S), ((cx + 17) * S, (top + 36) * S)], fill=col + (255,))
    else:                    # TO-92 black plastic, flat face forward
        d.pieslice([(cx - 17) * S, (top - 4) * S, (cx + 17) * S, (top + 30) * S], 180, 360, fill=(30, 30, 32, 255))
        d.rectangle([(cx - 17) * S, (top + 13) * S, (cx + 17) * S, (top + 36) * S], fill=(30, 30, 32, 255))
        d.rectangle([(cx - 15) * S, (top + 15) * S, (cx + 15) * S, (top + 34) * S], fill=(42, 42, 46, 255))


def pic_transistor_pair(name, w=SLOT_W, h=SLOT_H):
    im, d, S = canvas(w, h)
    for cx in (w // 2 - 36, w // 2 + 36):
        draw_transistor(d, S, cx, 22, name)
    return finish(im, w, h)


def draw_diode(d, S, cx, cy, name, vertical=False):
    """One axial diode centred at (cx, cy), horizontal."""
    d.line([((cx - 40) * S, cy * S), ((cx + 40) * S, cy * S)], fill=(190, 192, 196, 255), width=2 * S)       # leads
    if name == "LED":        # a red 5 mm LED: dome, flange, two legs
        d.line([((cx - 6) * S, (cy + 6) * S), ((cx - 6) * S, (cy + 26) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.line([((cx + 6) * S, (cy + 6) * S), ((cx + 6) * S, (cy + 30) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.rectangle([(cx - 40) * S, (cy - 2) * S, (cx + 40) * S, (cy + 2) * S], fill=(0, 0, 0, 0))
        d.rectangle([(cx - 13) * S, (cy + 2) * S, (cx + 13) * S, (cy + 7) * S], fill=(200, 30, 30, 255))
        d.pieslice([(cx - 11) * S, (cy - 22) * S, (cx + 11) * S, (cy + 6) * S], 180, 360, fill=(230, 40, 40, 230))
        d.rectangle([(cx - 11) * S, (cy - 8) * S, (cx + 11) * S, (cy + 4) * S], fill=(230, 40, 40, 230))
        d.ellipse([(cx - 6) * S, (cy - 18) * S, (cx - 1) * S, (cy - 10) * S], fill=(255, 190, 190, 200))
        return
    body = {"1N34A": (200, 210, 200, 150), "1N914": (230, 120, 40, 230), "1N6263": (40, 40, 44, 255), "D9B": (190, 205, 190, 160)}[name]
    L = 30 if name == "D9B" else 24
    d.rounded_rectangle([(cx - L // 2) * S, (cy - 6) * S, (cx + L // 2) * S, (cy + 6) * S], radius=5 * S, fill=body)
    band = (20, 20, 20, 255) if name != "1N6263" else (200, 200, 210, 255)
    d.rectangle([(cx + L // 2 - 6) * S, (cy - 6) * S, (cx + L // 2 - 3) * S, (cy + 6) * S], fill=band)
    if name in ("1N34A", "D9B"):    # visible whisker inside the glass
        d.line([((cx - L // 2 + 3) * S, cy * S), ((cx + 2) * S, (cy - 3) * S), ((cx + 5) * S, cy * S)], fill=(60, 60, 60, 255), width=S)
    if name == "D9B":               # the colour code dots
        d.ellipse([(cx - 8) * S, (cy - 3) * S, (cx - 4) * S, (cy + 1) * S], fill=(220, 40, 40, 255))


def pic_diode_pair(name, w=SLOT_W, h=SLOT_H):
    im, d, S = canvas(w, h)
    if name == "EMPTY":      # arrangement None: two empty pairs of solder pads
        for cy in (38, 76):
            for cx in (w // 2 - 40, w // 2 + 40):
                d.ellipse([(cx - 5) * S, (cy - 5) * S, (cx + 5) * S, (cy + 5) * S], fill=(200, 170, 90, 255))
                d.ellipse([(cx - 2) * S, (cy - 2) * S, (cx + 2) * S, (cy + 2) * S], fill=(20, 40, 25, 255))
        return finish(im, w, h)
    draw_diode(d, S, w // 2, 38, name)
    draw_diode(d, S, w // 2, 76, name)
    return finish(im, w, h)


def draw_cap(d, S, cx, cy, name):
    if name == "100uF":     # electrolytic can
        d.line([((cx - 8) * S, (cy + 24) * S), ((cx - 8) * S, (cy + 40) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.line([((cx + 8) * S, (cy + 24) * S), ((cx + 8) * S, (cy + 40) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.rounded_rectangle([(cx - 22) * S, (cy - 34) * S, (cx + 22) * S, (cy + 26) * S], radius=6 * S, fill=(30, 70, 150, 255))
        d.rectangle([(cx + 8) * S, (cy - 34) * S, (cx + 16) * S, (cy + 26) * S], fill=(210, 220, 235, 255))
        d.text(((cx + 9) * S, (cy - 10) * S), "-", font=font(14 * S, "Bold"), fill=(30, 70, 150, 255))
        d.ellipse([(cx - 22) * S, (cy - 40) * S, (cx + 22) * S, (cy - 28) * S], fill=(150, 160, 175, 255))
        return
    size = {"1n5": 14, "10n": 18, "47n": 24}[name]
    col = {"1n5": (200, 60, 50), "10n": (205, 70, 55), "47n": (60, 110, 180)}[name]
    d.line([((cx - 6) * S, (cy + size) * S), ((cx - 6) * S, (cy + size + 18) * S)], fill=(190, 192, 196, 255), width=2 * S)
    d.line([((cx + 6) * S, (cy + size) * S), ((cx + 6) * S, (cy + size + 18) * S)], fill=(190, 192, 196, 255), width=2 * S)
    d.rounded_rectangle([(cx - size) * S, (cy - size) * S, (cx + size) * S, (cy + size) * S], radius=3 * S, fill=col + (255,))
    d.text(((cx - size + 4) * S, (cy - 8) * S), name, font=font(11 * S, "Bold"), fill=(250, 240, 230, 255))


def pic_cap(name, w=SLOT_W, h=SLOT_H):
    im, d, S = canvas(w, h)
    draw_cap(d, S, w // 2, 52, name)
    return finish(im, w, h)


def pic_arr(name, w, h):
    """Diode arrangement as a schematic: pair, single, 2+1, none."""
    im, d, S = canvas(w, h)
    col = (235, 225, 200, 255)
    def diode(x, y, up):
        a = (y - 8) if up else (y + 8)
        b = (y + 8) if up else (y - 8)
        d.polygon([((x - 8) * S, b * S), ((x + 8) * S, b * S), (x * S, a * S)], outline=col, width=2 * S)
        d.line([((x - 8) * S, a * S), ((x + 8) * S, a * S)], fill=col, width=2 * S)
    cy = h // 2 - 4
    d.line([(14 * S, (cy - 18) * S), ((w - 14) * S, (cy - 18) * S)], fill=col, width=2 * S)
    d.line([(14 * S, (cy + 18) * S), ((w - 14) * S, (cy + 18) * S)], fill=col, width=2 * S)
    if name == "PAIR":
        for x, up in ((w // 2 - 16, True), (w // 2 + 16, False)):
            d.line([(x * S, (cy - 18) * S), (x * S, (cy + 18) * S)], fill=col, width=2 * S); diode(x, cy, up)
    elif name == "SINGLE":
        x = w // 2; d.line([(x * S, (cy - 18) * S), (x * S, (cy + 18) * S)], fill=col, width=2 * S); diode(x, cy, True)
    elif name == "2+1 ASYM":
        x = w // 2 - 18
        d.line([(x * S, (cy - 18) * S), (x * S, (cy + 18) * S)], fill=col, width=2 * S); diode(x, cy - 8, True); diode(x, cy + 8, True)
        x = w // 2 + 18; d.line([(x * S, (cy - 18) * S), (x * S, (cy + 18) * S)], fill=col, width=2 * S); diode(x, cy, False)
    else:
        d.line([((w // 2 - 14) * S, (cy - 10) * S), ((w // 2 + 14) * S, (cy + 10) * S)], fill=col, width=2 * S)
        d.line([((w // 2 + 14) * S, (cy - 10) * S), ((w // 2 - 14) * S, (cy + 10) * S)], fill=col, width=2 * S)
    return finish(im, w, h)


def pic_tone(name, w, h):
    """A tone stack as its rough response curve."""
    im, d, S = canvas(w, h)
    col = (235, 225, 200, 255)
    pts = []
    for i in range(41):
        x = i / 40.0
        y = {"MUFF": 0.5 - 0.28 * math.cos(2 * math.pi * x), "FLAT": 0.5, "MID BOOST": 0.5 - 0.3 * math.exp(-((x - 0.5) / 0.15) ** 2),
             "TILT": 0.75 - 0.5 * x, "BYPASS": 0.5}[name]
        pts.append(((12 + x * (w - 24)) * S, (8 + y * (h - 34)) * S))
    if name == "BYPASS":
        d.line(pts, fill=(140, 135, 120, 255), width=2 * S)
        d.polygon([((w - 18) * S, (h // 2 - 15) * S), ((w - 10) * S, (h // 2 - 11) * S), ((w - 18) * S, (h // 2 - 7) * S)], fill=col)
    else:
        d.line(pts, fill=col, width=3 * S)
    return finish(im, w, h)


def draw_amp(d, S, x, cy, col, size=14):
    """an amplifier stage as a triangle"""
    d.polygon([(x * S, (cy - size) * S), ((x + 2 * size) * S, cy * S), (x * S, (cy + size) * S)], outline=col, width=2 * S)


def pic_topo(name, w, h):
    """the circuit family as a block diagram: stages (triangles), diodes, the rectifier, parallel paths"""
    im, d, S = canvas(w, h)
    col, dim = (235, 225, 200, 255), (160, 150, 130, 255)
    cy = h // 2 - 6
    def tri(x, y=cy, s=12): draw_amp(d, S, x, y, col, s)
    def dio(x, y):
        d.polygon([((x - 6) * S, (y - 6) * S), ((x + 6) * S, (y - 6) * S), (x * S, (y + 4) * S)], fill=col)
        d.line([((x - 6) * S, (y + 5) * S), ((x + 6) * S, (y + 5) * S)], fill=col, width=2 * S)
    if name == "DUAL":
        d.line([(10 * S, cy * S), (24 * S, cy * S)], fill=dim, width=2 * S)
        for yy in (cy - 14, cy + 14):
            d.line([(24 * S, cy * S), (24 * S, yy * S), (40 * S, yy * S)], fill=dim, width=2 * S)
            tri(40, yy, 9); tri(64, yy, 9)
            d.line([(82 * S, yy * S), (100 * S, yy * S), (100 * S, cy * S)], fill=dim, width=2 * S)
        d.line([(100 * S, cy * S), ((w - 10) * S, cy * S)], fill=dim, width=2 * S)
        return finish(im, w, h)
    d.line([(8 * S, cy * S), ((w - 8) * S, cy * S)], fill=dim, width=2 * S)
    if name == "FACE":
        tri(30); tri(66)
    elif name == "BENDER":
        tri(18); tri(52); tri(86)
    elif name == "MUFF":
        tri(22); dio(36, cy - 20); tri(64); dio(78, cy - 20)
        for x in (22, 64):
            d.line([(x * S, (cy - 20) * S), ((x + 28) * S, (cy - 20) * S)], fill=dim, width=2 * S)
    elif name == "OP-AMP":
        draw_amp(d, S, 34, cy, col, 18)
        d.text((40 * S, (cy - 10) * S), "-", font=font(12 * S, "Bold"), fill=col)
        d.text((40 * S, (cy + 0) * S), "+", font=font(12 * S, "Bold"), fill=col)
        dio(52, cy - 24); d.line([(34 * S, (cy - 24) * S), (70 * S, (cy - 24) * S)], fill=dim, width=2 * S)
    else:   # OCTAVE: a preamp into a full-wave rectifier, then a stage
        tri(14, cy, 10)
        d.rectangle([(44 * S, (cy - 14) * S), (72 * S, (cy + 14) * S)], fill=(22, 20, 18, 255), outline=col, width=2 * S)
        d.arc([(48 * S, (cy - 8) * S), (58 * S, (cy + 8) * S)], 180, 360, fill=col, width=2 * S)
        d.arc([(58 * S, (cy - 8) * S), (68 * S, (cy + 8) * S)], 180, 360, fill=col, width=2 * S)
        tri(84, cy, 10)
    return finish(im, w, h)


def pic_stages(name, w, h):
    im, d, S = canvas(w, h)
    col, dim = (235, 225, 200, 255), (160, 150, 130, 255)
    n = int(name); cy = h // 2 - 6
    d.line([(10 * S, cy * S), ((w - 10) * S, cy * S)], fill=dim, width=2 * S)
    span = 30 * n
    x0 = (w - span) // 2
    for k in range(n):
        draw_amp(d, S, x0 + 30 * k, cy, col, 11)
    return finish(im, w, h)


def pic_q2(name, w=SLOT_W, h=SLOT_H):
    if name == "SAME":
        im, d, S = canvas(w, h)
        f = font(40 * S, "Bold")
        box = d.textbbox((0, 0), "= Q1", font=f)
        d.text(((w * S - (box[2] - box[0])) // 2 - box[0], (h * S - (box[3] - box[1])) // 2 - box[1] - 8 * S), "= Q1", font=f, fill=(235, 225, 200, 255))
        return finish(im, w, h)
    im, d, S = canvas(w, h)
    draw_transistor(d, S, w // 2, 22, name)
    return finish(im, w, h)


def pic_coupling(name, w=SLOT_W, h=SLOT_H):
    """a coupling capacitor: a film box (10n / 47n / 100n) or an electrolytic can (1u / 10u / 100u)"""
    im, d, S = canvas(w, h)
    cx, cy = w // 2, 50
    val, kind = name.split()
    if kind == "FILM":
        sw, sh = {"10n": (16, 14), "47n": (22, 18), "100n": (26, 22)}[val]
        col = {"10n": (200, 60, 50), "47n": (60, 110, 180), "100n": (220, 170, 40)}[val]
        for k in (-1, 1):
            d.line([((cx + 6 * k) * S, (cy + sh) * S), ((cx + 6 * k) * S, (cy + sh + 18) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.rounded_rectangle([(cx - sw) * S, (cy - sh) * S, (cx + sw) * S, (cy + sh) * S], radius=3 * S, fill=col + (255,))
    else:
        r, top = {"1u": (10, 22), "10u": (15, 28), "100u": (22, 36)}[val]
        for k in (-1, 1):
            d.line([((cx + 6 * k) * S, (cy + 24) * S), ((cx + 6 * k) * S, (cy + 40) * S)], fill=(190, 192, 196, 255), width=2 * S)
        d.rounded_rectangle([(cx - r) * S, (cy - top) * S, (cx + r) * S, (cy + 26) * S], radius=5 * S, fill=(30, 70, 150, 255))
        d.rectangle([(cx + r - 7) * S, (cy - top) * S, (cx + r - 2) * S, (cy + 26) * S], fill=(210, 220, 235, 255))
        d.ellipse([(cx - r) * S, (cy - top - 5) * S, (cx + r) * S, (cy - top + 5) * S], fill=(150, 160, 175, 255))
    return finish(im, w, h)


def pic_resistor(name, w=SLOT_W, h=SLOT_H):
    """a metal film (blue, five bands) or carbon comp (brown, chunky, three bands) resistor"""
    im, d, S = canvas(w, h)
    cx, cy = w // 2, 48
    d.line([((cx - 60) * S, cy * S), ((cx + 60) * S, cy * S)], fill=(190, 192, 196, 255), width=2 * S)
    if name == "METAL FILM":
        d.rounded_rectangle([(cx - 30) * S, (cy - 10) * S, (cx + 30) * S, (cy + 10) * S], radius=8 * S, fill=(90, 150, 210, 255))
        for k, c in enumerate([(150, 75, 0), (0, 0, 0), (0, 0, 0), (255, 140, 0), (150, 75, 0)]):
            x = cx - 20 + 9 * k + (6 if k == 4 else 0)
            d.rectangle([x * S, (cy - 10) * S, (x + 4) * S, (cy + 10) * S], fill=c + (255,))
    else:
        d.rounded_rectangle([(cx - 26) * S, (cy - 13) * S, (cx + 26) * S, (cy + 13) * S], radius=12 * S, fill=(140, 90, 50, 255))
        for k, c in enumerate([(150, 75, 0), (0, 0, 0), (255, 140, 0)]):
            x = cx - 18 + 10 * k
            d.rectangle([x * S, (cy - 13) * S, (x + 5) * S, (cy + 13) * S], fill=c + (255,))
    return finish(im, w, h)


def pic_buffer(name, w, h):
    """a buffer in front: a triangle in the cable (ON) or a straight cable (OFF)"""
    im, d, S = canvas(w, h)
    col, dim = (235, 225, 200, 255), (160, 150, 130, 255)
    cy = h // 2 - 6
    d.line([(10 * S, cy * S), ((w - 10) * S, cy * S)], fill=dim, width=2 * S)
    if name == "ON":
        draw_amp(d, S, w // 2 - 14, cy, col, 14)
        d.text(((w // 2 - 10) * S, (cy - 7) * S), "1", font=font(11 * S, "Bold"), fill=col)
    return finish(im, w, h)


def selector_button(icon, label, lit, w, h):
    """A PARTS page button: the part picture with its name drawn in; lit = selected (warm backlight, LED bar)."""
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bot = ((92, 74, 48), (64, 50, 32)) if lit else ((48, 44, 38), (32, 29, 25))
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=8 * S, fill=(10, 9, 8, 255))
    for i in range(2 * S, h * S - 2 * S):
        t = i / (h * S)
        d.line((2 * S, i, w * S - 1 - 2 * S, i), fill=tuple(int(top[c] + (bot[c] - top[c]) * t) for c in range(3)) + (255,))
    d.rectangle(((w // 2 - 22) * S, 5 * S, (w // 2 + 22) * S, 8 * S), fill=(255, 190, 70, 255) if lit else (70, 60, 40, 255))
    ic = icon.resize((icon.width * S, icon.height * S), Image.LANCZOS)
    im.alpha_composite(ic, ((w * S - ic.width) // 2, 10 * S))
    f = font(15 * S, "Bold")
    box = d.textbbox((0, 0), label, font=f)
    d.text(((w * S - (box[2] - box[0])) // 2 - box[0], (h - 22) * S - box[1]), label, font=f, fill=(250, 240, 220, 255) if lit else (190, 180, 160, 255))
    return im.resize((w, h), Image.LANCZOS)


def action_cap(w, h, lines, led):
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=7 * S, fill=(10, 9, 8, 255))
    for i in range(2 * S, h * S - 2 * S):
        t = i / (h * S)
        d.line((2 * S, i, w * S - 1 - 2 * S, i), fill=tuple(int(a + (b - a) * t) for a, b in zip((58, 52, 44), (36, 32, 27))) + (255,))
    d.rectangle(((w // 2 - 18) * S, 5 * S, (w // 2 + 18) * S, 8 * S), fill=led + (255,))
    f = font(17 * S, "Bold")
    box = d.textbbox((0, 0), lines, font=f)
    d.text(((w * S - (box[2] - box[0])) // 2 - box[0], (h * S - (box[3] - box[1])) // 2 - box[1] + 3 * S), lines, font=f, fill=(240, 230, 210, 255))
    return im.resize((w, h), Image.LANCZOS)


# ---------------------------------------------------------------------------------------------------------------
# Panels
# ---------------------------------------------------------------------------------------------------------------
def bench():
    rnd = random.Random(7)
    im = Image.new("RGB", (1280, 628), (22, 20, 18))
    px = im.load()
    for y in range(628):
        for x in range(0, 1280, 2):
            v = 20 + int(4 * math.sin(x * 0.013 + y * 0.002)) + rnd.randint(-2, 2)
            px[x, y] = (v + 2, v, v - 2)
            px[x + 1, y] = (v + 2, v, v - 2)
    return im


def draw_pedal(im):
    x, y, w, h = PEDAL
    S = 2
    pd = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(pd)
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=28 * S, fill=(52, 64, 38, 255))     # forest green hammertone
    rnd = random.Random(3)
    for _ in range(9000):                                                                       # the hammertone texture
        cx, cy, r = rnd.randint(0, w * S), rnd.randint(0, h * S), rnd.randint(3, 10)
        v = rnd.randint(-10, 10)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(52 + v, 64 + v, 38 + v // 2, 60))
    mask = Image.new("L", (w * S, h * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=28 * S, fill=255)
    pd.putalpha(mask)
    d = ImageDraw.Draw(pd)
    d.rounded_rectangle((3 * S, 3 * S, w * S - 4 * S, h * S - 4 * S), radius=26 * S, outline=(120, 140, 90, 255), width=2 * S)
    # the title, the footswitch, the status LED (all static)
    tf = font(46 * S, "Bold")
    d.text((40 * S, (h - 78) * S), "KASHYYYK FUZZ", font=tf, fill=(236, 222, 180, 255))
    d.text((44 * S, (h - 28) * S), "SURE EEL  -  MODULAR FUZZ", font=font(15 * S, "SemiBold"), fill=(180, 170, 130, 255))
    fx, fy = w - 120, h - 62
    d.ellipse(((fx - 40) * S, (fy - 40) * S, (fx + 40) * S, (fy + 40) * S), fill=(150, 152, 158, 255), outline=(60, 60, 64, 255), width=3 * S)
    d.ellipse(((fx - 26) * S, (fy - 26) * S, (fx + 26) * S, (fy + 26) * S), fill=(200, 202, 208, 255))
    d.ellipse(((fx - 160) * S, (fy - 12) * S, (fx - 136) * S, (fy + 12) * S), fill=(255, 60, 40, 255), outline=(40, 10, 10, 255), width=2 * S)
    pd = pd.resize((w, h), Image.LANCZOS)
    shadow = Image.new("RGBA", (1280, 628), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((x + 8, y + 10, x + w + 8, y + h + 10), radius=28, fill=(0, 0, 0, 150))
    im.paste(shadow.filter(ImageFilter.GaussianBlur(10)), (0, 0), shadow.filter(ImageFilter.GaussianBlur(10)))
    im.paste(pd, (x, y), pd)
    draw_logo(im, x + 402, y + h - 80, 66)                  # between the title and the LED
    # the circuit board window
    bx, by, bw, bh = BOARD
    board = Image.new("RGBA", (bw, bh), (24, 66, 40, 255))
    bd = ImageDraw.Draw(board)
    rnd = random.Random(11)
    for _ in range(40):                                                                         # copper traces
        x0, y0 = rnd.randint(0, bw), rnd.randint(0, bh)
        x1, y1 = x0 + rnd.choice((-1, 1)) * rnd.randint(30, 160), y0
        bd.line((x0, y0, x1, y1), fill=(180, 140, 60, 120), width=3)
        bd.line((x1, y1, x1, y1 + rnd.choice((-1, 1)) * rnd.randint(20, 80)), fill=(180, 140, 60, 120), width=3)
    for _ in range(60):
        cx, cy = rnd.randint(4, bw - 4), rnd.randint(4, bh - 4)
        bd.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), fill=(200, 170, 90, 200)); bd.ellipse((cx - 1, cy - 1, cx + 1, cy + 1), fill=(20, 40, 25, 255))
    bd.rectangle((0, 0, bw - 1, bh - 1), outline=(10, 25, 15, 255), width=3)
    for label, pname, nxt, sx in SLOTS:                                                         # slot outlines (silkscreen)
        bd.rectangle((sx - bx - 8, 30, sx - bx + SLOT_W + 8, 30 + SLOT_H + 52), outline=(235, 235, 220, 180), width=2)
    im.paste(board, (bx, by), board)
    # the right column plate
    d2 = ImageDraw.Draw(im)
    d2.rounded_rectangle((RX - 14, 18, 1262, 610), radius=14, fill=(30, 28, 25))
    d2.rounded_rectangle((RX - 14, 18, 1262, 610), radius=14, outline=(70, 64, 54), width=2)
    for yy in (324, 504):
        d2.line((RX, yy, 1248, yy), fill=(60, 55, 46), width=2)
    return im


def logo_mask(size):
    """the Sure Eel logo as a mask (its lines = 255), size x size (as in Elver)"""
    im = Image.open(LOGO).convert("L")
    w, h = im.size
    px = im.load()
    x0 = next(x for x in range(w) if px[x, h // 2] < 60)
    y0 = next(y for y in range(h) if px[w // 2, y] < 60)
    x1 = next(x for x in range(w - 1, 0, -1) if px[x, h // 2] < 60)
    y1 = next(y for y in range(h - 1, 0, -1) if px[w // 2, y] < 60)
    return im.crop((x0 + 2, y0 + 2, x1 - 1, y1 - 1)).resize((size, size), Image.LANCZOS)


def draw_logo(im, x, y, size, colour=(236, 222, 180)):
    m = logo_mask(size)
    layer = Image.new("RGBA", (size, size), colour + (255,))
    layer.putalpha(m)
    im.paste(layer, (x, y), layer)


def big_knob_strip():
    """Cream skirted pedal knob with a black pointer, 64 frames over 270 degrees."""
    frames = []
    S = 3
    for k in range(BIGF):
        a = math.radians(-135 + 270 * k / (BIGF - 1))
        im = Image.new("RGBA", (BIGK * S, BIGK * S), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        c = BIGK * S / 2
        d.ellipse((6 * S, 8 * S, BIGK * S - 2 * S, BIGK * S), fill=(0, 0, 0, 110))                             # shadow
        d.ellipse((4 * S, 4 * S, BIGK * S - 6 * S, BIGK * S - 6 * S), fill=(214, 200, 166, 255))              # skirt
        for t in range(0, 360, 12):                                                                               # skirt knurl
            ta = math.radians(t)
            r1, r2 = (BIGK / 2 - 6) * S, (BIGK / 2 - 12) * S
            d.line((c - 1 * S + r1 * math.sin(ta), c - 1 * S - r1 * math.cos(ta), c - 1 * S + r2 * math.sin(ta), c - 1 * S - r2 * math.cos(ta)), fill=(170, 156, 120, 255), width=S)
        d.ellipse((22 * S, 22 * S, BIGK * S - 24 * S, BIGK * S - 24 * S), fill=(238, 228, 200, 255), outline=(150, 136, 100, 255), width=2 * S)
        r = (BIGK / 2 - 8) * S
        d.line((c - S, c - S, c - S + r * math.sin(a), c - S - r * math.cos(a)), fill=(25, 22, 18, 255), width=5 * S)
        frames.append(im.resize((BIGK, BIGK), Image.LANCZOS))
    strip = Image.new("RGBA", (BIGK, BIGK * BIGF), (0, 0, 0, 0))
    for k, f in enumerate(frames):
        strip.paste(f, (0, k * BIGK))
    return strip


def film(images):
    w, h = images[0].size
    s = Image.new("RGBA", (w, h * len(images)), (0, 0, 0, 0))
    for k, im in enumerate(images):
        s.paste(im, (0, k * h))
    return s


# ---------------------------------------------------------------------------------------------------------------
# TUI pieces (as in LUFS Master / Sargasso)
# ---------------------------------------------------------------------------------------------------------------
def text_style(height, colour, fnt="Roboto", style="Regular", just="left verticallyCentred", case="Original"):
    return obj(("version", 1), ("font", obj(("version", 1), ("name", fnt), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def lab(name, bounds, kind, handle, ts, text=None):
    pairs = [("version", 1), ("textStyle", ts), ("type", kind), ("handleName", "Parameter %d" % P["Panel"] if kind == "Text" else handle)]
    if text is not None:
        pairs.append(("text", text))
    return G.entry(name, "Label", Obj(pairs), bounds, "No", "Show", "Always")


def caption(text, bounds, height=16, colour=MUTED, just="left verticallyCentred", style="Medium"):
    return lab("cap " + text + bounds, bounds, "Text", None, text_style(height, colour, "Roboto", style, just), text)


def value(name, bounds, pname, height=20, colour=INK, just="left verticallyCentred", style="Bold"):
    return lab(name, bounds, "Value", "Parameter %d" % P[pname], text_style(height, colour, "Roboto", style, just))


def image(name, file, bounds, frames=None, handle=None):
    if frames:
        data = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", file), ("numFrames", frames), ("handleName", handle))
    else:
        data = obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", file))
    return G.entry(name, "Image", data, bounds, "No", "Show", "Always")


def slot(name, comp, bounds, pname):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, "Yes", "Hide", "Always", remap=[("Data", "Parameter %d" % P[pname])])


def focus(accent, w, h):
    return G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


HITS = set()


def hit_button(name, bounds, pname):
    """An invisible tap area bound to a 'Next' action parameter (acts on every change)."""
    w, h = (int(v) for v in bounds.split()[2:4])
    HITS.add((w, h))
    img = "hit_%dx%d.png" % (w, h)
    data = obj(("version", 2), ("onImage", img), ("offImage", img), ("buttonId", 1), ("numButtonsInGroup", 1),
               ("handleName", "Parameter %d" % P[pname]), ("gestureBehaviour", "Instant"))
    return G.entry(name, "Button", data, bounds, "No", "Show", "Always")


def big_knob_component(accent):
    w, h = BIGK + 40, BIGK + 58
    kids = [focus(accent, w, h),
            G.entry("knob", "Knob", G.knob_filmstrip_data("big_knob.png", BIGF), "20 0 %d %d" % (BIGK, BIGK), "No", "Show", "Always"),
            lab("name", "0 %d %d 24" % (BIGK + 2, w), "Name", "Data", text_style(19, CREAM, "Roboto", "Bold", "horizontallyCentred verticallyCentred", "Upper Case")),
            lab("value", "0 %d %d 24" % (BIGK + 28, w), "Value", "Data", text_style(16, "ffd8cfb0", "Roboto", "Medium", "horizontallyCentred verticallyCentred"))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def knob_component(accent, w=KNOB_W):
    s = KNOB_SIZE
    kids = [focus(accent, w, KNOB_H),
            G.entry("knob", "Knob", G.knob_filmstrip_data("knob_small_uni.png", 127), "8 %d %d %d" % ((KNOB_H - s) // 2, s, s), "No", "Show", "Always"),
            lab("name", "%d 6 %d 32" % (s + 20, w - s - 24), "Name", "Data", text_style(16, MUTED, "Roboto", "Medium", "left verticallyCentred", "Upper Case")),
            lab("value", "%d 40 %d 38" % (s + 20, w - s - 24), "Value", "Data", text_style(22, INK, "Roboto", "Bold", "left verticallyCentred"))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def radio_component(accent, prefix, n, w, h, gap):
    W = n * w + (n - 1) * gap
    kids = [focus(accent, W, h)]
    for i in range(n):
        kids.append(G.entry("btn %d" % i, "Button", G.button_data("%s%d_on.png" % (prefix, i), "%s%d_off.png" % (prefix, i), i, n),
                            "%d 0 %d %d" % (i * (w + gap), w, h), "No", "Show", "Always"))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def action_component(accent, img, w, h):
    kids = [focus(accent, w, h), G.entry("btn", "Button", G.button_data(img, img, 1, 1), "0 0 %d %d" % (w, h), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


# ---------------------------------------------------------------------------------------------------------------
# Tabs
# ---------------------------------------------------------------------------------------------------------------
KNOBS = [("Fuzz", 88), ("Tone", 268), ("Level", 448)]
SMALL = [("Blend", 628, 40)]
CIRCUIT_KNOBS = [("Sag", 84), ("Stages", 264), ("Topology", 444), ("Feedback", 624)]
CK_Y = 222                                  # the band between the big knobs and the board


def pedal_tab():
    kids = [image("panel", "pedal_panel.png", "0 0 1280 628")]
    for pname, x in KNOBS:
        kids.append(slot("k " + pname, "bigKnob", "%d 40 %d %d" % (x, BIGK + 40, BIGK + 58), pname))
    for pname, x, y in SMALL:
        kids.append(slot("s " + pname, "pedKnob", "%d %d 180 %d" % (x, y, KNOB_H), pname))
    for pname, x in CIRCUIT_KNOBS:
        kids.append(slot("c " + pname, "pedKnob", "%d %d 180 %d" % (x, CK_Y, KNOB_H), pname))
    kids.append(caption("PARTS ON THE BOARD - TAP A PART TO SWAP IT", "%d %d 500 20" % (BOARD[0] + 12, BOARD[1] + 6), 14, "ffe8e2c8", style="Bold"))
    for label, pname, nxt, sx in SLOTS:
        sy = BOARD[1] + 34
        # the diode picture follows "Board D" (the part, or empty sockets when the arrangement is None)
        kids.append(image("slot " + pname, "slot_%s.png" % pname.lower().replace(" ", "_"), "%d %d %d %d" % (sx, sy, SLOT_W, SLOT_H),
                          {"Transistors": 5, "Diodes": 6, "Input Cap": 4}[pname], "Parameter %d" % P["Board D" if pname == "Diodes" else pname]))
        kids.append(caption(label, "%d %d 80 20" % (sx, sy + SLOT_H + 2), 14, "ffe8e2c8", style="Bold"))
        kids.append(value("v " + pname, "%d %d %d 22" % (sx + 70, sy + SLOT_H, SLOT_W - 70), pname, 17, CREAM, "right verticallyCentred"))
        kids.append(hit_button("tap " + pname, "%d %d %d %d" % (sx - 8, sy - 4, SLOT_W + 16, SLOT_H + 4), nxt))
    # the arrangement and tone stack read-outs on the board's caption line (both changed on the PARTS page)
    kids += [caption("ARR", "%d %d 40 20" % (566, BOARD[1] + 6), 14, "ffd8cfb0", style="Bold"),
             value("arr", "%d %d 90 20" % (604, BOARD[1] + 6), "Diode Arr", 15, CREAM),
             caption("TONE", "%d %d 50 20" % (694, BOARD[1] + 6), 14, "ffd8cfb0", style="Bold"),
             value("tone stack", "%d %d 90 20" % (740, BOARD[1] + 6), "Tone Stack", 15, CREAM)]
    # right column
    # right column: global, not in presets (Q-Link column 4 in this order: Guitar Vol, Vol Follow, Input, Output)
    kids += [caption("YOUR GUITAR - GLOBAL, NOT IN PRESETS", "%d 24 350 20" % RX, 14),
             slot("gv", "pedKnobR", "%d 42 %d %d" % (RX, KNOB_W - 40, KNOB_H), "Guitar Vol"),
             caption("VOLUME FOLLOW", "%d 128 200 20" % RX, 14),
             slot("vf", "volFollow", "%d 150 %d 52" % (RX, 2 * 110 + 8), "Vol Follow"),
             slot("learn", "actLearn", "%d 150 %d 52" % (RX + 236, 110), "Learn"),
             value("vol now", "%d 208 340 26" % RX, "Vol Now", 19, LED_GREEN_TXT),
             value("status", "%d 236 340 22" % RX, "Status", 15, INK, style="Medium"),
             caption("Auto follows your volume knob from note", "%d 262 340 18" % RX, 13),
             caption("attacks. LEARN: play at full volume 4 s.", "%d 280 340 18" % RX, 13),
             caption("Guitar Vol is the lowest it goes.", "%d 298 340 18" % RX, 13),
             caption("LEVELS - GLOBAL", "%d 332 200 20" % RX, 14),
             slot("in", "pedKnobR", "%d 350 %d %d" % (RX, KNOB_W - 40, KNOB_H), "Input"),
             slot("out", "pedKnobR", "%d 418 %d %d" % (RX, KNOB_W - 40, KNOB_H), "Output"),
             caption("ENGINE", "%d 512 200 20" % RX, 14),
             value("cpu", "%d 532 340 24" % RX, "CPU", 18, LED_AMBER_TXT),
             value("circuit", "%d 558 350 24" % RX, "Circuit", 16, LED_GREEN_TXT, style="Medium"),
             caption("CIRCUIT page: family, Q2, caps, resistors.", "%d 584 350 18" % RX, 13)]
    return G.component_definition([], kids, "ff161412")


PART_ROWS = [("TRANSISTORS - Q1 Q2", "Transistors", "tr", TRANSISTORS),
             ("CLIPPING DIODES", "Diodes", "di", DIODES),
             ("DIODE ARRANGEMENT", "Diode Arr", "ar", ARRS),
             ("INPUT CAPACITOR", "Input Cap", "cp", CAPS),
             ("TONE STACK", "Tone Stack", "ts", TONES)]
BTN_W, BTN_H, BTN_GAP, ROW_Y0, ROW_P = 168, 96, 10, 34, 118


def parts_tab():
    kids = [image("panel", "parts_panel.png", "0 0 1280 628")]
    for r, (cap, pname, key, names) in enumerate(PART_ROWS):
        y = ROW_Y0 + r * ROW_P
        kids.append(caption(cap, "36 %d 220 22" % (y + 30), 16, CREAM, style="Bold"))
        kids.append(value("pv " + pname, "36 %d 220 22" % (y + 56), pname, 15, LED_AMBER_TXT, style="Medium"))
        kids.append(slot("row " + pname, "row_" + key, "%d %d %d %d" % (270, y, len(names) * (BTN_W + BTN_GAP) - BTN_GAP, BTN_H), pname))
    return G.component_definition([], kids, "ff161412")


# the CIRCUIT page: picture-button rows in the PARTS style (6-button rows use narrower buttons)
NB_W = 142
CIRCUIT_ROWS = [  # (caption, parameter, key, names, button width, x, row)
    ("CIRCUIT FAMILY", "Topology", "tp", TOPOS, NB_W, 270, 0),
    ("STAGES", "Stages", "sg", STAGE_N, BTN_W, 270, 1),
    ("Q2 - LATER STAGES", "Q2", "q2", Q2S, NB_W, 270, 2),
    ("COUPLING CAPS", "Coupling", "cc", COUPLINGS, NB_W, 270, 3),
    ("RESISTORS", "Resistors", "rs", RESISTORS, BTN_W, 270, 4),
    ("BUFFER IN FRONT", "Buffer", "bf", BUFFERS, BTN_W, 820, 4)]


def circuit_tab():
    kids = [image("panel", "circuit_panel.png", "0 0 1280 628")]
    for cap, pname, key, names, bw, x, r in CIRCUIT_ROWS:
        y = ROW_Y0 + r * ROW_P
        if x == 270:
            kids.append(caption(cap, "36 %d 230 22" % (y + 30), 16, CREAM, style="Bold"))
            kids.append(value("cv " + pname, "36 %d 230 22" % (y + 56), pname, 15, LED_AMBER_TXT, style="Medium"))
        else:   # a second group on the row: its caption to its left
            kids.append(caption(cap, "%d %d 170 22" % (x - 184, y + 18), 15, CREAM, style="Bold"))
            kids.append(caption("(global)", "%d %d 170 20" % (x - 184, y + 42), 13))
            kids.append(value("cv " + pname, "%d %d 170 22" % (x - 184, y + 64), pname, 15, LED_AMBER_TXT, style="Medium"))
        kids.append(slot("row " + pname, "row_" + key, "%d %d %d %d" % (x, y, len(names) * (bw + BTN_GAP) - BTN_GAP, BTN_H), pname))
    # Dual Mix beside the stage count (it acts on the Dual family: Face path vs Op-Amp path)
    y = ROW_Y0 + 1 * ROW_P
    kids.append(slot("dual mix", "pedKnobR", "%d %d %d %d" % (836, y + 6, KNOB_W - 40, KNOB_H), "Dual Mix"))
    kids.append(caption("Dual: Face path <-> Op-Amp path", "%d %d 300 18" % (842, y + 86), 12))
    return G.component_definition([], kids, "ff161412")


def save_tab():
    kids = [image("panel", "save_panel.png", "0 0 1280 628")]
    kids += [caption("NEW SOUND", "60 40 400 24", 18, CREAM, style="Bold"),
             slot("random", "actRandom", "60 74 200 70", "Random"),
             slot("undo", "actUndo", "280 74 200 70", "Undo"),
             value("patch", "60 158 640 30", "Patch", 20, LED_AMBER_TXT),
             caption("RANDOM picks new parts, Fuzz and Tone in safe ranges.", "60 192 700 22", 14),
             caption("It never changes Input, Output, Guitar Vol or Vol Follow. UNDO goes back one step.", "60 214 760 22", 14),
             caption("SAVE AS A PRESET", "60 270 400 24", 18, CREAM, style="Bold"),
             value("name", "60 304 560 50", "Name", 34, INK, style="Bold"),
             slot("name pos", "pedKnobR", "60 368 %d %d" % (KNOB_W - 40, KNOB_H), "Name Pos"),
             slot("letter", "pedKnobR", "340 368 %d %d" % (KNOB_W - 40, KNOB_H), "Letter"),
             slot("save", "actSave", "660 368 160 70", "Save"),
             slot("cancel", "actCancel", "840 368 160 70", "Cancel"),
             caption("1. Tap SAVE: the name starts as Random 01 or User 01.", "60 470 900 22", 15),
             caption("2. Name Pos picks the letter to change, Letter changes it (Q-Links 1 and 2).", "60 494 900 22", 15),
             caption("3. Tap SAVE again. The preset is listed in the MPC's preset list after the 29 factory presets,", "60 518 900 22", 15),
             caption("   in the next of 16 user slots, and is kept when the MPC restarts.", "60 542 900 22", 15)]
    return G.component_definition([], kids, "ff161412")


def find(o, key):
    for k, v in o:
        if k == key:
            return v
    raise KeyError(key)


def main():
    build = os.path.join(PROJECT, "build")
    params_path = os.path.join(build, "params.json")
    if not os.path.isfile(params_path):
        sys.exit("make_skin: %s not found; run tools/build.py once first" % params_path)
    params = G.load_params(params_path)
    for p in params:
        P[p.name] = p.index
    pages, style, knob_image, frames, kit = G.load_ui(os.path.join(PROJECT, "ui.json"), G.ParamTable(params))
    kit_dir, kit_assets = G.kit_dir_for(kit, build, None)
    tui = G.build_tui(pages, params, style, knob_image, frames, kit_assets)
    qlinks = G.build_qlinks(pages, params)
    accent = "ffe0a040"

    page = find(tui, "pageData")
    defs = find(find(page, "componentDefinitions"), "localComponentDefinitions")
    defs.append(obj(("key", "bigKnob"), ("value", big_knob_component(accent))))
    defs.append(obj(("key", "pedKnob"), ("value", knob_component(accent, 180))))
    defs.append(obj(("key", "pedKnobR"), ("value", knob_component(accent, KNOB_W - 40))))
    defs.append(obj(("key", "volFollow"), ("value", radio_component(accent, "vf", 2, 110, 52, 8))))
    defs.append(obj(("key", "actLearn"), ("value", action_component(accent, "act_learn.png", 110, 52))))
    for k, img, w, h in (("actRandom", "act_random.png", 200, 70), ("actUndo", "act_undo.png", 200, 70),
                         ("actSave", "act_save.png", 160, 70), ("actCancel", "act_cancel.png", 160, 70)):
        defs.append(obj(("key", k), ("value", action_component(accent, img, w, h))))
    for cap, pname, key, names in PART_ROWS:
        defs.append(obj(("key", "row_" + key), ("value", radio_component(accent, key, len(names), BTN_W, BTN_H, BTN_GAP))))
    for cap, pname, key, names, bw, x, r in CIRCUIT_ROWS:
        defs.append(obj(("key", "row_" + key), ("value", radio_component(accent, key, len(names), bw, BTN_H, BTN_GAP))))
    tabs = find(page, "tabs")
    assert [find(x, "tabName") for x in tabs] == ["PEDAL", "PARTS", "CIRCUIT", "SAVE"]
    # one focus rectangle per Q-Link column: the pedal's knobs, the circuit knobs, the board, the global column
    pedal_bounds = ["72 30 752 190", "76 %d 740 %d" % (CK_Y - 4, KNOB_H + 6),
                    "%d %d %d %d" % (BOARD[0] - 6, BOARD[1] - 6, BOARD[2] + 12, BOARD[3] + 34), "%d 20 362 488" % (RX - 10)]
    circuit_bounds = ["262 %d 1000 %d" % (ROW_Y0 - 6, ROW_P + BTN_H + 12), "262 %d 1000 %d" % (ROW_Y0 + 2 * ROW_P - 6, 2 * ROW_P + BTN_H + 12)]
    parts_bounds = ["262 %d 920 %d" % (ROW_Y0 - 6, 4 * ROW_P + BTN_H + 12), "262 %d 920 %d" % (ROW_Y0 + 4 * ROW_P - 6, BTN_H + 12)]
    tabs[0] = obj(("version", 1), ("tabName", "PEDAL"), ("fnKeyIndex", 0), ("fnKeySubIndex", 0), ("componentDefinition", pedal_tab()), ("qlinkBoundsData", pedal_bounds))
    tabs[1] = obj(("version", 1), ("tabName", "PARTS"), ("fnKeyIndex", 1), ("fnKeySubIndex", 0), ("componentDefinition", parts_tab()), ("qlinkBoundsData", parts_bounds))
    tabs[2] = obj(("version", 1), ("tabName", "CIRCUIT"), ("fnKeyIndex", 2), ("fnKeySubIndex", 0), ("componentDefinition", circuit_tab()), ("qlinkBoundsData", circuit_bounds))
    tabs[3] = obj(("version", 1), ("tabName", "SAVE"), ("fnKeyIndex", 3), ("fnKeySubIndex", 0), ("componentDefinition", save_tab()), ("qlinkBoundsData", ["50 360 580 100"]))

    # Q-Links, "Column" banking, one meaning per column (see the top of this file).
    def cols(c):
        m = {}
        for ci, names in enumerate(c):
            for r, n in enumerate(names):
                m[13 + ci - 4 * r] = P[n]
        return m
    pedal_cols = [["Fuzz", "Tone", "Level", "Blend"], ["Sag", "Stages", "Topology", "Feedback"],
                  ["Transistors", "Diodes", "Diode Arr", "Input Cap"], ["Guitar Vol", "Vol Follow", "Input", "Output"]]
    maps = [cols(pedal_cols),
            cols([["Transistors", "Diodes", "Diode Arr", "Input Cap"], ["Tone Stack"]]),
            cols([["Topology", "Stages", "Dual Mix"], ["Q2", "Coupling", "Resistors", "Buffer"]]),
            {13: P["Name Pos"], 9: P["Letter"]}]
    qmap = find(find(qlinks, "Screen Mode Q-Links"), "map")
    for idx, m in enumerate(maps):
        entry = qmap[idx]
        for i, (k, v) in enumerate(entry):
            if k == "Bank Direction":
                entry[i] = (k, "Column")
            if k == "Q-Links":
                entry[i] = (k, Obj(("Q-Link %d" % q, m.get(q, -1)) for q in range(1, 17)))
    # Program Mode: the PEDAL page's layout (the same knob does the same thing in both modes)
    for i, (k, v) in enumerate(qlinks):
        if k == "Program Mode Q-Links":
            qlinks[i] = (k, Obj(("Q-Link %d" % q, maps[0].get(q, -1)) for q in range(1, 17)))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    art = {}
    art["pedal_panel.png"] = draw_pedal(bench()).convert("RGB")
    parts_bg = bench()
    pd = ImageDraw.Draw(parts_bg)
    for r in range(len(PART_ROWS)):
        y = ROW_Y0 + r * ROW_P
        pd.rounded_rectangle((24, y - 8, 1256, y + BTN_H + 8), radius=12, fill=(30, 28, 25), outline=(66, 60, 50), width=2)
    art["parts_panel.png"] = parts_bg
    circuit_bg = bench()
    cd = ImageDraw.Draw(circuit_bg)
    for r in range(5):
        y = ROW_Y0 + r * ROW_P
        cd.rounded_rectangle((24, y - 8, 1256, y + BTN_H + 8), radius=12, fill=(30, 28, 25), outline=(66, 60, 50), width=2)
    cd.line((628, ROW_Y0 + 4 * ROW_P - 2, 628, ROW_Y0 + 4 * ROW_P + BTN_H + 2), fill=(66, 60, 50), width=2)
    art["circuit_panel.png"] = circuit_bg
    fitc = lambda im: im.resize((116, 64), Image.LANCZOS)
    cicon = {"tp": lambda n: pic_topo(n, 120, 64), "sg": lambda n: pic_stages(n, 150, 64), "q2": lambda n: fitc(pic_q2(n)),
             "cc": lambda n: fitc(pic_coupling(n)), "rs": lambda n: fitc(pic_resistor(n)), "bf": lambda n: pic_buffer(n, 150, 64)}
    for cap, pname, key, names, bw, x, r in CIRCUIT_ROWS:
        for i, n in enumerate(names):
            ic = cicon[key](n)
            art["%s%d_on.png" % (key, i)] = selector_button(ic, n, True, bw, BTN_H)
            art["%s%d_off.png" % (key, i)] = selector_button(ic, n, False, bw, BTN_H)
    art["big_knob.png"] = big_knob_strip()
    art["slot_transistors.png"] = film([pic_transistor_pair(n) for n in TRANSISTORS])
    art["slot_diodes.png"] = film([pic_diode_pair(n) for n in DIODES + ["EMPTY"]])
    art["slot_input_cap.png"] = film([pic_cap(n) for n in CAPS])
    # icons: drawn at the board-slot size, then scaled to fit the button (keeps every part's proportions)
    fit = lambda im: im.resize((116, 64), Image.LANCZOS)
    icon = {"tr": lambda n: fit(pic_transistor_pair(n)), "di": lambda n: fit(pic_diode_pair(n)), "ar": lambda n: pic_arr(n, 150, 64),
            "cp": lambda n: fit(pic_cap(n)), "ts": lambda n: pic_tone(n, 150, 64)}
    for cap, pname, key, names in PART_ROWS:
        for i, n in enumerate(names):
            ic = icon[key](n)
            art["%s%d_on.png" % (key, i)] = selector_button(ic, n, True, BTN_W, BTN_H)
            art["%s%d_off.png" % (key, i)] = selector_button(ic, n, False, BTN_W, BTN_H)
    for i, n in enumerate(["OFF", "AUTO"]):
        art["vf%d_on.png" % i] = action_cap(110, 52, n, (120, 255, 150) if i else (255, 190, 70))
        art["vf%d_off.png" % i] = action_cap(110, 52, n, (60, 56, 48))
    art["act_learn.png"] = action_cap(110, 52, "LEARN", (255, 190, 70))
    art["act_random.png"] = action_cap(200, 70, "RANDOM", (120, 255, 150))
    art["act_undo.png"] = action_cap(200, 70, "UNDO", (255, 190, 70))
    art["act_save.png"] = action_cap(160, 70, "SAVE", (255, 120, 80))
    art["act_cancel.png"] = action_cap(160, 70, "CANCEL", (140, 130, 110))
    save_bg = bench()
    sd = ImageDraw.Draw(save_bg)
    for y0, y1 in ((24, 246), (256, 590)):
        sd.rounded_rectangle((24, y0, 1256, y1), radius=12, fill=(30, 28, 25), outline=(66, 60, 50), width=2)
    sd.rounded_rectangle((52, 300, 640, 358), radius=8, fill=(14, 13, 11), outline=(70, 64, 54), width=2)
    draw_logo(save_bg, 1090, 300, 140)
    art["save_panel.png"] = save_bg
    for w, h in HITS:
        art["hit_%dx%d.png" % (w, h)] = Image.new("RGBA", (w, h), (0, 0, 0, 1))
    for f, im in art.items():
        if im.height > 8192 or im.width > 8192:
            sys.exit("make_skin: %s is %dx%d (over 8192 px)" % (f, im.width, im.height))
        im.save(os.path.join(OUT, f), optimize=True)
    for img in sorted(G.referenced_images(tui, set()) - set(art)):
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
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
