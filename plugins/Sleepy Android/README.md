# Sleepy Android

A guitar effect for Akai MPC and Force standalone hardware, by sureeel: an envelope filter
("Android") and a capture-and-loop stutter ("Sleepy"), in either order or in parallel, with a
safety limiter, expression-pedal morphing and a patch randomizer. Needs MPC OS 3.10.1+ with
Preferences > Enhancements (Beta) > Enable Enhancements and External Plugin Support on.

## Pages

Every page is a rack of up to four sections; each section is one Q-Link bank (column), top to
bottom as on screen. A bottom strip holds two meters and the help window.

| Page | Sections | Meters |
|---|---|---|
| MAIN | **levels** Input Gain, Output, Mix, Bypass / **routing · pedal** Routing, Pedal, pedal status, preset name / **pedal assign** Assign, Set Heel, Set Toe, Clear / **save preset** Save Preset, Name Pos, Letter, Confirm | input level, limiter GR |
| FILTER | **envelope follower** Sensitivity, Attack, Release, Env Mode + envelope LED / **lfo** Rate, Depth, Shape, LFO To / **filter** Cutoff, Sweep, Resonance, Filter Type / **output** VCA, Filter Mix, Stereo, Filter On | envelope, filter position |
| SWEEP | **auto sweep** Auto Sweep, Sweep Dir, Sweep Range, speed now / **sweep speed** Sweep Rate (slowest), Sweep Rate Max (fastest) / **touch · filter** Env Touch, Env Touch Amt, Sweep Touch, Sweep Touch Amt | touch level, filter position |
| STUTTER | **stutter** Stutter On, Trigger + looping LED, Hold, Dry Mute / **mode** Stut Mode, Sync, stutter now / **lengths** Slice, Loop, Feedback, Stutter Mix | input level, limiter GR |
| RANDOM | **random range** Slice Min/Max, Loop Min/Max / **re-roll** Rand Width, Rand Rate (slowest), Rand Rate Max (fastest), stutter now / **touch · stutter** Stut Touch, Stut Touch Amt | touch level, input level |
| PATCH | **randomize patch** Randomize, Undo, Rand Amount, status / **locks** Lock Filter, Lock Stutter / **save preset** Save Preset, Confirm, Cancel, preset name / **name** Name Pos, Letter | input level, limiter GR |
Program Mode Q-Links: Pedal, Randomize, Undo, Save, Rand Amount, Sensitivity, Sweep, Resonance,
Cutoff, Feedback, Slice, Trigger, Env Touch Amt, Sweep Touch Amt, Stut Touch Amt, Sweep Rate.

Selectors show their choices as text on the buttons (the active one lit white) and the active
choice again beside the name. Help: touch a control's name or a meter; the help window explains it.

### Buttons

- **Action buttons** (Set Heel, Set Toe, Clear, Save Preset, Randomize, Undo) act on every tap. The
  MPC touch screen can only make on/off switches (its spring-back option leaves a button dead on
  the hardware), so a tap flips the switch and the plugin acts on each flip; it ignores a second
  flip within 0.35 s, so a footswitch's press and release, or a Q-Link turned through the middle,
  acts once. Each shows its result in a status line: pedal status ("Heel 10 % stored: Set Toe",
  "Resonance kept: 10 % > 90 %"), save / randomize status ("Randomized 12 settings", "Saved as
  Random 03").
- **Trigger** loops the stutter while it is on and stops when it is off (it works even with Stutter
  On off). On screen: tap on, tap off, lit while on. On a Q-Link or footswitch: hold. **Hold** is
  the latch for Q-Link / footswitch use. The looping LED is lit while the stutter records or loops.
- **Pedal**: Assign on, turn a knob, Set Heel, move the knob, Set Toe: the assignment is kept (no
  separate save step). Clear removes all.

### User presets and naming

**Save Preset** opens naming with an auto-name ("Random 01" after a randomize, else "User 01").
The touch screen has no keyboard, so naming is a letter picker: **Name Pos** picks the character
(shown in [brackets] in the preset name read-out), **Letter** changes it (A-Z, a-z, 0-9, space,
- _ . # & + !); both are knobs, so a Q-Link works. **Confirm** (or Save again) stores it, **Cancel**
drops it. The preset joins the preset list after the factory presets, in the next of 16 user slots
(overwriting the oldest when full). User presets are kept in `VST2 Plugins/.sureeel Sleepy Android
user presets.bin` on each device's drive (so each device has its own), beside the plugin folder,
so reinstalling does not erase them, and they come back after a restart. A preset holds every
sound setting (filter, sweep, touch, stutter, levels); pedal assignments stay with the project.

### Touch sensitivity

One shared touch reading: the input's short-window peak (10 ms rise, 200 ms fall) in dB, -50 dBFS
(soft) .. -6 dBFS (hard), smoothed over 150 ms so speeds glide instead of jumping (tested: under
0.1 % drift on a held note, at most 18 % change per 0.1 s on hard plucks). One log10 per 16 samples.
Each touch feature has its own switch and amount:

- **Env Touch / Env Touch Amt**: how much playing dynamics shape the envelope sweep. 100 % = soft
  notes barely move the filter, hard picks sweep it wide; lower = more even; off = every note
  sweeps alike.
- **Sweep Touch / Sweep Touch Amt**: the auto sweep's speed moves from Sweep Rate (soft) toward
  Sweep Rate Max (hard).
- **Stut Touch / Stut Touch Amt**: the stutter's re-roll speed moves from Rand Rate (soft) toward
  Rand Rate Max (hard).
## How it works

- **Android filter** (after the Mutronics Mutator): four-pole 24 dB/oct ladder (LP / BP / HP;
  zero-delay-feedback, measured 24.6 / 23.3 dB per octave). **Env** mode: a peak follower
  (Attack 1 ms-2 s, Release 5 ms-5 s) whose level, in dB plus the Sensitivity drive (0..+48 dB),
  is mapped over a 30 dB window with a progressive curve, so playing dynamics decide the sweep.
  Init patch, measured: a -30 dBFS pluck moves the cutoff 0.4 octave, -18 dBFS 2.3 octaves, a
  -6 dBFS pick attack the full 4.5. **Gate** mode: Sensitivity is a threshold (-6..-60 dB); above
  it the filter snaps fully open, below it fully closed, at the Attack / Release speeds (set them
  short for an instant snap). Cutoff is the resting point; Sweep -6..+6 octaves, centre zero (+ up,
  - down). LFO 0.1-100 Hz, Triangle / Square / Ramp Up / Ramp Down, Depth in octaves, to the filter,
  the volume or both, alone or with the envelope; Stereo inverts the right channel's LFO. VCA
  makes the envelope (or gate) also drive the volume. Resonance tops out at a strong, clear twang
  and stops short of self-oscillation; the feedback is softly saturated as well, so hard playing at
  full resonance cannot squeal (tested: impulse dies away, loud plucks stay at the -1 dBFS ceiling).
  Modulation runs every 16 samples.- **Auto sweep**: its own sweep of the cutoff, Sweep Range octaves above the resting Cutoff, Up /
  Down / Up-Down, 0.05-10 Hz; adds to the envelope and the LFO, or works alone.
- **Sleepy stutter**: captures a Slice of the input, then loops it for Loop; every pass writes
  `buffer*Feedback + input*(1-Feedback)` (100 % freezes the slice). Hold (latching) keeps cycling (each
  cycle captures a fresh slice). **Trigger is a momentary hold**: it loops while held - on screen,
  on a Q-Link turned past halfway, or a footswitch mapped to it - and stops on release; holding it
  runs the stutter even when Stutter On is off. A tap too short to hold plays one cycle. Random mode draws new lengths every cycle between the Min/Max knobs, pulled toward the Slice /
  Loop knobs by **Rand Width** (0 % = always the Slice / Loop lengths, 100 % = anywhere in Min..Max). **Rand Rate** sets how
  often new lengths are drawn (0.1-10 per second; between draws the last lengths repeat); a re-roll
  lands at the next slice boundary. Sync switches all length knobs between ms and note values at the
  MPC tempo. Dry Mute silences the dry signal while looping.
- **Routing**: Filt>Stut, Stut>Filt, or Parallel. Then Mix (whole plugin dry/wet) and Output.
- **Limiter**: sample-peak look-ahead limiter at -1 dBFS, always on (latency 64 samples, reported
  to the host). Bypass is bit-exact and time-aligned.
- **Pedal morph**: effects get no MIDI on the MPC, so map the expression pedal to the Pedal
  parameter on the MPC. Assign on, turn a knob, set heel, Set Heel, set toe, Set Toe, Save. Up to 8
  knobs; Clear removes all. Saved with projects and user presets (not part of factory presets).
- **Randomize / Undo**: Rand Amount 0..100 % (low nudges, 100 % rerolls), Lock Filter / Lock
  Stutter. Each parameter has a musical range (e.g. Resonance 10-85 %, Feedback 20-95 %). Never
  touches bypass, input gain, output, master mix, routing, the on/off/Hold/Trigger/Sync switches,
  or pedal-assigned knobs (their heel/toe stay valid). Undo restores the previous settings.
- **Push buttons** (Trigger, Randomize, Undo, Set Heel, Set Toe, Save, Clear): the screen button
  springs back when the finger lifts; the plugin acts on the press and never writes the button
  itself (earlier versions did, and the MPC ignored it, so taps were lost). A press shorter than one
  audio block is still caught. After Randomize / Undo the plugin asks the MPC to redraw the page.

Presets: Init, Quack Funk, Down Sweep, Gate Wah, Sleepy Glitch, Android Stutter, Frozen Pad,
Volume Swell, Stereo Wobble, Tremolo Gate, seven inspired by two players, and five extreme ones. They are our own
settings in the spirit of those sounds, not copies of anyone's rig or tone:

| Preset | Inspired by | What it is |
|---|---|---|
| RH Paranoid Wah | Radiohead, "Paranoid Android" | Greenwood reportedly ran his recorded Paranoid Android solos through a Mutronics Mutator (live: a DOD 440 envelope filter). Here: the Mutator's classic 24 dB low-pass, high resonance, 1 ms attack, opening ~3.8 octaves on a normal -18 dBFS guitar |
| RH Go To Sleep | Radiohead, "Go To Sleep" | After Greenwood's randomised-stutter Max/MSP patch on the outro: random tempo-synced chops (1/32..1/8) re-rolling quickly (faster when you dig in), replacing the dry guitar while they loop; no wah, as on the record |
| RH Paranoid Sleep | Radiohead, both | The random stutter into the Paranoid wah (Stut>Filt), every chop re-filtered |
| RH Kill Switch | Radiohead, "Paranoid Android" | The Paranoid wah plus a 12 Hz square volume chop, after the kill-switch stutter in the solo |
| AB Wild Envelope | Adrian Belew | Huge upward sweep, near-maximum resonance, instant attack |
| AB Animal Noises | Adrian Belew | Band-pass squawks sweeping down from a high resting point, fast LFO wobble on top; try bends and slides |
| AB Stutter Squawk | Adrian Belew | Short fixed stutter into the squawking filter; hold Trigger to loop |
| XT Laser Zap | extreme | Full-resonance band-pass diving 6 octaves from 8 kHz on every pick |
| XT Ring Tremor | extreme | 100 Hz square LFO on cutoff and volume, stereo: ring-mod-like |
| XT Gate Stab | extreme | Gate + VCA, instant on/off, full-depth down sweep: only hard hits speak |
| XT Chaos Glitch | extreme | Random stutter re-rolling 10 times a second over the widest ranges, near-frozen feedback, dry muted, into a 6-octave touch-speed auto sweep |
| XT Frozen Swarm | extreme | A 2 s slice frozen at 100 % feedback, wobbled by a 40 Hz stereo LFO |

No published settings exist for the originals; these are our own settings in the spirit of those
sounds, from what is documented about the gear, not copies.(The MPC's preset list shows names only; these descriptions live here.)

Look: the sureeel logo (skin_src/reference/) is the full background of every page, dimmed, with dark plates baked in under each control group so labels stay readable. White hairline frames, thin lowercase type, a concentric-ring envelope LED. Each page background is one static image, drawn once.

## Build and test

```
python tools/build.py "plugins/Sleepy Android" --target native
python "plugins/Sleepy Android/skin_src/make_skin.py"
python tools/build.py "plugins/Sleepy Android"
.sdk-tools\zigenv\Lib\site-packages\ziglang\zig.exe c++ -std=c++17 -O2 -I framework -o sleepy_test.exe "plugins/Sleepy Android/test/sleepy_test.cpp"
sleepy_test.exe "plugins/Sleepy Android/build/native/sleepy-android.dll"
```

Tested on the desktop (host simulator 20/20, functional tests 0 failures): filter sweep and
direction, bit-exact bypass, limiter at -1.00 dBFS with everything maxed, exact loop repeats,
Trigger hold / release / short tap, playing dynamics vs sweep, no self-oscillation, random note values at a 90 BPM host tempo, pedal heel/toe exact and saved,
randomizer ranges / locks / undo / nudge. Not yet verified on hardware: CPU on the Force, the
pedal mapping on the Key 37, and the touch page's look.

All DSP is original code; the SDK framework is MIT licensed. Button text is drawn with the Bahnschrift font (ships with Windows) into the button images by skin_src/make_skin.py when Pillow is installed; the rendered images are kept in skin_src/assets/buttons/.
