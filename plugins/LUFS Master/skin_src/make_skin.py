"""Builds LUFS Master's hand-written skin (plugins/LUFS Master/skins/) for the MPC touch screen.

The MASTER tab is a rack-mount mastering-limiter panel; the METERS tab, the knob/toggle
definitions, the overlays and the replacementStrings are produced by the SDK generator from
ui.json exactly as before, so they stay in step with the plugin's parameters.

Design rules (CPU on the MPC): one static, pre-drawn panel image; meters are film-strip Image
widgets (one pre-drawn frame per LED count, no drawing at run time); every other moving part
is a text label bound to a parameter. Flat colours and simple vertical gradients only: no
animation, blur or shadows. The plugin refreshes the meters at 10 Hz and the other numbers at
5 Hz, so the page never redraws faster than 10 Hz.

usage (from the SDK root, after one native build so build/params.json exists):
    python "plugins/LUFS Master/skin_src/make_skin.py"
then build as usual (build.py copies skins/ verbatim):
    python tools/build.py "plugins/LUFS Master"
"""
import os
import random
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

# Parameter indices (src/LUFSMaster.cpp ParamIndex)
P_CEILING, P_TARGET, P_CUSTOM, P_MODE, P_RESET, P_REFRESH = 0, 1, 2, 3, 4, 5
P_IN_MOM, P_IN_INT = 6, 8
P_OUT_MOM, P_OUT_ST, P_OUT_INT = 10, 11, 12
P_OUT_TP, P_GAIN, P_GR, P_GR_MAX, P_PRED = 15, 16, 17, 18, 19
P_IN_ST, P_IN_AVG, P_OUT_AVG, P_IN_TP = 7, 9, 13, 14
P_HIST, HIST_COLS = 20, 48                 # loudness history columns (5 s each, oldest first)
P_HELP, P_HELP2 = 68, 69                   # help item (set by the help buttons) and its mirror

# METERS page: loudness-over-time graph (must match the plugin: -48..0 LUFS, 1 LU per frame)
HX0, HPITCH, HCOL_W = 120, 22, 18
HY0, HPX_PER_LU = 64, 5
HPLOT_H = 48 * HPX_PER_LU                  # 240 px

# Meter geometry (must match readoutNorm() in the plugin: -48..0 LUFS, 0..12 dB)
MOM_LEDS, MOM_PITCH, MOM_LED_W, MOM_H = 48, 23, 20, 40          # 1 LU per LED
MOM_X, MOM_Y = 88, 110
GR_LEDS, GR_PITCH, GR_LED_W, GR_H = 24, 11, 9, 28                 # 0.5 dB per LED
GR_X, GR_Y = 936, 268

# Colours (r, g, b)
INK = "ffe4e6e9"
MUTED = "ff8f9399"
LED_GREEN_TXT = "ff6ff59a"
LED_AMBER_TXT = "ffffc04d"
LED_ORANGE_TXT = "ffffa53a"


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

    def round_corners(self, r):
        """Make the corners transparent (alpha canvases only)."""
        for yy in range(r):
            cut = r - int((r * r - (r - yy - 0.5) ** 2) ** 0.5 + 0.5)
            for y in (yy, self.h - 1 - yy):
                for x in list(range(cut)) + list(range(self.w - cut, self.w)):
                    self.rows[y][x * 4 + 3] = 0

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
    """Film strip: frames stacked vertically."""
    out = Canvas(frames[0].w, frames[0].h * len(frames), alpha=frames[0].n == 4)
    out.rows = [row for fr in frames for row in fr.rows]
    out.h = len(out.rows)
    return out


# ------------------------------------------------------------------------------------------
# Artwork
# ------------------------------------------------------------------------------------------
def recess(c, x, y, w, h):
    """A meter window cut into the panel: flat dark fill with a 1 px bevel (no shadow)."""
    c.fill(x, y, w, h, (10, 11, 12))
    c.fill(x, y, w, 1, (4, 4, 5))
    c.fill(x, y, 1, h, (6, 6, 7))
    c.fill(x, y + h - 1, w, 1, (64, 67, 71))
    c.fill(x + w - 1, y, 1, h, (52, 55, 59))


def groove(c, y, x0=64, x1=1216):
    c.fill(x0, y, x1 - x0, 1, (16, 17, 18))
    c.fill(x0, y + 1, x1 - x0, 1, (60, 62, 66))


def screw(c, cx, cy):
    c.disc(cx, cy, 10, (18, 18, 20))
    c.disc(cx, cy, 8, (86, 89, 94))
    c.fill(cx - 6, cy - 1, 13, 3, (30, 31, 34))


def draw_meters_panel():
    """METERS page art: same brushed panel, the history graph window with its LU grid."""
    c = brushed_base()
    groove(c, 346)
    recess(c, 64, 48, 1152, 290)
    for v in range(-48, 1, 6):                                   # horizontal grid every 6 LU
        y = HY0 + HPLOT_H - (v + 48) * HPX_PER_LU
        c.fill(HX0 - 6, y, HIST_COLS * HPITCH + 6, 1, (44, 46, 50) if v > -48 else (90, 93, 98))
    return c


def hist_strip():
    """One vertical loudness bar per frame (frame k = k LU above -48), transparent above the bar
    so the grid shows through. Same colour zones as the momentary meter."""
    frames = []
    for lit in range(49):
        f = Canvas(HCOL_W, HPLOT_H, (0, 0, 0), alpha=True)
        f.fill(0, 0, HCOL_W, HPLOT_H, (0, 0, 0), a=0)
        for i in range(lit):                                     # LU step i, from the bottom
            top = -48 + i + 1
            col = (61, 220, 106) if top <= -18 else ((255, 176, 46) if top <= -9 else (255, 59, 48))
            y = HPLOT_H - (i + 1) * HPX_PER_LU
            f.fill(0, y, HCOL_W, HPX_PER_LU, col)
        if lit:
            f.fill(0, HPLOT_H - lit * HPX_PER_LU, HCOL_W, 1, (255, 255, 255))   # bright cap line
        frames.append(f)
    return stack(frames)


def brushed_base():
    rnd = random.Random(1770)
    W, H = 1280, 628
    c = Canvas(W, H)
    # Brushed metal: long horizontal streaks (runs of +-1 around a row level), a slow vertical
    # gradient, and row-to-row variation. Long runs keep the PNG small.
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
    # Rack ears with screws
    for x0 in (0, W - 44):
        c.vgrad(x0, 0, 44, H, (30, 31, 34), (24, 25, 27))
        c.fill(x0 + (43 if x0 == 0 else 0), 0, 1, H, (62, 64, 68))
        for cy in (44, H - 44):
            screw(c, x0 + 22, cy)
    return c


def draw_panel():
    c = brushed_base()
    # Section grooves
    groove(c, 60)
    groove(c, 206)
    groove(c, 426)
    # Momentary meter window + scale ticks
    recess(c, 64, 100, 1152, 60)
    for v in range(-48, 1, 6):
        x = MOM_X + (v + 48) * MOM_PITCH - 1
        c.fill(x, 164, 2, 7, (128, 132, 138))
    for v in range(-48, 1, 1):
        if v % 6:
            c.fill(MOM_X + (v + 48) * MOM_PITCH - 1, 164, 1, 3, (84, 87, 92))
    # Integrated and target windows
    recess(c, 64, 250, 470, 128)
    recess(c, 560, 250, 336, 128)
    c.fill(568, 331, 320, 1, (40, 42, 45))                   # divider above the target strip
    # Gain-reduction window + ticks (0, 3, 6, 9, 12 dB)
    recess(c, 920, 250, 296, 64)
    for db in range(0, 13, 3):
        c.fill(GR_X + db * 2 * GR_PITCH - 1, 316, 2, 6, (128, 132, 138))
    # Help window (bottom right, between the knobs and the Reset/Refresh buttons)
    recess(c, HELP_X, HELP_Y, HELP_W, HELP_H)
    return c


def mom_strip():
    frames = []
    for lit in range(MOM_LEDS + 1):
        f = Canvas(MOM_X * 0 + MOM_LEDS * MOM_PITCH, MOM_H, (10, 11, 12))
        for i in range(MOM_LEDS):
            top = -48 + i + 1                               # this LED lights at >= top - 1 LU
            on_col = (61, 220, 106) if top <= -18 else ((255, 176, 46) if top <= -9 else (255, 59, 48))
            off_col = tuple(int(v * 0.16) + 6 for v in on_col)
            x = i * MOM_PITCH + 1
            if i < lit:
                f.vgrad(x, 2, MOM_LED_W, MOM_H - 4, tuple(min(255, v + 40) for v in on_col), on_col)
            else:
                f.fill(x, 2, MOM_LED_W, MOM_H - 4, off_col)
        frames.append(f)
    return stack(frames)


def gr_strip():
    frames = []
    for lit in range(GR_LEDS + 1):
        f = Canvas(GR_LEDS * GR_PITCH, GR_H, (10, 11, 12))
        for j in range(GR_LEDS):
            on_col = (255, 176, 46) if j < 12 else (255, 59, 48)
            off_col = tuple(int(v * 0.16) + 6 for v in on_col)
            x = j * GR_PITCH + 1
            if j < lit:
                f.vgrad(x, 2, GR_LED_W, GR_H - 4, tuple(min(255, v + 40) for v in on_col), on_col)
            else:
                f.fill(x, 2, GR_LED_W, GR_H - 4, off_col)
        frames.append(f)
    return stack(frames)


def switch_cap(w, h, lit, led_rgb, led_w, led_h, led_y):
    """A hardware push-switch cap: dark rounded square, simple vertical gradient, an LED bar."""
    c = Canvas(w, h, (0, 0, 0), alpha=True)
    c.fill(0, 0, w, h, (8, 8, 9))                                        # 1 px rim
    if lit:
        c.vgrad(1, 1, w - 2, h - 2, (70, 73, 78), (50, 52, 56))
    else:
        c.vgrad(1, 1, w - 2, h - 2, (52, 54, 58), (34, 36, 39))
    c.fill(1, 1, w - 2, 1, (96, 99, 104) if lit else (72, 75, 80))       # top edge highlight
    led = led_rgb if lit else tuple(int(v * 0.28) + 10 for v in led_rgb)
    c.fill((w - led_w) // 2, led_y, led_w, led_h, led)
    c.round_corners(6)
    return c


# ------------------------------------------------------------------------------------------
# TUI pieces
# ------------------------------------------------------------------------------------------
def text_style(height, colour, font="Roboto", style="Regular", just="left verticallyCentred", case="Original"):
    return obj(("version", 1),
               ("font", obj(("version", 1), ("name", font), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def label(kind, handle, ts, text=None):
    # The MPC only draws a label whose handle resolves to a real value. "Invalid", and "Data" at
    # page level (it only means something inside a component), draw nothing. Static text is
    # therefore bound to a real parameter (Ceiling); a Text label shows only its fixed text.
    if kind == "Text":
        handle = "Parameter %d" % P_CEILING
    pairs = [("version", 1), ("textStyle", ts), ("type", kind), ("handleName", handle)]
    if text is not None:
        pairs.append(("text", text))
    return Obj(pairs)


def lab(name, bounds, kind, handle, ts, text=None, invalidating=None):
    return G.entry(name, "Label", label(kind, handle, ts, text), bounds, "No", "Show", "Always", invalidating=invalidating)


def caption(text, bounds, height=18, colour=MUTED, just="left verticallyCentred"):
    """Static caption. Written in capitals by hand so units keep their case (dB, dBTP)."""
    return lab("cap " + text, bounds, "Text", "Invalid", text_style(height, colour, "Roboto", "Medium", just, "Original"), text)


HIT_SIZES = set()


def help_trigger(item, bounds, invalidating=None):
    """Invisible radio button: touching it selects help item `item`. Its image is exactly the size
    of the touch area (buttons are drawn at native size and may only take touches on their
    image) and almost fully transparent (alpha 1/255), so it is invisible but still hit-testable."""
    w, h = (int(v) for v in bounds.split()[2:4])
    HIT_SIZES.add((w, h))
    img = "hit_%dx%d.png" % (w, h)
    # A radio button on the plugin's Help parameter: the same mechanism as the Mode buttons.
    data = obj(("version", 2), ("onImage", img), ("offImage", img), ("buttonId", item),
               ("numButtonsInGroup", HELP_N), ("handleName", "Parameter %d" % P_HELP), ("gestureBehaviour", "Instant"))
    return G.entry("help %d" % item, "Button", data, bounds, "No", "Show", "Always", invalidating=invalidating)


def help_window():
    """Two value labels whose text the plugin supplies (Help = line 1, Help 2 = line 2): the same
    mechanism as the readouts. They redraw only when the touched item changes."""
    ts = text_style(17, INK, "Roboto", "Regular", "left verticallyCentred")
    return [caption("HELP", "%d %d %d 24" % (HELP_X + 16, HELP_Y + 8, HELP_W - 32), 15),
            lab("help line 1", "%d %d %d 30" % (HELP_X + 16, HELP_Y + 54, HELP_W - 32), "Value", "Parameter %d" % P_HELP, ts),
            lab("help line 2", "%d %d %d 30" % (HELP_X + 16, HELP_Y + 86, HELP_W - 32), "Value", "Parameter %d" % P_HELP2, ts)]


def knob_compact_component(accent):
    """A compact knob: small film-strip knob, name (with unit) above the value, 260 x 84."""
    s = KNOB_SIZE
    kids = [G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (KNOB_W, KNOB_H), "No", "Show", "WhenFocussed"),
            G.entry("knob", "Knob", G.knob_filmstrip_data("knob_small_uni.png", 127), "8 %d %d %d" % ((KNOB_H - s) // 2, s, s),
                    "No", "Show", "Always"),
            lab("name", "%d 6 %d 32" % (s + 20, KNOB_W - s - 24), "Name", "Data",
                text_style(18, MUTED, "Roboto", "Medium", "left verticallyCentred", "Upper Case")),
            lab("value", "%d 40 %d 38" % (s + 20, KNOB_W - s - 24), "Value", "Data",
                text_style(26, INK, "Roboto", "Bold", "left verticallyCentred"))]
    return G.component_definition(G.control_actions("knob overlay"), kids, "00000000")


def image(name, file, bounds, frames=None, handle=None):
    if frames:
        data = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", file),
                   ("numFrames", frames), ("handleName", handle))
    else:
        data = obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", file))
    return G.entry(name, "Image", data, bounds, "No", "Show", "Always")


def slot(name, comp, bounds, param, invalidating=None):
    return G.entry(name, comp, obj(("version", 1), ("handleName", "Data")), bounds, "Yes", "Hide", "Always",
                   remap=[("Data", "Parameter %d" % param)], invalidating=invalidating)


def button(name, on_img, off_img, bid, n, bounds):
    return G.entry(name, "Button", G.button_data(on_img, off_img, bid, n), bounds, "No", "Show", "Always")


MODE_W, MODE_H, MODE_GAP = 140, 72, 12
MODES = [("Bypass", (236, 236, 236)), ("Measure", (61, 220, 106)), ("Locked", (255, 59, 48))]
PUSH_W, PUSH_H, PUSH_X = 104, 52, 1112
PRESETS = ["STREAMING", "APPLE MUSIC / PODCASTS", "EBU R128 BROADCAST", "ATSC A/85 BROADCAST", "CUSTOM TARGET"]
KNOB_X, KNOB_W, KNOB_H, KNOB_SIZE = 524, 260, 84, 56          # compact Ceiling / Custom knobs
CEIL_Y, CUSTOM_Y = 444, 532

# Help window. Touching a meter, or the label of a control, sets a UI-only value (a Local
# handle) through an invisible radio button; the help window shows the two text lines whose
# visibility condition matches it. Static labels only: the window redraws only when the
# touched item changes. Touching the window itself brings back the default hint.
HELP_X, HELP_Y, HELP_W, HELP_H = 796, 440, 304, 176
HELP_HANDLE = "Local/16/help"
HELP_N = 16
HELP = [  # id: (line 1, line 2)
    ("Touch a meter or a label", "to see what it does."),
    ("Loudness of the last 0.4 s", "at the output, in LUFS."),
    ("Loudness of the last 3 s", "at the output, in LUFS."),
    ("Gated average loudness of the", "output since Refresh, LUFS."),
    ("Loudness to aim for. Double-", "tap the box to pick a preset."),
    ("How hard the limiter is", "working right now, in dB."),
    ("Most gain reduction since", "Refresh, in dB."),
    ("Loudness of the input stored", "while in Measure, in LUFS."),
    ("Gain before the limiter:", "leveler or locked offset, dB."),
    ("Expected loudness of the", "bounce once Locked, in LUFS."),
    ("Highest true peak at the", "output since Refresh, dBTP."),
    ("Bypass: off. Measure: learn", "and ride. Locked: fixed gain."),
    ("Highest true peak the limiter", "lets through, in dBTP."),
    ("Your own target loudness,", "used when Target is Custom."),
    ("Reset: clear measurement+lock.", "Refresh: clear readouts only."),
]


def mode_component(accent):
    w = 3 * MODE_W + 2 * MODE_GAP
    kids = [G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (w, MODE_H + 36), "No", "Show", "WhenFocussed")]
    for i, (name, _) in enumerate(MODES):
        x = i * (MODE_W + MODE_GAP)
        kids.append(button("mode %d" % i, "mode%d_on.png" % i, "mode%d_off.png" % i, i, 3, "%d 18 %d %d" % (x, MODE_W, MODE_H)))
        kids.append(lab("mode %d caption" % i, "%d 40 %d 44" % (x, MODE_W), "Text", "Invalid",
                        text_style(22, INK, "Roboto", "Bold", "horizontallyCentred verticallyCentred", "Upper Case"), name))
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


def push_component(accent):
    kids = [G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (PUSH_W, PUSH_H), "No", "Show", "WhenFocussed"),
            button("push", "push_on.png", "push_off.png", 1, 1, "0 0 %d %d" % (PUSH_W, PUSH_H)),
            lab("push caption", "0 16 %d 32" % PUSH_W, "Name", "Data",
                text_style(18, INK, "Roboto", "Bold", "horizontallyCentred verticallyCentred", "Upper Case"))]
    return G.component_definition([G.action("Mouse Down", "Q-Link", "Data", "")], kids, "00000000")


TARGET_W, TARGET_H = 336, 44


def target_component(accent):
    """Target strip: the preset value and name in one value label (no conditional visibility).
    Double-tap or Enter opens the target menu."""
    kids = [G.entry("focus", "Focus", G.focus_data(accent), "0 0 %d %d" % (TARGET_W, TARGET_H), "No", "Show", "WhenFocussed"),
            lab("target caption", "14 0 90 %d" % TARGET_H, "Text", "Invalid",
                text_style(15, MUTED, "Roboto", "Medium", "left verticallyCentred"), "TARGET"),
            lab("target value", "92 0 %d %d" % (TARGET_W - 104, TARGET_H), "Value", "Data",
                text_style(22, LED_AMBER_TXT, "Roboto", "Bold", "left verticallyCentred"))]
    acts = [G.action("Mouse Down", "Q-Link", "Data", ""),
            G.action("Double Click", "Show Overlay", "Data", "menu overlay"),
            G.action("Enter Pressed", "Show Overlay", "Data", "menu overlay")]
    return G.component_definition(acts, kids, "00000000")


def master_tab():
    big = text_style(100, LED_GREEN_TXT, "Roboto", "Bold", "horizontallyCentred verticallyCentred")
    num = text_style(26, LED_GREEN_TXT, "Roboto", "Bold", "right verticallyCentred")
    small_val = text_style(22, INK, "Roboto", "Medium", "left verticallyCentred")
    kids = [image("panel", "panel.png", "0 0 1280 628"),
            # header
            lab("title", "64 12 330 44", "Text", "Invalid", text_style(34, INK, "Nexa", "Bold", "left verticallyCentred", "Upper Case"), "LUFS Master"),
            caption("True-peak loudness limiter", "330 16 420 40", 18),
            lab("brand", "900 12 316 44", "Text", "Invalid", text_style(24, MUTED, "Nexa", "Bold", "right verticallyCentred"), "sureeel"),
            # momentary meter (bar + number) and short-term number
            caption("OUTPUT MOMENTARY LOUDNESS  ·  LUFS", "64 68 390 30", 17),
            lab("mom value", "456 66 130 32", "Value", "Parameter %d" % P_OUT_MOM,
                text_style(26, LED_GREEN_TXT, "Roboto", "Bold", "left verticallyCentred")),
            caption("OUTPUT SHORT-TERM  ·  LUFS", "760 68 320 30", 17),
            lab("st value", "1080 66 136 32", "Value", "Parameter %d" % P_OUT_ST, num),
            image("mom strip", "mom_strip.png", "%d %d %d %d" % (MOM_X, MOM_Y, MOM_LEDS * MOM_PITCH, MOM_H), MOM_LEDS + 1, "Parameter %d" % P_OUT_MOM)]
    for v in range(-48, 1, 6):
        x = MOM_X + (v + 48) * MOM_PITCH
        kids.append(lab("scale %d" % v, "%d 172 60 24" % (x - 30, ), "Text", "Invalid",
                        text_style(17, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"), str(v)))
    kids += [
        # integrated
        caption("OUTPUT INTEGRATED LOUDNESS  ·  LUFS", "64 218 470 30"),
        lab("integrated", "64 250 470 128", "Value", "Parameter %d" % P_OUT_INT, big),
        # second box: LUFS over time (running integrated loudness of the measurement) + target strip
        caption("LUFS OVER TIME  ·  RUNNING INTEGRATED", "560 218 336 30", 16),
        lab("lufs over time", "560 252 336 78", "Value", "Parameter %d" % P_IN_INT,
            text_style(60, LED_AMBER_TXT, "Roboto", "Bold", "horizontallyCentred verticallyCentred")),
        slot("target", "targetSelect", "560 334 %d %d" % (TARGET_W, TARGET_H), P_TARGET),
        # gain reduction
        caption("LIMITER GAIN REDUCTION  ·  dB", "920 218 296 30", 17),
        image("gr strip", "gr_strip.png", "%d %d %d %d" % (GR_X, GR_Y, GR_LEDS * GR_PITCH, GR_H), GR_LEDS + 1, "Parameter %d" % P_GR)]
    for db in range(0, 13, 3):
        x = GR_X + db * 2 * GR_PITCH
        kids.append(lab("gr scale %d" % db, "%d 322 40 22" % (x - 20), "Text", "Invalid",
                        text_style(16, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"), str(db)))
    kids += [caption("LIMITER GR MAX  ·  dB", "920 352 200 30", 16),
             lab("gr max", "1120 352 96 30", "Value", "Parameter %d" % P_GR_MAX,
                 text_style(26, LED_ORANGE_TXT, "Roboto", "Bold", "right verticallyCentred"))]
    # small readouts under the integrated / target windows: caption (with unit) over value
    stats = [("INPUT TRUE PEAK  ·  dBTP", P_IN_TP), ("GAIN BEFORE LIMITER  ·  dB", P_GAIN),
             ("PREDICTED OUTPUT  ·  LUFS", P_PRED), ("OUTPUT TRUE PEAK  ·  dBTP", P_OUT_TP)]
    for i, (cap, p) in enumerate(stats):
        x = 64 + i * 212
        kids.append(caption(cap, "%d 382 206 18" % x, 14))
        kids.append(lab("stat %d" % i, "%d 400 206 24" % x, "Value", "Parameter %d" % p, small_val))
    kids += [
        # controls
        caption("MODE", "64 440 444 26"),
        slot("mode", "modeSwitch", "64 470 %d %d" % (3 * MODE_W + 2 * MODE_GAP, MODE_H + 36), P_MODE),
        slot("ceiling", "knobCompact", "%d %d %d %d" % (KNOB_X, CEIL_Y, KNOB_W, KNOB_H), P_CEILING),
        slot("custom", "knobCompact", "%d %d %d %d" % (KNOB_X, CUSTOM_Y, KNOB_W, KNOB_H), P_CUSTOM),
        caption("RESET / REFRESH", "%d 440 %d 26" % (PUSH_X, PUSH_W), 13),
        slot("reset", "pushButton", "%d 470 %d %d" % (PUSH_X, PUSH_W, PUSH_H), P_RESET),
        slot("refresh", "pushButton", "%d 532 %d %d" % (PUSH_X, PUSH_W, PUSH_H), P_REFRESH)]
    kids += help_window()
    # Help triggers, last so they sit on top. Meters: the whole meter. Controls: their label only,
    # so touching the knob or switch itself still just operates it.
    name_x = KNOB_X + KNOB_SIZE + 20
    kids += [help_trigger(0, "%d %d %d %d" % (HELP_X, HELP_Y, HELP_W, HELP_H)),
             help_trigger(1, "64 66 690 32"), help_trigger(1, "64 100 1152 96"),
             help_trigger(2, "760 66 456 32"),
             help_trigger(3, "64 218 470 160"),
             help_trigger(15, "560 218 336 112"),
             help_trigger(4, "560 334 90 44"),
             help_trigger(5, "920 218 296 126"),
             help_trigger(6, "920 350 296 32"),
             help_trigger(11, "64 440 444 26"),
             help_trigger(12, "%d %d %d 34" % (name_x, CEIL_Y + 4, KNOB_X + KNOB_W - name_x)),
             help_trigger(13, "%d %d %d 34" % (name_x, CUSTOM_Y + 4, KNOB_X + KNOB_W - name_x)),
             help_trigger(14, "%d 440 %d 26" % (PUSH_X, PUSH_W))]
    for i in range(4):
        kids.append(help_trigger(7 + i, "%d 382 206 44" % (64 + i * 212)))
    return G.component_definition([], kids, "ff26282b")


def meters_tab():
    """METERS page: input loudness over time (Measure mode) and every meter as a labelled number."""
    muted14 = text_style(14, MUTED, "Roboto", "Medium", "right verticallyCentred")
    kids = [image("panel", "meters_panel.png", "0 0 1280 628"),
            caption("INPUT LOUDNESS OVER TIME  ·  5 s AVERAGES  ·  LUFS", "64 12 760 30", 18),
            caption("TARGET  ·  LUFS", "900 12 180 30", 16),
            lab("target", "1080 10 136 34", "Value", "Parameter %d" % P_TARGET,
                text_style(24, LED_AMBER_TXT, "Roboto", "Bold", "right verticallyCentred"))]
    for v in range(-48, 1, 6):                                   # y axis
        y = HY0 + HPLOT_H - (v + 48) * HPX_PER_LU
        kids.append(lab("hist axis %d" % v, "64 %d 46 18" % (y - 9), "Text", "Invalid", muted14, str(v)))
    for i in range(HIST_COLS):                                   # the bars
        kids.append(image("hist %d" % i, "hist_strip.png", "%d %d %d %d" % (HX0 + i * HPITCH, HY0, HCOL_W, HPLOT_H),
                          49, "Parameter %d" % (P_HIST + i)))
    x_end = HX0 + HIST_COLS * HPITCH
    small = text_style(15, MUTED, "Roboto", "Regular", "left verticallyCentred")
    kids += [lab("x old", "%d 310 120 22" % HX0, "Text", "Invalid", small, "-4 min"),
             lab("x note", "%d 310 600 22" % (HX0 + 228), "Text", "Invalid",
                 text_style(15, MUTED, "Roboto", "Regular", "horizontallyCentred verticallyCentred"),
                 "records while Mode = Measure  ·  Reset clears it"),
             lab("x now", "%d 310 120 22" % (x_end - 120), "Text", "Invalid",
                 text_style(15, MUTED, "Roboto", "Regular", "right verticallyCentred"), "now")]
    rows = [("INPUT", [("MOMENTARY  ·  LUFS", P_IN_MOM), ("SHORT-TERM  ·  LUFS", P_IN_ST), ("INTEGRATED  ·  LUFS", P_IN_INT),
                       ("AVERAGE, UNGATED  ·  LUFS", P_IN_AVG), ("TRUE PEAK MAX  ·  dBTP", P_IN_TP)]),
            ("OUTPUT", [("MOMENTARY  ·  LUFS", P_OUT_MOM), ("SHORT-TERM  ·  LUFS", P_OUT_ST), ("INTEGRATED  ·  LUFS", P_OUT_INT),
                        ("AVERAGE, UNGATED  ·  LUFS", P_OUT_AVG), ("TRUE PEAK MAX  ·  dBTP", P_OUT_TP)]),
            ("LIMITER", [("GAIN REDUCTION  ·  dB", P_GR), ("GAIN REDUCTION MAX  ·  dB", P_GR_MAX),
                         ("GAIN BEFORE LIMITER  ·  dB", P_GAIN), ("PREDICTED OUTPUT  ·  LUFS", P_PRED),
                         ("CEILING  ·  dBTP", P_CEILING)])]
    for r, (group, cells) in enumerate(rows):
        y = 358 + r * 82
        kids.append(lab("group %s" % group, "64 %d 124 34" % (y + 20), "Text", "Invalid",
                        text_style(20, INK, "Roboto", "Bold", "left verticallyCentred"), group))
        for j, (cap, p) in enumerate(cells):
            x = 196 + j * 204
            kids.append(caption(cap, "%d %d 198 20" % (x, y), 14))
            kids.append(lab("%s %d" % (group, j), "%d %d 198 34" % (x, y + 20), "Value", "Parameter %d" % p,
                            text_style(28, LED_GREEN_TXT if group != "LIMITER" else LED_AMBER_TXT, "Roboto", "Bold",
                                       "left verticallyCentred")))
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
    pages, style, knob_image, frames, kit = G.load_ui(os.path.join(PROJECT, "ui.json"), G.ParamTable(params))
    kit_dir, kit_assets = G.kit_dir_for(kit, build, None)
    tui = G.build_tui(pages, params, style, knob_image, frames, kit_assets)
    qlinks = G.build_qlinks(pages, params)

    page = find(tui, "pageData")
    defs = find(find(page, "componentDefinitions"), "localComponentDefinitions")
    for name, comp in (("modeSwitch", mode_component(style["accent"])), ("pushButton", push_component(style["accent"])),
                       ("targetSelect", target_component(style["accent"])),
                       ("knobCompact", knob_compact_component(style["accent"]))):
        defs.append(obj(("key", name), ("value", comp)))
    tabs = find(page, "tabs")
    assert find(tabs[0], "tabName") == "MASTER"
    tabs[0] = obj(("version", 1), ("tabName", "MASTER"), ("fnKeyIndex", 0), ("fnKeySubIndex", 0),
                  ("componentDefinition", master_tab()), ("qlinkBoundsData", []))
    assert find(tabs[1], "tabName") == "METERS"
    tabs[1] = obj(("version", 1), ("tabName", "METERS"), ("fnKeyIndex", 1), ("fnKeySubIndex", 0),
                  ("componentDefinition", meters_tab()), ("qlinkBoundsData", []))

    # Q-Links on the MASTER tab: top row Mode, Target, Ceiling, Custom; next row Reset, Refresh.
    qmap = find(find(qlinks, "Screen Mode Q-Links"), "map")
    for i, (k, v) in enumerate(qmap[1]):                         # METERS: display only, no knobs
        if k == "Q-Links":
            qmap[1][i] = (k, Obj(("Q-Link %d" % q, -1) for q in range(1, 17)))
    entry0 = qmap[0]
    assign = {13: P_MODE, 14: P_TARGET, 15: P_CEILING, 16: P_CUSTOM, 9: P_RESET, 10: P_REFRESH}
    for i, (k, v) in enumerate(entry0):
        if k == "Bank Direction":
            entry0[i] = (k, "RowTopDown")
        if k == "Q-Links":
            entry0[i] = (k, Obj(("Q-Link %d" % q, assign.get(q, -1)) for q in range(1, 17)))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    own = {"panel.png", "mom_strip.png", "gr_strip.png", "push_on.png", "push_off.png",
           "meters_panel.png", "hist_strip.png"} | \
          {"hit_%dx%d.png" % s for s in HIT_SIZES} | \
          {"mode%d_%s.png" % (i, s) for i in range(3) for s in ("on", "off")}
    for img in sorted(G.referenced_images(tui, set()) - own):
        shutil.copy2(os.path.join(kit_dir, img), os.path.join(OUT, img))
    for w, h in sorted(HIT_SIZES):                              # invisible help-trigger buttons
        hit = Canvas(w, h, alpha=True)
        hit.fill(0, 0, w, h, (0, 0, 0), a=1)
        hit.save(os.path.join(OUT, "hit_%dx%d.png" % (w, h)))
    draw_panel().save(os.path.join(OUT, "panel.png"))
    draw_meters_panel().save(os.path.join(OUT, "meters_panel.png"))
    hist_strip().save(os.path.join(OUT, "hist_strip.png"))
    mom_strip().save(os.path.join(OUT, "mom_strip.png"))
    gr_strip().save(os.path.join(OUT, "gr_strip.png"))
    for i, (_, led) in enumerate(MODES):
        switch_cap(MODE_W, MODE_H, True, led, 34, 6, 10).save(os.path.join(OUT, "mode%d_on.png" % i))
        switch_cap(MODE_W, MODE_H, False, led, 34, 6, 10).save(os.path.join(OUT, "mode%d_off.png" % i))
    switch_cap(PUSH_W, PUSH_H, True, (255, 176, 46), 26, 5, 7).save(os.path.join(OUT, "push_on.png"))
    switch_cap(PUSH_W, PUSH_H, False, (255, 176, 46), 26, 5, 7).save(os.path.join(OUT, "push_off.png"))
    with open(os.path.join(OUT, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(OUT, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    total = sum(os.path.getsize(os.path.join(OUT, n)) for n in os.listdir(OUT))
    print("make_skin: wrote %s (%d files, %.0f KB)" % (OUT, len(os.listdir(OUT)), total / 1024))


if __name__ == "__main__":
    main()
