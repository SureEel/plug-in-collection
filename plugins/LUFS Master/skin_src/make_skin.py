"""Builds LUFS Master's hand-written skin (plugins/LUFS Master/skins/) for the MPC touch screen.

MASTER: a rack-mount panel with the three meters (input level, output integrated loudness, gain
change), LUFS over time against the target, the mode bar (Manual / Measure), the Live Speed
switch (Fast / Off / Slow), the knobs, auto-set with Clear / Undo, Reset / Refresh and help.
METERS: output loudness over time against the target, and every readout as a labelled number.

Rules learned on the MPC hardware (Sleepy Android, Hydra Eel, Elver):
  * nothing lies on top of a button: button text is drawn INTO the button images (Pillow);
  * action buttons (Clear, Undo, Reset, Refresh) are plain on/off switches; the plugin acts on
    every change;
  * static text is bound to the plugin's "Panel" parameter, which never changes (not to
    parameter 0, which the host writes on every prepare); no conditional visibility;
  * help: two value labels (Help / Help 2) set by invisible buttons over names and meters.
CPU: one static panel image per page; meters are film strips (no drawing at run time); the plugin
refreshes meters at 10 Hz and the other numbers at 5 Hz.

usage (from the SDK root, after one native build so build/params.json exists; needs Pillow):
    python "plugins/LUFS Master/skin_src/make_skin.py"
    python tools/build.py "plugins/LUFS Master"
"""
import os
import random
import shutil
import struct
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
FONT_FILE = r"C:\Windows\Fonts\bahnschrift.ttf"

INK = "ffe4e6e9"
MUTED = "ff8f9399"
LED_GREEN_TXT = "ff6ff59a"
LED_AMBER_TXT = "ffffc04d"
LED_ORANGE_TXT = "ffffa53a"
GREEN, AMBER, RED, WHITE = (61, 220, 106), (255, 176, 46), (255, 59, 48), (236, 236, 236)

P = {}          # parameter name -> index (from build/params.json)

# geometry (MASTER)
C1, C1W = 64, 528           # column 1: mode bar, input level, knobs, auto-set
C2, C2W = 616, 176          # column 2: live speed, gain change
C3, C3W = 816, 400          # column 3: output integrated, LUFS over time, target
IN_LEDS, IN_PITCH, IN_LED_W, IN_H, IN_Y = 48, 11, 9, 30, 178          # input level -48..0 dBFS, 1 dB per LED
OI_LEDS, OI_PITCH, OI_LED_W, OI_H, OI_Y = 36, 11, 9, 26, 178          # output integrated: target-24 .. target+12
GC_LEDS, GC_PITCH, GC_LED_W, GC_H, GC_X, GC_Y = 24, 7, 5, 24, C2 + 4, 306   # gain change -24..+24 dB, 2 dB per LED
KNOB_W, KNOB_H, KNOB_SIZE = 260, 84, 56
MODE_W, MODE_H, MODE_Y = 258, 60, 82
SPEED_W, SPEED_H, SPEED_Y, SPEED_GAP = 176, 44, 82, 4
ACT_W, ACT_H, ACT_Y = 120, 50, 512
TARGET_W, TARGET_H = 336, 44
HELP_X, HELP_Y, HELP_W, HELP_H = 616, 440, 600, 176
# METERS: graph of output loudness vs target, 36 LU tall (target at 24 LU from the bottom)
HX0, HPITCH, HCOL_W = 120, 22, 18
HY0, HPX_PER_LU, HLU = 64, 6, 36
HPLOT_H = HLU * HPX_PER_LU
HIST_COLS = 48

HELP_N = 19     # must equal kNumHelp in the plugin


# ------------------------------------------------------------------------------------------
# Tiny raster helpers (pure Python, no dependencies)
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

    def vgrad(self, x, y, w, h, top, bottom):
        for i in range(h):
            t = i / max(1, h - 1)
            self.fill(x, y + i, w, 1, tuple(int(round(top[c] + (bottom[c] - top[c]) * t)) for c in range(3)))

    def disc(self, cx, cy, r, rgb):
        for yy in range(-r, r + 1):
            half = int((r * r - yy * yy) ** 0.5)
            self.fill(cx - half, cy + yy, 2 * half + 1, 1, rgb)

    def save(self, path):
        def chunk(tag, data):
            body = tag + data
            return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        raw = b"".join(b"\0" + bytes(r) for r in self.rows)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6 if self.n == 4 else 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


def stack(frames):
    out = Canvas(frames[0].w, frames[0].h * len(frames), alpha=frames[0].n == 4)
    out.rows = [row for fr in frames for row in fr.rows]
    out.h = len(out.rows)
    return out


def recess(c, x, y, w, h):
    c.fill(x, y, w, h, (10, 11, 12))
    c.fill(x, y, w, 1, (4, 4, 5))
    c.fill(x, y, 1, h, (6, 6, 7))
    c.fill(x, y + h - 1, w, 1, (64, 67, 71))
    c.fill(x + w - 1, y, 1, h, (52, 55, 59))


def groove(c, y, x0=64, x1=1216):
    c.fill(x0, y, x1 - x0, 1, (16, 17, 18))
    c.fill(x0, y + 1, x1 - x0, 1, (60, 62, 66))


def vgroove(c, x, y0, y1):
    c.fill(x, y0, 1, y1 - y0, (16, 17, 18))
    c.fill(x + 1, y0, 1, y1 - y0, (60, 62, 66))


def screw(c, cx, cy):
    c.disc(cx, cy, 10, (18, 18, 20))
    c.disc(cx, cy, 8, (86, 89, 94))
    c.fill(cx - 6, cy - 1, 13, 3, (30, 31, 34))


def brushed_base():
    rnd = random.Random(1770)
    W, H = 1280, 628
    c = Canvas(W, H)
    level = 0.0
    for y in range(H):
        level = 0.8 * level + 0.2 * rnd.uniform(-3, 3)
        base = 40 - 6 * y / H + level
        row = c.rows[y]
        x = 0
        while x < W:
            run = rnd.randint(8, 90)
            v = int(round(base + rnd.choice((-1, 0, 0, 1))))
            px = bytes((v, v + 2, v + 4)) * min(run, W - x)
            row[x * 3:(x + len(px) // 3) * 3] = px
            x += run
    for x0 in (0, W - 44):
        c.vgrad(x0, 0, 44, H, (30, 31, 34), (24, 25, 27))
        c.fill(x0 + (43 if x0 == 0 else 0), 0, 1, H, (62, 64, 68))
        for cy in (44, H - 44):
            screw(c, x0 + 22, cy)
    return c


# ------------------------------------------------------------------------------------------
# Artwork
# ------------------------------------------------------------------------------------------
def draw_panel():
    c = brushed_base()
    groove(c, 54)
    groove(c, 232)
    groove(c, 428)
    vgroove(c, C2 - 12, 60, 424)
    vgroove(c, C3 - 12, 60, 424)
    recess(c, C1, IN_Y - 4, IN_LEDS * IN_PITCH + 2, IN_H + 8)              # input level window
    for v in (-48, -36, -24, -18, -12, -6, -3, 0):
        c.fill(C1 + 1 + (v + 48) * IN_PITCH - 1, IN_Y + IN_H + 5, 2, 6, (128, 132, 138))
    recess(c, C3, 80, C3W, 92)                                             # output integrated number
    recess(c, C3, OI_Y - 4, OI_LEDS * OI_PITCH + 6, OI_H + 8)              # its bar
    tx = C3 + 2 + 24 * OI_PITCH - 1                                        # the target line
    c.fill(tx, OI_Y - 8, 2, OI_H + 16, (240, 240, 240))
    recess(c, C2, 262, C2W, 40)                                            # gain change number
    recess(c, GC_X - 4, GC_Y - 4, GC_LEDS * GC_PITCH + 8, GC_H + 8)        # gain change bar
    c.fill(GC_X + 12 * GC_PITCH - 1, GC_Y - 8, 2, GC_H + 16, (240, 240, 240))   # 0 dB line
    recess(c, C3, 262, C3W, 48)                                            # LUFS over time
    recess(c, C1, 464, C1W, 40)                                            # auto-set status
    recess(c, HELP_X, HELP_Y, HELP_W, HELP_H)
    return c


def led_strip(n, pitch, led_w, h, colour_of, centre=None):
    """Film strip with n+1 frames. Plain: frame k lights LEDs 0..k-1. Centre (bipolar): frame k
    lights from the centre to k."""
    frames = []
    for lit in range(n + 1):
        f = Canvas(n * pitch, h, (10, 11, 12))
        for i in range(n):
            on_col = colour_of(i)
            off_col = tuple(int(v * 0.16) + 6 for v in on_col)
            on = i < lit if centre is None else ((centre <= i < lit) or (lit <= i < centre))
            x = i * pitch + 1
            if on:
                f.vgrad(x, 2, led_w, h - 4, tuple(min(255, v + 40) for v in on_col), on_col)
            else:
                f.fill(x, 2, led_w, h - 4, off_col)
        frames.append(f)
    return stack(frames)


def hold_strip():
    """Peak-hold marker: one bright LED at the hold position, transparent elsewhere (laid over the
    input level strip)."""
    frames = []
    for k in range(IN_LEDS + 1):
        f = Canvas(IN_LEDS * IN_PITCH, IN_H, (0, 0, 0), alpha=True)
        f.fill(0, 0, f.w, f.h, (0, 0, 0), a=0)
        if k > 0:
            f.fill((k - 1) * IN_PITCH + 1, 2, IN_LED_W, IN_H - 4, (255, 255, 255))
        frames.append(f)
    return stack(frames)


def hist_strip():
    """METERS graph: one bar per frame (frame k = k LU above target - 24), transparent above."""
    frames = []
    for lit in range(HLU + 1):
        f = Canvas(HCOL_W, HPLOT_H, (0, 0, 0), alpha=True)
        f.fill(0, 0, HCOL_W, HPLOT_H, (0, 0, 0), a=0)
        for i in range(lit):
            col = GREEN if i < 25 else (AMBER if i < 28 else RED)
            f.fill(0, HPLOT_H - (i + 1) * HPX_PER_LU, HCOL_W, HPX_PER_LU, col)
        if lit:
            f.fill(0, HPLOT_H - lit * HPX_PER_LU, HCOL_W, 1, (255, 255, 255))
        frames.append(f)
    return stack(frames)


def draw_meters_panel():
    c = brushed_base()
    groove(c, 346)
    recess(c, 64, 48, 1152, 290)
    for lu in range(0, HLU + 1, 6):
        y = HY0 + HPLOT_H - lu * HPX_PER_LU
        c.fill(HX0 - 6, y, HIST_COLS * HPITCH + 6, 1, (44, 46, 50))
    y = HY0 + HPLOT_H - 24 * HPX_PER_LU                                   # the target line
    c.fill(HX0 - 10, y - 1, HIST_COLS * HPITCH + 14, 2, (240, 240, 240))
    return c


def font(size, variant="SemiBold"):
    f = ImageFont.truetype(FONT_FILE, size)
    try:
        f.set_variation_by_name(variant)
    except Exception:
        pass
    return f


def switch_cap(w, h, lit, led, lines):
    """A hardware push-switch cap with its text drawn in: dark rounded square, LED bar on top."""
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bot = ((70, 73, 78), (50, 52, 56)) if lit else ((52, 54, 58), (34, 36, 39))
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, fill=(8, 8, 9, 255))
    for i in range(1, h * S - 1):
        t = i / (h * S)
        d.line((S, i, w * S - 1 - S, i), fill=tuple(int(top[c] + (bot[c] - top[c]) * t) for c in range(3)) + (255,))
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=6 * S, outline=(8, 8, 9, 255), width=S)
    lc = led if lit else tuple(int(v * 0.28) + 10 for v in led)
    lw = min(34, w - 20)
    d.rectangle(((w - lw) // 2 * S, 5 * S, ((w - lw) // 2 + lw) * S, 9 * S), fill=lc + (255,))
    txt = (236, 238, 240, 255) if lit else (176, 180, 186, 255)
    if len(lines) == 1:
        sizes = [(lines[0], 20)]
    else:
        sizes = [(lines[0], 18), (lines[1], 12)]
    total = sum(s for _, s in sizes) + 4 * (len(sizes) - 1)
    y = (h - total) / 2 + 5
    for text, size in sizes:
        f = font(size * S, "Bold" if size >= 16 else "SemiBold")
        box = d.textbbox((0, 0), text, font=f)
        d.text(((w * S - (box[2] - box[0])) / 2 - box[0], y * S - box[1]), text, font=f, fill=txt)
        y += size + 4
    return im.resize((w, h), Image.LANCZOS)


# ------------------------------------------------------------------------------------------
# TUI pieces
# ------------------------------------------------------------------------------------------
def text_style(height, colour, fnt="Roboto", style="Regular", just="left verticallyCentred", case="Original"):
    return obj(("version", 1),
               ("font", obj(("version", 1), ("name", fnt), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def lab(name, bounds, kind, handle, ts, text=None):
    # static text is bound to "Panel", which never changes
    pairs = [("version", 1), ("textStyle", ts), ("type", kind), ("handleName", "Parameter %d" % P["Panel"] if kind == "Text" else handle)]
    if text is not None:
        pairs.append(("text", text))
    return G.entry(name, "Label", Obj(pairs), bounds, "No", "Show", "Always")


def caption(text, bounds, height=17, colour=MUTED, just="left verticallyCentred"):
    return lab("cap " + text, bounds, "Text", None, text_style(height, colour, "Roboto", "Medium", just, "Original"), text)


def value(name, bounds, pname, height=24, colour=INK, just="left verticallyCentred", style="Bold"):
    return lab(name, bounds, "Value", "Parameter %d" % P[pname], text_style(height, colour, "Roboto", style, just))


HIT_SIZES = set()


def help_trigger(item, bounds):
    w, h = (int(v) for v in bounds.split()[2:4])
    HIT_SIZES.add((w, h))
    img = "hit_%dx%d.png" % (w, h)
    data = obj(("version", 2), ("onImage", img), ("offImage", img), ("buttonId", item),
               ("numButtonsInGroup", HELP_N), ("handleName", "Parameter %d" % P["Help"]), ("gestureBehaviour", "Instant"))
    return G.entry("help %d %s" % (item, bounds), "Button", data, bounds, "No", "Show", "Always")


def image(name, file, bounds, frames=None, handle=None):
    if frames:
        data = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", file), ("numFrames", frames), ("handleName", handle))
    else:
        data = obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", file))
    return G.entry(name, "Image", data, bounds, "No", "Show", "Always")


def slot(name, comp, bounds, pname):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, "Yes", "Hide", "Always",
                   remap=[("Data", "Parameter %d" % P[pname])])


def focus(accent, w, h):
    return G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def radio_component(accent, prefix, n, w, h, vertical, gap):
    W = w if vertical else n * w + (n - 1) * gap
    H = n * h + (n - 1) * gap if vertical else h
    kids = [focus(accent, W, H)]
    for i in range(n):
        x, y = (0, i * (h + gap)) if vertical else (i * (w + gap), 0)
        kids.append(G.entry("btn %d" % i, "Button", G.button_data("%s%d_on.png" % (prefix, i), "%s%d_off.png" % (prefix, i), i, n),
                            "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always"))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def action_component(accent, img):
    kids = [focus(accent, ACT_W, ACT_H),
            G.entry("btn", "Button", G.button_data(img, img, 1, 1), "0 0 %d %d" % (ACT_W, ACT_H), "No", "Show", "Always")]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def knob_component(accent):
    s = KNOB_SIZE
    kids = [focus(accent, KNOB_W, KNOB_H),
            G.entry("knob", "Knob", G.knob_filmstrip_data("knob_small_uni.png", 127), "8 %d %d %d" % ((KNOB_H - s) // 2, s, s), "No", "Show", "Always"),
            lab("name", "%d 6 %d 32" % (s + 20, KNOB_W - s - 24), "Name", "Data", text_style(17, MUTED, "Roboto", "Medium", "left verticallyCentred", "Upper Case")),
            lab("value", "%d 40 %d 38" % (s + 20, KNOB_W - s - 24), "Value", "Data", text_style(24, INK, "Roboto", "Bold", "left verticallyCentred"))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def target_component(accent):
    kids = [focus(accent, TARGET_W, TARGET_H),
            lab("target caption", "14 0 90 %d" % TARGET_H, "Text", None, text_style(15, MUTED, "Roboto", "Medium", "left verticallyCentred"), "TARGET"),
            lab("target value", "92 0 %d %d" % (TARGET_W - 104, TARGET_H), "Value", "Data", text_style(22, LED_AMBER_TXT, "Roboto", "Bold", "left verticallyCentred"))]
    acts = [G.action("Mouse Down", "Q-Link", "Data", ""),
            G.action("Double Click", "Show Overlay", "Data", "menu overlay"),
            G.action("Enter Pressed", "Show Overlay", "Data", "menu overlay")]
    return G.component_definition(acts, kids, "00000000")


def master_tab():
    kids = [image("panel", "panel.png", "0 0 1280 628"),
            lab("title", "64 8 330 42", "Text", None, text_style(32, INK, "Nexa", "Bold", "left verticallyCentred", "Upper Case"), "LUFS Master"),
            caption("LOUDNESS LEVELER  \u00b7  TRUE-PEAK LIMITER", "320 12 520 36", 17),
            lab("brand", "900 8 316 42", "Text", None, text_style(24, MUTED, "Nexa", "Bold", "right verticallyCentred"), "sureeel")]
    # --- column 1: mode bar, input level ---
    kids += [caption("MODE", "%d 58 200 22" % C1),
             slot("mode", "modeBar", "%d %d %d %d" % (C1, MODE_Y, 2 * MODE_W + 12, MODE_H), "Mode"),
             caption("INPUT LEVEL  \u00b7  dBFS  \u00b7  RAW, BEFORE ANY GAIN", "%d 150 380 24" % C1, 15),
             caption("HOLD", "%d 150 50 24" % (C1 + 384), 15),
             value("in hold", "%d 148 96 26" % (C1 + 432), "In Peak Hold", 18, LED_AMBER_TXT, "right verticallyCentred"),
             image("in level", "in_strip.png", "%d %d %d %d" % (C1 + 1, IN_Y, IN_LEDS * IN_PITCH, IN_H), IN_LEDS + 1, "Parameter %d" % P["In Level"]),
             image("in hold mark", "hold_strip.png", "%d %d %d %d" % (C1 + 1, IN_Y, IN_LEDS * IN_PITCH, IN_H), IN_LEDS + 1, "Parameter %d" % P["In Peak Hold"])]
    for v in (-48, -36, -24, -18, -12, -6, 0):
        x = C1 + 1 + (v + 48) * IN_PITCH
        kids.append(lab("in scale %d" % v, "%d %d 44 18" % (x - 22, IN_Y + IN_H + 10), "Text", None,
                        text_style(14, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"), str(v)))
    # --- column 2: live speed ---
    kids += [caption("LIVE SPEED", "%d 58 %d 22" % (C2, C2W)),
             slot("live speed", "speedSwitch", "%d %d %d %d" % (C2, SPEED_Y, SPEED_W, 3 * SPEED_H + 2 * SPEED_GAP), "Live Speed")]
    # --- column 3: output integrated + bar against target ---
    kids += [caption("OUTPUT INTEGRATED LOUDNESS  \u00b7  LUFS  \u00b7  THIS PASS", "%d 58 %d 22" % (C3, C3W), 15),
             value("out int", "%d 80 %d 92" % (C3, C3W), "Out Int LUFS", 72, LED_GREEN_TXT, "horizontallyCentred verticallyCentred"),
             image("out int bar", "oi_strip.png", "%d %d %d %d" % (C3 + 3, OI_Y, OI_LEDS * OI_PITCH, OI_H), OI_LEDS + 1, "Parameter %d" % P["Out Int LUFS"])]
    for lu, txt in ((0, "-24 LU"), (12, "-12"), (24, "TARGET"), (36, "+12")):
        x = C3 + 3 + lu * OI_PITCH
        kids.append(lab("oi scale %d" % lu, "%d %d 70 18" % (max(C3, x - 35), OI_Y + OI_H + 8), "Text", None,
                        text_style(14, INK if lu == 24 else MUTED, "Roboto", "Bold" if lu == 24 else "Regular", "horizontallyCentred verticallyCentred"), txt))
    # --- row 2: knobs | gain change | LUFS over time + target ---
    kids += [slot("input gain", "knobC", "%d 244 %d %d" % (C1, KNOB_W, KNOB_H), "Input Gain"),
             slot("max change", "knobC", "%d 244 %d %d" % (C1 + 268, KNOB_W, KNOB_H), "Max Change"),
             slot("ceiling", "knobC", "%d 334 %d %d" % (C1, KNOB_W, KNOB_H), "Ceiling"),
             slot("custom", "knobC", "%d 334 %d %d" % (C1 + 268, KNOB_W, KNOB_H), "Custom LUFS"),
             caption("GAIN CHANGE  \u00b7  dB", "%d 238 %d 22" % (C2, C2W)),
             value("gain change", "%d 262 %d 40" % (C2, C2W), "Gain Change", 30, LED_AMBER_TXT, "horizontallyCentred verticallyCentred"),
             image("gain bar", "gc_strip.png", "%d %d %d %d" % (GC_X, GC_Y, GC_LEDS * GC_PITCH, GC_H), GC_LEDS + 1, "Parameter %d" % P["Gain Change"])]
    for txt, x in (("-24", GC_X), ("0", GC_X + 12 * GC_PITCH), ("+24", GC_X + GC_LEDS * GC_PITCH)):
        kids.append(lab("gc scale %s" % txt, "%d %d 40 16" % (x - 20, GC_Y + GC_H + 6), "Text", None,
                        text_style(13, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"), txt))
    kids += [caption("LIMITER  \u00b7  dB", "%d 364 %d 22" % (C2, C2W), 15),
             value("limiter", "%d 386 %d 32" % (C2, C2W), "Limiter GR", 24, LED_ORANGE_TXT, "horizontallyCentred verticallyCentred"),
             caption("LUFS OVER TIME  \u00b7  OUTPUT, LAST 3 s", "%d 238 260 22" % C3, 15),
             caption("VS TARGET", "%d 238 130 22" % (C3 + 270), 15, MUTED, "right verticallyCentred"),
             value("lufs now", "%d 262 230 48" % (C3 + 10), "Out ST LUFS", 36, LED_GREEN_TXT),
             value("vs target", "%d 262 150 48" % (C3 + 240), "Vs Target", 30, LED_AMBER_TXT, "right verticallyCentred"),
             slot("target", "targetSelect", "%d 318 %d %d" % (C3, TARGET_W, TARGET_H), "Target"),
             caption("SUGGESTED GAIN  \u00b7  dB", "%d 368 190 22" % C3, 15),
             value("suggested", "%d 390 190 32" % C3, "Suggested", 24, INK),
             caption("OUTPUT TRUE PEAK MAX  \u00b7  dBTP", "%d 368 200 22" % (C3 + 200), 14, MUTED, "right verticallyCentred"),
             value("out tp", "%d 390 200 32" % (C3 + 200), "Out TP Max", 24, INK, "right verticallyCentred")]
    # --- row 3: auto-set, clear / undo, reset / refresh ---
    kids += [caption("AUTO-SET GAIN  \u00b7  SET ON STOP IN MEASURE", "%d 438 %d 22" % (C1, C1W), 15),
             value("auto status", "%d 466 %d 36" % (C1 + 12, C1W - 24), "Auto-Set", 24, LED_AMBER_TXT),
             slot("clear", "actClear", "%d %d %d %d" % (C1, ACT_Y, ACT_W, ACT_H), "Clear"),
             slot("undo", "actUndo", "%d %d %d %d" % (C1 + 132, ACT_Y, ACT_W, ACT_H), "Undo"),
             slot("reset", "actReset", "%d %d %d %d" % (C1 + 276, ACT_Y, ACT_W, ACT_H), "Reset"),
             slot("refresh", "actRefresh", "%d %d %d %d" % (C1 + 408, ACT_Y, ACT_W, ACT_H), "Refresh"),
             caption("CLEAR = 0 dB  \u00b7  UNDO = BACK", "%d 570 252 22" % C1, 13),
             caption("RESET = NEW PASS  \u00b7  REFRESH = MAX", "%d 570 252 22" % (C1 + 276), 13),
             caption("HELP", "%d %d 200 24" % (HELP_X + 16, HELP_Y + 8), 15),
             value("help 1", "%d %d %d 32" % (HELP_X + 16, HELP_Y + 56, HELP_W - 32), "Help", 22, INK, style="Regular"),
             value("help 2", "%d %d %d 32" % (HELP_X + 16, HELP_Y + 96, HELP_W - 32), "Help 2", 22, INK, style="Regular")]
    # help triggers: over captions, meters and knob names only - never over a button
    ny = KNOB_SIZE + 20
    trig = [(0, (HELP_X, HELP_Y, HELP_W, HELP_H)),
            (1, (C1, 58, 200, 22)), (2, (C2, 58, C2W, 22)),
            (3, (C1 + ny, 248, KNOB_W - ny, 32)), (4, (C1 + 268 + ny, 248, KNOB_W - ny, 32)),
            (12, (C1 + ny, 338, KNOB_W - ny, 32)), (13, (C1 + 268 + ny, 338, KNOB_W - ny, 32)),
            (5, (C1, 150, C1W, 82)),
            (6, (C3, 58, C3W, 116)), (8, (C3, OI_Y - 4, C3W, 56)),
            (9, (C2, 238, C2W, 120)), (10, (C2, 364, C2W, 56)),
            (7, (C3, 238, C3W, 74)),
            (11, (C3, 368, 190, 56)), (17, (C3 + 200, 368, 200, 56)),
            (14, (C1, 438, C1W, 66)),
            (15, (C1, 570, 252, 22)), (16, (C1 + 276, 570, 252, 22))]
    for item, (x, y, w, h) in trig:
        kids.append(help_trigger(item, "%d %d %d %d" % (x, y, w, h)))
    return G.component_definition([], kids, "ff26282b")


def meters_tab():
    kids = [image("panel", "meters_panel.png", "0 0 1280 628"),
            caption("OUTPUT LOUDNESS OVER TIME  \u00b7  5 s COLUMNS  \u00b7  LU AGAINST THE TARGET", "64 12 760 30", 17),
            caption("TARGET", "900 12 130 30", 16, MUTED, "right verticallyCentred"),
            value("target", "1036 10 180 34", "Target", 20, LED_AMBER_TXT, "right verticallyCentred")]
    for lu, txt in ((0, "-24"), (6, "-18"), (12, "-12"), (18, "-6"), (24, "target"), (30, "+6"), (36, "+12")):
        y = HY0 + HPLOT_H - lu * HPX_PER_LU
        kids.append(lab("axis %d" % lu, "58 %d 54 18" % (y - 9), "Text", None,
                        text_style(14, INK if lu == 24 else MUTED, "Roboto", "Bold" if lu == 24 else "Medium", "right verticallyCentred"), txt))
    for i in range(HIST_COLS):
        kids.append(image("hist %d" % i, "hist_strip.png", "%d %d %d %d" % (HX0 + i * HPITCH, HY0, HCOL_W, HPLOT_H), HLU + 1, "Parameter %d" % (P["Hist 01"] + i)))
    x_end = HX0 + HIST_COLS * HPITCH
    small = text_style(15, MUTED, "Roboto", "Regular", "left verticallyCentred")
    kids += [lab("x old", "%d 310 120 22" % HX0, "Text", None, small, "-4 min"),
             lab("x note", "%d 310 600 22" % (HX0 + 228), "Text", None, text_style(15, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"),
                 "updates whenever audio plays, in every mode"),
             lab("x now", "%d 310 120 22" % (x_end - 120), "Text", None, text_style(15, MUTED, "Roboto", "Regular", "right verticallyCentred"), "now")]
    rows = [("INPUT", [("MOMENTARY  \u00b7  LUFS", "In Mom LUFS"), ("SHORT-TERM  \u00b7  LUFS", "In ST LUFS"), ("INTEGRATED, PASS  \u00b7  LUFS", "In Int LUFS"),
                       ("AVERAGE, UNGATED  \u00b7  LUFS", "In Avg LUFS"), ("TRUE PEAK MAX  \u00b7  dBTP", "In TP Max")]),
            ("OUTPUT", [("MOMENTARY  \u00b7  LUFS", "Out Mom LUFS"), ("SHORT-TERM  \u00b7  LUFS", "Out ST LUFS"), ("INTEGRATED, PASS  \u00b7  LUFS", "Out Int LUFS"),
                        ("AVERAGE, UNGATED  \u00b7  LUFS", "Out Avg LUFS"), ("TRUE PEAK MAX  \u00b7  dBTP", "Out TP Max")]),
            ("GAIN", [("GAIN CHANGE  \u00b7  dB", "Gain Change"), ("LIMITER  \u00b7  dB", "Limiter GR"), ("LIMITER MAX  \u00b7  dB", "GR Max"),
                      ("SUGGESTED GAIN  \u00b7  dB", "Suggested"), ("CEILING  \u00b7  dBTP", "Ceiling")])]
    for r, (group, cells) in enumerate(rows):
        y = 358 + r * 82
        kids.append(lab("group %s" % group, "64 %d 124 34" % (y + 20), "Text", None, text_style(20, INK, "Roboto", "Bold", "left verticallyCentred"), group))
        for j, (cap, pname) in enumerate(cells):
            x = 196 + j * 204
            kids.append(caption(cap, "%d %d 198 20" % (x, y), 14))
            kids.append(value("%s %d" % (group, j), "%d %d 198 34" % (x, y + 20), pname, 26, LED_GREEN_TXT if group != "GAIN" else LED_AMBER_TXT))
    kids.append(help_trigger(18, "64 12 760 30"))
    return G.component_definition([], kids, "ff26282b")


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
    accent = style["accent"]

    page = find(tui, "pageData")
    defs = find(find(page, "componentDefinitions"), "localComponentDefinitions")
    comps = [("modeBar", radio_component(accent, "mode", 2, MODE_W, MODE_H, False, 12)),
             ("speedSwitch", radio_component(accent, "speed", 3, SPEED_W, SPEED_H, True, SPEED_GAP)),
             ("targetSelect", target_component(accent)), ("knobC", knob_component(accent))]
    for n in ("Clear", "Undo", "Reset", "Refresh"):
        comps.append(("act" + n, action_component(accent, "act_%s.png" % n.lower())))
    for name, comp in comps:
        defs.append(obj(("key", name), ("value", comp)))
    tabs = find(page, "tabs")
    assert find(tabs[0], "tabName") == "MASTER" and find(tabs[1], "tabName") == "METERS"
    tabs[0] = obj(("version", 1), ("tabName", "MASTER"), ("fnKeyIndex", 0), ("fnKeySubIndex", 0), ("componentDefinition", master_tab()), ("qlinkBoundsData", []))
    tabs[1] = obj(("version", 1), ("tabName", "METERS"), ("fnKeyIndex", 1), ("fnKeySubIndex", 0), ("componentDefinition", meters_tab()), ("qlinkBoundsData", []))

    # Q-Links on MASTER (rows, top down): Mode, Live Speed, Input Gain, Target / Ceiling, Custom,
    # Max Change / Clear, Undo, Reset, Refresh. METERS: display only.
    qmap = find(find(qlinks, "Screen Mode Q-Links"), "map")
    for i, (k, v) in enumerate(qmap[1]):
        if k == "Q-Links":
            qmap[1][i] = (k, Obj(("Q-Link %d" % q, -1) for q in range(1, 17)))
    assign = {13: "Mode", 14: "Live Speed", 15: "Input Gain", 16: "Target", 9: "Ceiling", 10: "Custom LUFS", 11: "Max Change",
              5: "Clear", 6: "Undo", 7: "Reset", 8: "Refresh"}
    entry0 = qmap[0]
    for i, (k, v) in enumerate(entry0):
        if k == "Bank Direction":
            entry0[i] = (k, "RowTopDown")
        if k == "Q-Links":
            entry0[i] = (k, Obj(("Q-Link %d" % q, P[assign[q]] if q in assign else -1) for q in range(1, 17)))

    # Program mode: the controls first, then the main readouts
    prog = ["Input Gain", "Ceiling", "Target", "Custom LUFS", "Mode", "Live Speed", "Max Change", "Clear", "Undo", "Reset", "Refresh",
            "Gain Change", "Suggested", "Out Int LUFS", "Out ST LUFS", "In Level"]
    for i, (k, v) in enumerate(qlinks):
        if k == "Program Mode Q-Links":
            qlinks[i] = (k, Obj(("Q-Link %d" % (q + 1), P[n]) for q, n in enumerate(prog)))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    art = {}
    for i, (lines, led) in enumerate(((["MANUAL"], WHITE), (["MEASURE"], GREEN))):
        art["mode%d_on.png" % i] = switch_cap(MODE_W, MODE_H, True, led, lines)
        art["mode%d_off.png" % i] = switch_cap(MODE_W, MODE_H, False, led, lines)
    for i, (lines, led) in enumerate(((["FAST", "LIVE BROADCAST"], RED), (["OFF", "MEASURE ONLY"], WHITE), (["SLOW", "MASTERING"], GREEN))):
        art["speed%d_on.png" % i] = switch_cap(SPEED_W, SPEED_H, True, led, lines)
        art["speed%d_off.png" % i] = switch_cap(SPEED_W, SPEED_H, False, led, lines)
    for n in ("clear", "undo", "reset", "refresh"):
        art["act_%s.png" % n] = switch_cap(ACT_W, ACT_H, False, AMBER, [n.upper()])
    for f, im in art.items():
        im.save(os.path.join(OUT, f), optimize=True)
    own = set(art) | {"panel.png", "meters_panel.png", "in_strip.png", "hold_strip.png", "oi_strip.png", "gc_strip.png", "hist_strip.png"} | \
        {"hit_%dx%d.png" % s for s in HIT_SIZES}
    for img in sorted(G.referenced_images(tui, set()) - own):
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
    for w, h in sorted(HIT_SIZES):
        hit = Canvas(w, h, alpha=True)
        hit.fill(0, 0, w, h, (0, 0, 0), a=1)
        hit.save(os.path.join(OUT, "hit_%dx%d.png" % (w, h)))
    draw_panel().save(os.path.join(OUT, "panel.png"))
    draw_meters_panel().save(os.path.join(OUT, "meters_panel.png"))
    led_strip(IN_LEDS, IN_PITCH, IN_LED_W, IN_H, lambda i: GREEN if i < 36 else (AMBER if i < 45 else RED)).save(os.path.join(OUT, "in_strip.png"))
    hold_strip().save(os.path.join(OUT, "hold_strip.png"))
    led_strip(OI_LEDS, OI_PITCH, OI_LED_W, OI_H, lambda i: GREEN if i < 25 else (AMBER if i < 28 else RED)).save(os.path.join(OUT, "oi_strip.png"))
    led_strip(GC_LEDS, GC_PITCH, GC_LED_W, GC_H, lambda i: AMBER, centre=12).save(os.path.join(OUT, "gc_strip.png"))
    hist_strip().save(os.path.join(OUT, "hist_strip.png"))
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
