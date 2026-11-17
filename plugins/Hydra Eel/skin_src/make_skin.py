"""Builds Hydra Eel's touch skin (plugins/Hydra Eel/skins/) and its ui.json.

Look: the Sure Eel logo recoloured - rust brown field, baby-blue lines and type (skin_src/make_art.py
makes assets/bg_hydra.png from skin_src/reference/). Every page is one pre-drawn background image
with darker rust plates baked in under each section; meters and LEDs are pre-drawn film strips;
numbers and states are value labels. Nothing is drawn at run time.

Five pages, one per section (MAIN, DRIVE, MOD, DELAY, REVERB). Each page is a rack of up to four
sections side by side; each section is one Q-Link bank (column), top to bottom as on screen.

Rules learned on the MPC hardware (from Sleepy Android):
  * nothing may lie on top of a button (a label over a button takes the touch): selector and button
    text is drawn INTO the button images (assets/buttons/, rendered with Pillow);
  * action buttons are plain on/off switches; the plugin acts on every change;
  * static text is bound to a real parameter; no conditional visibility;
  * help: two value labels (Help / Help 2) set by invisible buttons over names and meters.

usage (SDK root, after one native build so build/params.json exists):
    python "plugins/Hydra Eel/skin_src/make_skin.py"
    python tools/build.py "plugins/Hydra Eel"
Button, knob and toggle art is rendered with Pillow when it is installed, else the committed images
in skin_src/assets/ are used.
"""
import json
import math
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
ART = os.path.join(HERE, "assets")
BTN_DIR = os.path.join(ART, "buttons")
FONT_FILE = r"C:\Windows\Fonts\bahnschrift.ttf"
# colours: baby blue type and lines on rust brown
BLUE, BLUE_DIM, RUST, RUST_DARK = (0x89, 0xCF, 0xF0), (0x4A, 0x80, 0x98), (0x6E, 0x32, 0x14), (0x30, 0x14, 0x07)
TEXT, MUTED = "ff89cff0", "ff6aa6c4"
STATIC_HANDLE = "Parameter 0"
HEAD_FONT = "Nexa"
SEP = "\u00b7"
MORE = "^"                              # this row is taken by the control above (a tall selector)

PAGES = [
    ("MAIN", [
        ("levels", ["Input", "Output", "Bypass", "Stereo"]),
        ("patch", ["Randomize", "Undo", "Rand Amount", ("status", "Status", "status")]),
        ("save preset", ["Save", "Confirm", "Cancel", ("status", "Preset Name", "preset name " + SEP + " [cursor]")]),
        ("name", ["Name Pos", "Letter", None, None]),
    ]),
    ("DRIVE", [
        ("fuzz " + SEP + " comp", ["Drive On", "Drive Type", MORE, MORE]),
        ("controls", ["Amount", "Tone", "Level", "Gate Bias"]),
        ("blend", ["Drive Mix", None, None, None]),
    ]),
    ("MOD", [
        ("modulation", ["Mod On", "Mod Mode", "Blend", "Wave"]),
        ("speed", ["Rate", "Depth", "Rotor", None]),
        ("rate mode", ["Rate Mode", ("status", "Rotor Now", "speed now"), None, None]),
    ]),
    ("DELAY", [
        ("delay", ["Delay On", "Delay Type", "Sync", ("status", "Delay Now", "delay time now")]),
        ("time " + SEP + " repeats", ["Time", "Feedback", "Delay Mix", None]),
    ]),
    ("REVERB", [
        ("reverb", ["Reverb On", "Reverb Type", None, None]),
        ("sound", ["Decay", "Rev Tone", "Reverb Mix", None]),
    ]),
]
# bottom strip meters per page: (caption, parameter, help id)
METERS = {
    "MAIN":   [("input level " + SEP + " dBFS", "In Level", 28), ("limiter gain reduction " + SEP + " dB", "Limiter GR", 28)],
    "DRIVE":  [("input level " + SEP + " dBFS", "In Level", 28), ("compressor gain reduction " + SEP + " dB", "Comp GR", 31)],
    "MOD":    [("input level " + SEP + " dBFS", "In Level", 28), ("output level " + SEP + " dBFS", "Out Level", 28)],
    "DELAY":  [("output level " + SEP + " dBFS", "Out Level", 28), ("limiter gain reduction " + SEP + " dB", "Limiter GR", 28)],
    "REVERB": [("output level " + SEP + " dBFS", "Out Level", 28), ("limiter gain reduction " + SEP + " dB", "Limiter GR", 28)],
}
TOGGLES = {"Bypass", "Drive On", "Mod On", "Delay On", "Reverb On"}
SELECTORS = {
    "Stereo": ["mono", "stereo"], "Mod Mode": ["trem " + SEP + " vib", "rotary " + SEP + " phaser"],
    "Wave": ["sine", "triangle", "square"], "Rotor": ["slow", "fast"], "Delay Type": ["digital", "analog"],
    "Sync": ["ms", "tempo"], "Reverb Type": ["plate", "spring", "shimmer"], "Rate Mode": ["hz", "sync"],
}
# the drive selector: two columns, cleanest to dirtiest - compressors left, fuzzes right
BIG = {"Drive Type": ["opto", "smooth", "fet", "punch", "germanium", "gated", "sustain", "octave"]}
ACTIONS = {"Randomize": "randomize", "Undo": "undo", "Save": "save preset", "Confirm": "confirm name", "Cancel": "cancel"}
HELP_FOR = {
    "Input": 1, "Output": 2, "Bypass": 3, "Stereo": 4, "Drive On": 5, "Drive Type": 5, "Amount": 6, "Tone": 7,
    "Level": 8, "Gate Bias": 9, "Mod On": 10, "Mod Mode": 10, "Blend": 11, "Rate": 12, "Depth": 13, "Wave": 14,
    "Rotor": 15, "Delay On": 16, "Delay Type": 16, "Sync": 17, "Time": 17, "Division": 17, "Feedback": 18,
    "Delay Mix": 19, "Reverb On": 20, "Reverb Type": 20, "Decay": 21, "Rev Tone": 22, "Reverb Mix": 23,
    "Randomize": 24, "Undo": 25, "Rand Amount": 26, "Save": 27, "Confirm": 27, "Cancel": 27, "Name Pos": 27,
    "Letter": 27, "Preset Name": 27, "Status": 30, "Rotor Now": 29, "Delay Now": 29, "Drive Mix": 32, "Rate Mode": 12,
}
HELP_N = 33
PROGRAM_QLINKS = ["Amount", "Tone", "Level", "Gate Bias", "Blend", "Rate", "Depth", "Rotor",
                  "Time", "Feedback", "Delay Mix", "Decay", "Reverb Mix", "Randomize", "Save", "Output"]

SEC_X0, SEC_PITCH, SEC_W, SEC_Y, SEC_H = 18, 312, 300, 6, 420
ROW_Y0, ROW_PITCH, CTL_W, CTL_H = 52, 92, 288, 88
STRIP_Y, STRIP_H, HELP_X = 436, 172, 654
LEDS, LED_PITCH, LED_W, LED_H = 24, 11, 8, 18
PLATE = 0.42
SEG_GAP, SEG_Y, SEG_H = 6, 36, 42
SEG_W = {2: 132, 3: 86}
BIG_W, BIG_H, BIG_GAP_X, BIG_GAP_Y, BIG_Y = 132, 50, 6, 6, 40
ACT_W, ACT_H = 200, 46


def slug(s):
    return "".join(ch if ch.isalnum() else "_" for ch in s.lower()).strip("_")


# ------------------------------------------------------------------------------------------
# Raster helpers (pure Python)
# ------------------------------------------------------------------------------------------
class Canvas:
    def __init__(self, w, h, rgb=(0, 0, 0), alpha=False):
        self.w, self.h, self.n = w, h, 4 if alpha else 3
        px = bytes(rgb) + (b"\xff" if alpha else b"")
        self.rows = [bytearray(px * w) for _ in range(h)]

    def fill(self, x, y, w, h, rgb, a=255):
        x0, x1 = max(0, x), min(self.w, x + w)
        if x1 <= x0:
            return
        px = bytes(rgb) + (bytes([a]) if self.n == 4 else b"")
        seg = px * (x1 - x0)
        for yy in range(max(0, y), min(self.h, y + h)):
            self.rows[yy][x0 * self.n:x1 * self.n] = seg

    def shade(self, x, y, w, h, k):
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.rows[yy]
            inset = 3 if yy - y < 3 or y + h - 1 - yy < 3 else 0
            for i in range((max(0, x) + inset) * self.n, (min(self.w, x + w) - inset) * self.n):
                row[i] = int(row[i] * k)

    def put(self, x, y, rgb):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.rows[y][x * self.n:x * self.n + 3] = bytes(rgb)

    def ring(self, cx, cy, r, rgb, thick=1.0):
        ri = int(r + thick + 1)
        for yy in range(-ri, ri + 1):
            for xx in range(-ri, ri + 1):
                if abs(math.hypot(xx, yy) - r) <= thick / 2:
                    self.put(cx + xx, cy + yy, rgb)

    def frame(self, x, y, w, h, rgb, rad=10):
        self.fill(x + rad, y, w - 2 * rad, 1, rgb)
        self.fill(x + rad, y + h - 1, w - 2 * rad, 1, rgb)
        self.fill(x, y + rad, 1, h - 2 * rad, rgb)
        self.fill(x + w - 1, y + rad, 1, h - 2 * rad, rgb)
        for a in range(0, 91, 2):
            t = math.radians(a)
            dx, dy = int(round(rad - rad * math.cos(t))), int(round(rad - rad * math.sin(t)))
            for px, py in ((x + dx, y + dy), (x + w - 1 - dx, y + dy), (x + dx, y + h - 1 - dy), (x + w - 1 - dx, y + h - 1 - dy)):
                self.put(px, py, rgb)

    def save(self, path):
        def chunk(tag, data):
            body = tag + data
            return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        raw = b"".join(b"\0" + bytes(r) for r in self.rows)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6 if self.n == 4 else 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


def load_png_rgb(path):
    d = open(path, "rb").read()
    pos, idat, w, h, ct = 8, b"", 0, 0, 2
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        tag, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + ln
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    c = Canvas(w, h)
    c.rows = [bytearray(b"".join(bytes(r[x * bpp:x * bpp + 3]) for x in range(w))) for r in rows]
    return c


# ------------------------------------------------------------------------------------------
# Artwork
# ------------------------------------------------------------------------------------------
_BG = None


def page_panel(sections):
    global _BG
    if _BG is None:
        _BG = load_png_rgb(os.path.join(ART, "bg_hydra.png"))
    c = Canvas(_BG.w, _BG.h)
    c.rows = [bytearray(r) for r in _BG.rows]
    for s in range(len(sections)):
        x = SEC_X0 + s * SEC_PITCH
        c.shade(x, SEC_Y, SEC_W, SEC_H, PLATE)
        c.frame(x, SEC_Y, SEC_W, SEC_H, BLUE, 12)
        c.fill(x + 14, SEC_Y + 40, SEC_W - 28, 1, BLUE_DIM)
    for x, w, col in ((18, 300, BLUE_DIM), (336, 300, BLUE_DIM), (HELP_X, 1262 - HELP_X, BLUE)):
        c.shade(x, STRIP_Y, w, STRIP_H, PLATE)
        c.frame(x, STRIP_Y, w, STRIP_H, col, 8)
    return c


def led_bar():
    w, rows = LEDS * LED_PITCH, []
    for lit in range(LEDS + 1):
        for y in range(LED_H):
            row = bytearray(bytes(RUST_DARK) * w)
            if 2 <= y < LED_H - 2:
                for i in range(LEDS):
                    col = BLUE if i < lit else (0x4A, 0x24, 0x10)
                    x0 = i * LED_PITCH + 1
                    row[x0 * 3:(x0 + LED_W) * 3] = bytes(col) * LED_W
            rows.append(row)
    c = Canvas(w, LED_H * (LEDS + 1))
    c.rows = rows
    return c


def hit_image(path, w, h):
    c = Canvas(w, h, (0, 0, 0), alpha=True)
    c.fill(0, 0, w, h, (0, 0, 0), 1)
    c.save(path)


def art_files():
    """{file: spec} for every rendered image: labelled buttons and the recoloured knobs / toggles."""
    want = {}
    for name, caps in SELECTORS.items():
        for i, cap in enumerate(caps):
            for lit in (True, False):
                want["sel_%s_%d_%s.png" % (slug(name), i, "on" if lit else "off")] = ("btn", SEG_W[len(caps)], SEG_H, cap, lit)
    for name, caps in BIG.items():
        for i, cap in enumerate(caps):
            for lit in (True, False):
                want["sel_%s_%d_%s.png" % (slug(name), i, "on" if lit else "off")] = ("btn", BIG_W, BIG_H, cap, lit)
    for name, cap in ACTIONS.items():
        want["act_%s.png" % slug(name)] = ("btn", ACT_W, ACT_H, cap, False)
    for kit in ("knob_small_uni.png", "knob_small_bi.png", "toggle_on.png", "toggle_off.png"):
        want["hydra_" + kit] = ("tint", kit)
    return want


def render_art(kit_src):
    want = art_files()
    try:
        from PIL import Image, ImageDraw, ImageFont
        if not os.path.isfile(FONT_FILE):
            raise ImportError(FONT_FILE)
    except ImportError as e:
        missing = [f for f in want if not os.path.isfile(os.path.join(BTN_DIR, f))]
        if missing:
            sys.exit("make_skin: art missing (%s) and cannot render it (%s): run once with Pillow" % (", ".join(missing[:3]), e))
        return want
    os.makedirs(BTN_DIR, exist_ok=True)
    for f, spec in want.items():
        if spec[0] == "tint":
            # the mono kit's white arcs and grey bodies -> baby blue arcs on rust bodies
            im = Image.open(os.path.join(kit_src, spec[1])).convert("RGBA")
            r, g, b, a = im.split()
            lum = Image.merge("RGB", (r, g, b)).convert("L")
            lut = []
            for c in range(3):
                lut += [int(RUST_DARK[c] + (BLUE[c] - RUST_DARK[c]) * v / 255.0 + 0.5) for v in range(256)]
            out = Image.merge("RGB", (lum, lum, lum)).point(lut).convert("RGBA")
            out.putalpha(a)
            out.save(os.path.join(BTN_DIR, f), optimize=True)
            continue
        _, w, h, text, lit = spec
        S = 3
        im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, fill=BLUE + (255,) if lit else RUST_DARK + (255,),
                            outline=BLUE + (255,) if lit else BLUE_DIM + (255,), width=2 * S)
        size = 21
        while True:
            font = ImageFont.truetype(FONT_FILE, size * S)
            try:
                font.set_variation_by_name("Regular" if lit else "Light")
            except Exception:
                pass
            box = d.textbbox((0, 0), text, font=font)
            if box[2] - box[0] <= (w - 10) * S or size <= 11:
                break
            size -= 1
        tw, th = box[2] - box[0], box[3] - box[1]
        d.text(((w * S - tw) / 2 - box[0], (h * S - th) / 2 - box[1]), text, font=font, fill=RUST + (255,) if lit else BLUE + (255,))
        im.resize((w, h), Image.LANCZOS).save(os.path.join(BTN_DIR, f), optimize=True)
    return want


# ------------------------------------------------------------------------------------------
# TUI pieces
# ------------------------------------------------------------------------------------------
def ts(height, colour, font="Roboto", style="Regular", just="left verticallyCentred", case="Original"):
    return obj(("version", 1),
               ("font", obj(("version", 1), ("name", font), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def label(name, bounds, kind, handle, style, text=None):
    pairs = [("version", 1), ("textStyle", style), ("type", kind), ("handleName", STATIC_HANDLE if kind == "Text" else handle)]
    if text is not None:
        pairs.append(("text", text))
    return G.entry(name, "Label", Obj(pairs), bounds, "No", "Show", "Always")


def caption(text, bounds, size=15, colour=MUTED):
    return label("cap " + text, bounds, "Text", None, ts(size, colour, HEAD_FONT, "Light", "left verticallyCentred", "Original"), text)   # case as written (keeps dBFS, Hz)


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
    return G.entry("help %d" % item, "Button", data, "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always")


def focus(w, h):
    return G.entry("focus", "Focus", G.focus_data("ff89cff0"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def name_style():
    return ts(17, MUTED, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case")


def knob_def(image_file):
    kids = [focus(CTL_W, CTL_H),
            G.entry("knob", "Knob", G.knob_filmstrip_data(image_file, 127), "8 16 56 56", "No", "Show", "Always"),
            label("name", "78 8 204 30", "Name", "Data", name_style()),
            label("value", "78 42 204 36", "Value", "Data", ts(24, TEXT))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def selector_def(name, caps, w, h, cols, height):
    n = len(caps)
    kids = [focus(CTL_W, height),
            label("name", "12 4 140 28", "Name", "Data", name_style()),
            label("value", "140 4 136 28", "Value", "Data", ts(18, TEXT, "Roboto", "Regular", "right verticallyCentred"))]
    rows = (n + cols - 1) // cols
    for i in range(n):
        if cols == 1 or name not in BIG:
            x, y = 12 + i * (w + SEG_GAP), SEG_Y
        else:   # column-major: compressors down the left, fuzzes down the right
            c, r = divmod(i, rows)
            x, y = 12 + c * (w + BIG_GAP_X), BIG_Y + r * (h + BIG_GAP_Y)
        kids.append(G.entry("seg %d" % i, "Button", G.button_data("sel_%s_%d_on.png" % (slug(name), i), "sel_%s_%d_off.png" % (slug(name), i), i, n),
                            "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always"))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def toggle_def():
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 8 190 30", "Name", "Data", name_style()),
            label("value", "12 42 190 36", "Value", "Data", ts(24, TEXT)),
            G.entry("toggle", "Button", G.button_data("hydra_toggle_on.png", "hydra_toggle_off.png", 1, 1), "210 28 64 32", "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def action_def(name):
    img = "act_%s.png" % slug(name)
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 4 270 26", "Name", "Data", name_style()),
            G.entry("button", "Button", G.button_data(img, img, 1, 1), "12 34 %d %d" % (ACT_W, ACT_H), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def slot(name, comp, bounds, p):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, "Yes", "Hide", "Always", remap=[("Data", p.handle)])


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
    kids = [image("panel", "panel_%s.png" % page_name.lower(), "0 0 1280 628")]
    trig = []
    for s, (header, rows) in enumerate(sections):
        x0 = SEC_X0 + s * SEC_PITCH
        kids.append(label("section %d" % s, "%d %d %d 34" % (x0 + 16, SEC_Y + 4, SEC_W - 32), "Text", None,
                          ts(22, TEXT, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case"), header))
        for r, row in enumerate(rows):
            if row is None or row == MORE:
                continue
            x, y = x0 + 6, ROW_Y0 + r * ROW_PITCH
            if isinstance(row, tuple):
                _, pname, cap = row
                kids.append(caption(cap, "%d %d 270 26" % (x + 12, y + 4)))
                kids.append(label("%s value" % pname, "%d %d 276 50" % (x + 12, y + 32), "Value", by_name[pname].handle, ts(21, TEXT)))
                trig.append((HELP_FOR[pname], x + 12, y + 2, 270, 30))
                continue
            p = by_name[row]
            h = CTL_H
            if row in TOGGLES:
                comp, hx = "toggleC", (x + 12, y + 6, 180, 32)
            elif row in BIG:
                h = 3 * ROW_PITCH - 4                      # a tall selector: rows r .. r+2
                comp, hx = "sel_" + slug(row), (x + 12, y + 2, 126, 30)
            elif row in SELECTORS:
                comp, hx = "sel_" + slug(row), (x + 12, y + 2, 126, 30)
            elif row in ACTIONS:
                comp, hx = "act_" + slug(row), (x + 12, y + 2, 270, 28)
            else:
                comp, hx = "knobC", (x + 78, y + 6, 200, 32)
            kids.append(slot(row, comp, "%d %d %d %d" % (x, y, CTL_W, h), p))
            trig.append((HELP_FOR[row],) + hx)
    for j, (cap, pname, hid) in enumerate(METERS[page_name]):
        x = 18 + j * 318
        kids.append(caption(cap, "%d %d 270 26" % (x + 14, STRIP_Y + 8)))
        kids.append(image("meter %d" % j, "led_bar.png", "%d %d %d %d" % (x + 14, STRIP_Y + 48, LEDS * LED_PITCH, LED_H), LEDS + 1, by_name[pname].handle))
        kids.append(label("meter %d value" % j, "%d %d 270 40" % (x + 14, STRIP_Y + 80), "Value", by_name[pname].handle, ts(26, TEXT)))
        trig.append((hid, x, STRIP_Y, 300, STRIP_H))
    hw = 1262 - HELP_X
    kids.append(caption("help " + SEP + " touch a name or a meter", "%d %d %d 26" % (HELP_X + 16, STRIP_Y + 8, hw - 32)))
    kids.append(label("help 1", "%d %d %d 36" % (HELP_X + 16, STRIP_Y + 48, hw - 32), "Value", help1.handle, ts(22, TEXT)))
    kids.append(label("help 2", "%d %d %d 36" % (HELP_X + 16, STRIP_Y + 90, hw - 32), "Value", help2.handle, ts(22, TEXT)))
    trig.append((0, HELP_X, STRIP_Y, hw, STRIP_H))
    for t in trig:
        kids.append(help_trigger(*t, help1))
    return G.component_definition([], kids, "ff6e3214")


def write_ui_json():
    pages = []
    for name, sections in PAGES:
        controls = []
        for r in range(4):
            for c in range(4):
                row = sections[c][1][r] if c < len(sections) else None
                if row is None or row == MORE or isinstance(row, tuple):
                    controls.append("empty")
                elif row in TOGGLES or row in SELECTORS or row in ACTIONS:      # (BIG: listed as a knob; the page is ours)
                    controls.append({"param": row, "widget": "switch"})
                else:
                    controls.append({"param": row, "widget": "knob"})
        while controls and controls[-1] == "empty":
            controls.pop()
        pages.append({"name": name, "controls": controls})
    ui = {"kit": "mono", "pages": pages,
          "style": {"background": "ff6e3214", "text": "ff89cff0", "muted": "ff6aa6c4", "accent": "ff89cff0"}}
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
    art = render_art(kit_dir)

    page_data = find(tui, "pageData")
    defs = find(find(page_data, "componentDefinitions"), "localComponentDefinitions")
    defs.append(obj(("key", "knobC"), ("value", knob_def("hydra_knob_small_uni.png"))))
    defs.append(obj(("key", "toggleC"), ("value", toggle_def())))
    for name, caps in SELECTORS.items():
        defs.append(obj(("key", "sel_" + slug(name)), ("value", selector_def(name, caps, SEG_W[len(caps)], SEG_H, len(caps), CTL_H))))
    for name, caps in BIG.items():
        defs.append(obj(("key", "sel_" + slug(name)), ("value", selector_def(name, caps, BIG_W, BIG_H, 2, 3 * ROW_PITCH - 4))))
    for name in ACTIONS:
        defs.append(obj(("key", "act_" + slug(name)), ("value", action_def(name))))

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
    own = {"panel_%s.png" % n.lower() for n, _ in PAGES} | {"led_bar.png"} | set(art) | {"hit_%dx%d.png" % s for s in HIT_SIZES}
    for img in sorted(G.referenced_images(tui, set()) - own):
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
    for f in art:
        shutil.copy2(os.path.join(BTN_DIR, f), os.path.join(OUT, f))
    for name, sections in PAGES:
        page_panel(sections).save(os.path.join(OUT, "panel_%s.png" % name.lower()))
    led_bar().save(os.path.join(OUT, "led_bar.png"))
    for w, h in sorted(HIT_SIZES):
        hit_image(os.path.join(OUT, "hit_%dx%d.png" % (w, h)), w, h)
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
