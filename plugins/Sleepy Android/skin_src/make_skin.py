"""Builds Sleepy Android's touch skin (plugins/Sleepy Android/skins/) and its ui.json.

Style guide: the sureeel logo (skin_src/reference/) - black and white op-art, white lines on
black, thin geometric lowercase type. Everything is monochrome and static:
  * each page's background is one pre-drawn image: the logo, whole and dimmed, with dark plates
    baked in under each section so labels stay readable;
  * meters and LEDs are pre-drawn film strips; numbers and states are value labels. Nothing is
    drawn at run time; every element redraws only when its value changes.

Every page is a hand-laid rack: up to four sections side by side, each one Q-Link column (bank),
four rows of controls each, and a bottom strip with two meters and the help window.

Rules learned on the hardware (why it is built this way):
  * Nothing may lie on top of a button. A label over a button takes the touch, so the button never
    changes (that broke every selector in an earlier version). Selector and button text is
    therefore drawn INTO the button images (skin_src/assets/buttons/, made with Pillow), and every
    other label sits beside its control, never over it.
  * The MPC's spring-back option (ResetOnGestureEnd) leaves a button dead on the hardware, so action
    buttons are plain on/off switches; the plugin acts on every change (see SleepyAndroid.cpp).
  * Static text must be bound to a real parameter (bound to nothing, the MPC does not draw it).
  * No conditional visibility. The help window shows two value labels (Help / Help 2) whose text
    the plugin supplies, set by invisible buttons over the controls' name labels and the meters.

usage (SDK root, after one native build so build/params.json exists):
    python "plugins/Sleepy Android/skin_src/make_skin.py"
    python tools/build.py "plugins/Sleepy Android"
Button images are rendered with Pillow when it is installed and the font is present, else the
committed images in skin_src/assets/buttons/ are used.
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
BTN_DIR = os.path.join(HERE, "assets", "buttons")
FONT_FILE = r"C:\Windows\Fonts\bahnschrift.ttf"      # thin geometric type, like the logo's
WHITE, MUTED = "ffffffff", "ffa8a8a8"
STATIC_HANDLE = "Parameter 0"          # a real parameter, so static text is drawn
HEAD_FONT = "Nexa"
SEP = "\u00b7"                         # middle dot

# ------------------------------------------------------------------------------------------
# Pages: up to four sections (header, four rows). A row is a parameter name, None (empty), or
# ("status", parameter, caption) for a read-out line.
# ------------------------------------------------------------------------------------------
PAGES = [
    ("MAIN", [
        ("levels", ["Input Gain", "Output", "Mix", "Bypass"]),
        ("routing " + SEP + " pedal", ["Routing", "Pedal", ("status", "Pedal Status", "pedal status"),
                                       ("status", "Preset Name", "preset name " + SEP + " [cursor]")]),
        ("pedal assign", ["Assign", "Set Heel", "Set Toe", "Clear"]),
        ("save preset", ["Save", "Name Pos", "Letter", "Confirm"]),
    ]),
    ("FILTER", [
        ("envelope follower", ["Sensitivity", "Attack", "Release", "Env Mode"]),
        ("lfo", ["LFO Rate", "LFO Depth", "LFO Shape", "LFO To"]),
        ("filter", ["Cutoff", "Sweep", "Resonance", "Filter Type"]),
        ("output", ["VCA", "Filter Mix", "Stereo", "Filter On"]),
    ]),
    ("SWEEP", [
        ("auto sweep", ["Auto Sweep", "Sweep Dir", "Sweep Range", ("status", "Sweep Now", "sweep speed now")]),
        ("sweep speed", ["Sweep Rate", "Sweep Rate Max", None, None]),
        ("touch " + SEP + " filter", ["Env Touch", "Env Touch Amt", "Sweep Touch", "Sweep Touch Amt"]),
    ]),
    ("STUTTER", [
        ("stutter", ["Stutter On", "Trigger", "Hold", "Dry Mute"]),
        ("mode", ["Stut Mode", "Sync", ("status", "Stutter Now", "stutter now"), None]),
        ("lengths", ["Slice", "Loop", "Feedback", "Stutter Mix"]),
    ]),
    ("RANDOM", [
        ("random range", ["Slice Min", "Slice Max", "Loop Min", "Loop Max"]),
        ("re-roll", ["Rand Width", "Rand Rate", "Rand Rate Max", ("status", "Stutter Now", "stutter now")]),
        ("touch " + SEP + " stutter", ["Stut Touch", "Stut Touch Amt", None, None]),
    ]),
    ("PATCH", [
        ("randomize patch", ["Randomize", "Undo", "Rand Amount", ("status", "Rand Status", "status")]),
        ("locks", ["Lock Filter", "Lock Stutter", None, None]),
        ("save preset", ["Save", "Confirm", "Cancel", ("status", "Preset Name", "preset name " + SEP + " [cursor]")]),
        ("name", ["Name Pos", "Letter", None, None]),
    ]),
]
# bottom strip meters: (caption, parameter, help id)
METERS = {
    "FILTER": [("envelope " + SEP + " %", "Env Level", 35), ("filter position " + SEP + " Hz", "Filter Pos", 15)],
    "SWEEP":  [("touch level " + SEP + " soft..hard", "Touch Level", 44), ("filter position " + SEP + " Hz", "Filter Pos", 15)],
    "RANDOM": [("touch level " + SEP + " soft..hard", "Touch Level", 44), ("input level " + SEP + " dBFS", "Input Level", 15)],
    None:     [("input level " + SEP + " dBFS", "Input Level", 15), ("limiter gain reduction " + SEP + " dB", "Limiter GR", 23)],
}

BIPOLAR = {"Sweep"}
TOGGLES = {"VCA", "Filter On", "Bypass", "Stutter On", "Hold", "Dry Mute", "Assign", "Lock Filter", "Lock Stutter",
           "Auto Sweep", "Sweep Touch", "Env Touch", "Stut Touch"}
SELECTORS = {   # caption per choice, drawn into the button images
    "Env Mode": ["env", "gate"], "LFO Shape": ["tri", "square", "ramp up", "ramp dn"],
    "LFO To": ["filter", "volume", "both"], "Filter Type": ["lp", "bp", "hp"], "Stereo": ["mono", "stereo"],
    "Routing": ["filt>stut", "stut>filt", "parallel"], "Stut Mode": ["fixed", "random"], "Sync": ["ms", "tempo"],
    "Sweep Dir": ["up", "down", "up-down"],
}
ACTIONS = {"Set Heel": "set heel", "Set Toe": "set toe", "Clear": "clear", "Save": "save preset",
           "Randomize": "randomize", "Undo": "undo", "Confirm": "confirm name", "Cancel": "cancel"}
TRIGGER = "Trigger"

HELP_FOR = {
    "Input Gain": 1, "Output": 2, "Mix": 3, "Routing": 4, "Bypass": 5, "Pedal": 6,
    "Assign": 7, "Set Heel": 7, "Set Toe": 7, "Pedal Status": 7, "Clear": 8, "Save": 37,
    "Sensitivity": 9, "Sweep": 10, "Resonance": 11, "Filter Type": 12, "Attack": 13, "Release": 13,
    "Filter On": 14, "Filter Mix": 14, "Cutoff": 29, "Env Mode": 30,
    "LFO Rate": 31, "LFO Depth": 31, "LFO Shape": 31, "VCA": 32, "LFO To": 33, "Stereo": 34,
    "Hold": 16, "Trigger": 16, "Stutter On": 16, "Stut Mode": 17, "Sync": 18, "Rand Width": 36,
    "Slice": 19, "Loop": 19, "Feedback": 20, "Dry Mute": 21, "Stutter Mix": 24, "Stutter Now": 23,
    "Slice Min": 22, "Slice Max": 22, "Loop Min": 22, "Loop Max": 22,
    "Randomize": 25, "Rand Status": 25, "Undo": 26, "Rand Amount": 27, "Lock Filter": 28, "Lock Stutter": 28,
    "Auto Sweep": 39, "Sweep Dir": 39, "Sweep Range": 39, "Sweep Now": 40, "Sweep Rate": 40, "Sweep Rate Max": 40,
    "Sweep Touch": 41, "Sweep Touch Amt": 41, "Stut Touch": 41, "Stut Touch Amt": 41, "Env Touch": 42, "Env Touch Amt": 42,
    "Rand Rate": 43, "Rand Rate Max": 43, "Touch Level": 44,
    "Name Pos": 45, "Letter": 45, "Confirm": 45, "Cancel": 45, "Preset Name": 45,
}
HELP_N = 46
HELP_ENV_LED, HELP_STUT_LED = 35, 38

PROGRAM_QLINKS = ["Pedal", "Randomize", "Undo", "Save", "Rand Amount", "Sensitivity", "Sweep", "Resonance",
                  "Cutoff", "Feedback", "Slice", "Trigger", "Env Touch Amt", "Sweep Touch Amt", "Stut Touch Amt", "Sweep Rate"]

# geometry
SEC_X0, SEC_PITCH, SEC_W, SEC_Y, SEC_H = 18, 312, 300, 6, 420
ROW_Y0, ROW_PITCH, CTL_W, CTL_H = 52, 92, 288, 88
STRIP_Y, STRIP_H, HELP_X = 436, 172, 654
LEDS, LED_PITCH, LED_W, LED_H = 24, 11, 8, 18
PLATE = 0.28                            # plates keep 28 % of the background's brightness
SEG_GAP, SEG_Y, SEG_H = 6, 36, 42
SEG_W = {2: 132, 3: 86, 4: 63}          # segment widths in a 270 px row
ACT_W, ACT_H = 200, 46
TRIG_W = 150


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
        """Darkens a rectangle to k of its brightness (with slightly rounded corners)."""
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.rows[yy]
            inset = 3 if yy - y < 3 or y + h - 1 - yy < 3 else 0
            for i in range((max(0, x) + inset) * self.n, (min(self.w, x + w) - inset) * self.n):
                if self.n == 3 or i % 4 != 3:
                    row[i] = int(row[i] * k)

    def put(self, x, y, rgb, a=255):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.rows[y][x * self.n:x * self.n + 3] = bytes(rgb)
            if self.n == 4:
                self.rows[y][x * self.n + 3] = a

    def ring(self, cx, cy, r, rgb, thick=1.0):
        ri = int(r + thick + 1)
        for yy in range(-ri, ri + 1):
            for xx in range(-ri, ri + 1):
                if abs(math.hypot(xx, yy) - r) <= thick / 2:
                    self.put(cx + xx, cy + yy, rgb)

    def frame(self, x, y, w, h, rgb, rad=10):
        """Rounded hairline rectangle."""
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
    """Reads an 8-bit RGB / RGBA PNG (any filter type) as RGB."""
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


def base_panel():
    """The sureeel logo, whole and dimmed, as the page background (skin_src/assets/bg_logo.png,
    made once from skin_src/reference/). A plain static image: drawn once, never animated."""
    global _BG
    if _BG is None:
        _BG = load_png_rgb(os.path.join(HERE, "assets", "bg_logo.png"))
    c = Canvas(_BG.w, _BG.h)
    c.rows = [bytearray(r) for r in _BG.rows]
    return c


def plate(c, x, y, w, h, frame_rgb=(70, 70, 70), rad=8):
    c.shade(x, y, w, h, PLATE)
    c.frame(x, y, w, h, frame_rgb, rad)


def page_panel(n_sections):
    c = base_panel()
    for s in range(n_sections):
        x = SEC_X0 + s * SEC_PITCH
        plate(c, x, SEC_Y, SEC_W, SEC_H, (225, 225, 225), 12)
        c.fill(x + 14, SEC_Y + 40, SEC_W - 28, 1, (60, 60, 60))           # under the header
    plate(c, 18, STRIP_Y, 300, STRIP_H)
    plate(c, 336, STRIP_Y, 300, STRIP_H)
    plate(c, HELP_X, STRIP_Y, 1262 - HELP_X, STRIP_H, (200, 200, 200))
    return c


def led_strip(size, rings, frames_lit):
    """Concentric-ring LED (the logo's motif); frame k lights frames_lit(k) rings."""
    frames = []
    for k in range(len(frames_lit)):
        f = Canvas(size, size, (0, 0, 0))
        for i in range(rings):
            f.ring(size // 2, size // 2, 2.5 + i * 2.0, (255, 255, 255) if i < frames_lit[k] else (38, 38, 38), 1.1)
        frames.append(f)
    out = Canvas(size, size * len(frames))
    out.rows = [r for fr in frames for r in fr.rows]
    out.h = len(out.rows)
    return out


def led_bar():
    w, rows = LEDS * LED_PITCH, []
    for lit in range(LEDS + 1):
        for y in range(LED_H):
            row = bytearray(bytes((0, 0, 0)) * w)
            if 2 <= y < LED_H - 2:
                for i in range(LEDS):
                    col = (255, 255, 255) if i < lit else (40, 40, 40)
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


def button_images():
    """{file: (w, h, text, lit)} for every labelled button face."""
    want = {}
    for name, caps in SELECTORS.items():
        w = SEG_W[len(caps)]
        for i, cap in enumerate(caps):
            for lit in (True, False):
                want["sel_%s_%d_%s.png" % (slug(name), i, "on" if lit else "off")] = (w, SEG_H, cap, lit)
    for name, cap in ACTIONS.items():
        want["act_%s.png" % slug(name)] = (ACT_W, ACT_H, cap, False)        # same face on and off
    want["trig_on.png"] = (TRIG_W, ACT_H, "trigger  on", True)
    want["trig_off.png"] = (TRIG_W, ACT_H, "trigger", False)
    return want


def render_buttons():
    """Draws the labelled button faces with Pillow (lit = white face, black text; unlit = black
    face, grey outline, light text). Skipped when Pillow or the font is missing: the committed
    images in skin_src/assets/buttons/ are used instead."""
    want = button_images()
    try:
        from PIL import Image, ImageDraw, ImageFont
        if not os.path.isfile(FONT_FILE):
            raise ImportError(FONT_FILE)
    except ImportError as e:
        missing = [f for f in want if not os.path.isfile(os.path.join(BTN_DIR, f))]
        if missing:
            sys.exit("make_skin: button images missing (%s) and cannot render them (%s): run once with Pillow"
                     % (", ".join(missing[:3]), e))
        return want
    os.makedirs(BTN_DIR, exist_ok=True)
    for f, (w, h, text, lit) in want.items():
        S = 3                                                            # supersample
        im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, fill=(255, 255, 255, 255) if lit else (0, 0, 0, 255),
                            outline=(255, 255, 255, 255) if lit else (120, 120, 120, 255), width=2 * S)
        size = 21
        while True:
            font = ImageFont.truetype(FONT_FILE, size * S)
            try:
                font.set_variation_by_name("Light" if not lit else "Regular")
            except Exception:
                pass
            box = d.textbbox((0, 0), text, font=font)
            if box[2] - box[0] <= (w - 10) * S or size <= 11:
                break
            size -= 1
        tw, th = box[2] - box[0], box[3] - box[1]
        d.text(((w * S - tw) / 2 - box[0], (h * S - th) / 2 - box[1]), text, font=font,
               fill=(0, 0, 0, 255) if lit else (225, 225, 225, 255))
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


def caption(text, bounds, size=15, colour=MUTED, just="left verticallyCentred"):
    return label("cap " + text, bounds, "Text", None, ts(size, colour, HEAD_FONT, "Light", just, "Lower Case"), text)


def image(name, file, bounds, frames=None, handle=None):
    if frames:
        data = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", file),
                   ("numFrames", frames), ("handleName", handle))
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
    return G.entry("focus", "Focus", G.focus_data("ffffffff"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


NAME_STYLE = lambda: ts(17, MUTED, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case")   # noqa: E731


def knob_def(image_file):
    kids = [focus(CTL_W, CTL_H),
            G.entry("knob", "Knob", G.knob_filmstrip_data(image_file, 127), "8 16 56 56", "No", "Show", "Always"),
            label("name", "78 8 204 30", "Name", "Data", NAME_STYLE()),
            label("value", "78 42 204 36", "Value", "Data", ts(25, WHITE))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def selector_def(name):
    """Name + the active choice on top; below it, the choice buttons with their text in the image."""
    n = len(SELECTORS[name])
    w = SEG_W[n]
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 4 150 28", "Name", "Data", NAME_STYLE()),
            label("value", "150 4 126 28", "Value", "Data", ts(19, WHITE, "Roboto", "Regular", "right verticallyCentred"))]
    for i in range(n):
        kids.append(G.entry("seg %d" % i, "Button",
                            G.button_data("sel_%s_%d_on.png" % (slug(name), i), "sel_%s_%d_off.png" % (slug(name), i), i, n),
                            "%d %d %d %d" % (12 + i * (w + SEG_GAP), SEG_Y, w, SEG_H), "No", "Show", "Always"))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def toggle_def():
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 8 190 30", "Name", "Data", NAME_STYLE()),
            label("value", "12 42 190 36", "Value", "Data", ts(25, WHITE)),
            G.entry("toggle", "Button", G.button_data("toggle_on.png", "toggle_off.png", 1, 1), "210 28 64 32", "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def action_def(name):
    """An action button: its text is in the image; the same face on and off (each tap flips the
    switch and the plugin acts on every flip), the result shows in the page's status line."""
    img = "act_%s.png" % slug(name)
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 4 270 26", "Name", "Data", NAME_STYLE()),
            G.entry("button", "Button", G.button_data(img, img, 1, 1), "12 34 %d %d" % (ACT_W, ACT_H), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def trigger_def():
    """Trigger: lit while on (the stutter loops while Trigger is on)."""
    kids = [focus(CTL_W, CTL_H),
            label("name", "12 4 200 26", "Name", "Data", NAME_STYLE()),
            G.entry("button", "Button", G.button_data("trig_on.png", "trig_off.png", 1, 1), "12 34 %d %d" % (TRIG_W, ACT_H), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def slot(name, comp, bounds, p):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, "Yes", "Hide", "Always",
                   remap=[("Data", p.handle)])


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
        kids.append(label("section %d" % s, "%d %d %d 34" % (x0 + 16, SEC_Y + 4, SEC_W - 80), "Text", None,
                          ts(22, WHITE, HEAD_FONT, "Light", "left verticallyCentred", "Lower Case"), header))
        if page_name == "FILTER" and s == 0:      # envelope activity LED in the section header
            kids.append(image("env led", "env_led.png", "%d %d 44 44" % (x0 + SEC_W - 58, SEC_Y - 2), 11, by_name["Env Level"].handle))
            trig.append((HELP_ENV_LED, x0 + SEC_W - 62, SEC_Y, 52, 40))
        for r, row in enumerate(rows):
            if row is None:
                continue
            x, y = x0 + 6, ROW_Y0 + r * ROW_PITCH
            if isinstance(row, tuple):            # a read-out line: caption + value, nothing to press
                _, pname, cap = row
                kids.append(caption(cap, "%d %d 270 26" % (x + 12, y + 4)))
                kids.append(label("%s value" % pname, "%d %d 276 50" % (x + 12, y + 32), "Value", by_name[pname].handle, ts(21, WHITE)))
                trig.append((HELP_FOR[pname], x + 12, y + 2, 270, 30))
                continue
            p = by_name[row]
            if row in TOGGLES:
                comp, hx = "toggleC", (x + 12, y + 6, 180, 32)
            elif row in SELECTORS:
                comp, hx = "sel_" + slug(row), (x + 12, y + 2, 136, 30)
            elif row in ACTIONS:
                comp, hx = "act_" + slug(row), (x + 12, y + 2, 270, 28)
            elif row == TRIGGER:
                comp, hx = "trigC", (x + 12, y + 2, 200, 28)
                kids.append(slot(row, comp, "%d %d %d %d" % (x, y, CTL_W, CTL_H), p))
                # Stutter LED beside the button (page level, not over it): lit while recording / looping
                kids.append(image("stutter led", "stut_led.png", "%d %d 44 44" % (x + 24 + TRIG_W, y + 35), 2, by_name["Stutter LED"].handle))
                kids.append(caption("looping", "%d %d 80 26" % (x + 76 + TRIG_W, y + 44)))
                trig.append((HELP_STUT_LED, x + 76 + TRIG_W, y + 40, 80, 34))
                trig.append((HELP_FOR[row], *hx))
                continue
            else:
                comp, hx = ("knobCBi" if row in BIPOLAR else "knobC"), (x + 78, y + 6, 200, 32)
            kids.append(slot(row, comp, "%d %d %d %d" % (x, y, CTL_W, CTL_H), p))
            trig.append((HELP_FOR[row], *hx))
    # bottom strip: two meters and the help window
    for j, (cap, pname, hid) in enumerate(METERS.get(page_name, METERS[None])):
        x = 18 + j * 318
        kids.append(caption(cap, "%d %d 270 26" % (x + 14, STRIP_Y + 8)))
        kids.append(image("meter %d" % j, "led_bar.png", "%d %d %d %d" % (x + 14, STRIP_Y + 48, LEDS * LED_PITCH, LED_H), LEDS + 1, by_name[pname].handle))
        kids.append(label("meter %d value" % j, "%d %d 270 40" % (x + 14, STRIP_Y + 80), "Value", by_name[pname].handle, ts(26, WHITE)))
        trig.append((hid, x, STRIP_Y, 300, STRIP_H))
    hw = 1262 - HELP_X
    kids.append(caption("help " + SEP + " touch a name or a meter", "%d %d %d 26" % (HELP_X + 16, STRIP_Y + 8, hw - 32)))
    kids.append(label("help 1", "%d %d %d 36" % (HELP_X + 16, STRIP_Y + 48, hw - 32), "Value", help1.handle, ts(22, WHITE)))
    kids.append(label("help 2", "%d %d %d 36" % (HELP_X + 16, STRIP_Y + 90, hw - 32), "Value", help2.handle, ts(22, WHITE)))
    trig.append((0, HELP_X, STRIP_Y, hw, STRIP_H))
    for t in trig:                                 # invisible help buttons: over names and meters only
        kids.append(help_trigger(t[0], t[1], t[2], t[3], t[4], help1))
    return G.component_definition([], kids, "ff000000")


def write_ui_json():
    """ui.json mirrors the layout: a 4 x 4 grid per page, row-major, one section per column, so the
    Q-Link banks (Bank Direction "Column") are the sections."""
    pages = []
    for name, sections in PAGES:
        controls = []
        for r in range(4):
            for c in range(4):
                row = sections[c][1][r] if c < len(sections) else None
                if row is None or isinstance(row, tuple):
                    controls.append("empty")
                elif row in TOGGLES or row in SELECTORS or row in ACTIONS or row == TRIGGER:
                    controls.append({"param": row, "widget": "switch"})
                else:
                    controls.append({"param": row, "widget": "knob"})
        while controls and controls[-1] == "empty":
            controls.pop()
        pages.append({"name": name, "controls": controls})
    ui = {"kit": "mono", "pages": pages,
          "style": {"background": "ff000000", "text": "ffffffff", "muted": "ff9a9a9a", "accent": "ffffffff"}}
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
    buttons = render_buttons()

    page_data = find(tui, "pageData")
    defs = find(find(page_data, "componentDefinitions"), "localComponentDefinitions")
    defs.append(obj(("key", "knobC"), ("value", knob_def("knob_small_uni.png"))))
    defs.append(obj(("key", "knobCBi"), ("value", knob_def("knob_small_bi.png"))))
    defs.append(obj(("key", "toggleC"), ("value", toggle_def())))
    defs.append(obj(("key", "trigC"), ("value", trigger_def())))
    for name in SELECTORS:
        defs.append(obj(("key", "sel_" + slug(name)), ("value", selector_def(name))))
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
    own = {"panel_%s.png" % n.lower() for n, _ in PAGES} | {"env_led.png", "stut_led.png", "led_bar.png"} | \
          set(buttons) | {"hit_%dx%d.png" % s for s in HIT_SIZES}
    for img in sorted(G.referenced_images(tui, set()) - own):        # kit art: knobs, toggle
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
    for f in buttons:
        shutil.copy2(os.path.join(BTN_DIR, f), os.path.join(OUT, f))
    for name, sections in PAGES:
        page_panel(len(sections)).save(os.path.join(OUT, "panel_%s.png" % name.lower()))
    led_strip(44, 10, list(range(11))).save(os.path.join(OUT, "env_led.png"))
    led_strip(44, 10, [0, 10]).save(os.path.join(OUT, "stut_led.png"))
    led_bar().save(os.path.join(OUT, "led_bar.png"))
    for w, h in sorted(HIT_SIZES):
        hit_image(os.path.join(OUT, "hit_%dx%d.png" % (w, h)), w, h)
    # unused generator art must not be referenced
    referenced = G.referenced_images(tui, set())
    missing = sorted(i for i in referenced if not os.path.isfile(os.path.join(OUT, i)))
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
