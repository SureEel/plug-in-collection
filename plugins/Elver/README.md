# Elver

A clean, simple, low-memory synthesizer for Akai MPC and Force, by Sure Eel.

Two sides, **A** and **B**, each a small complete synth: three oscillators, a filter, an amp envelope and a
filter envelope. **Blend** (or the XY pad) crossfades between them. A **mood randomizer** builds new sounds
in seven categories, and presets are organised the same way.

## Signal flow

```
            ┌ A: osc 1-3 (+unison, ring) ─ A filter ─ A amp env ┐
note ──────┤                                                     ├─ blend ─ drive ─ grit ─ volume ─ soft limit ─ out
            └ B: osc 1-3 (+unison, ring) ─ B filter ─ B amp env ┘
Series routing: both sides ─ A filter ─ B filter.
LFO 1 / LFO 2 → pitch, filter, amp or blend.   Drift: slow random pitch / filter wander per voice.
```

## Pages

| Page | What is there |
|---|---|
| MAIN | XY pad (X = Blend, Y = Pad Y; Pad Y To picks cutoff, morph, resonance or drive), filter cutoff / resonance A and B, Category + Randomize, Undo, Rand Amount, status, Volume |
| OSC A / OSC B | per oscillator: wave (sine, triangle, saw, square, noise), level, octave, semitone; unison copies, detune, stereo spread |
| TUNE | fine tuning and pulse width for all six oscillators |
| FILTER | type (24 dB LP, 12 dB LP, band-pass, high-pass, notch), cutoff, resonance, envelope amount, key tracking, routing (parallel / series), Blend and Pad Y knobs |
| ENV | amp and filter ADSR for each side (attack and decay up to 10 s, release up to 20 s) |
| MOD | LFO 1 and 2 (rate, depth, shape, destination), Drift, Drive, Grit (bit-crush), Ring, Volume, Output, Voices, Skin |
| PRESET | browse by category, pick a preset, Load; Rand Amount, Undo and the section locks |
| SAVE | type a name on the letter keys, choose the category, Confirm |

Every page shows Elver's own **CPU** load and the **voices** playing / allowed. Touch any control name or
meter to see what it does in the help box.

## Shape and drawing (1.1)

Each side has a **Shaper**: a single-cycle wave you build and then play by setting any of that side's
oscillators to the wave **Shape**.

- **Start From** picks one of 60 original library waves in 7 groups:
  - ANA: analog saws, squares and pulses
  - VOX: vowel formants
  - FM: bells and tines
  - ORG: drawbar organs
  - DIG: folds, bit steps and phase distortion
  - HRM: harmonic recipes
  - NSE: seeded noise spectra

  They are computed from recipes, so no sample files are involved.
- **DRAW page**: drag a finger across the pad to paint the wave. Fast strokes are filled in, so there are
  no gaps. **Draw Mix** blends the drawing with the start wave; your first stroke turns it up to 100 %.
  **Smooth / Linear / Steps** sets how the 64 drawn points are joined. **Copy Start** copies the start
  wave into the drawing so you can edit it, **Clear** flattens it, and **Undo** removes the last
  stroke (8 steps).
- **Shaping**:
  - **Bright**: tilts the harmonics.
  - **Odd/Even**: hollow ↔ organ-like.
  - **Formant**: a vocal resonance.
  - **Fold**: a wavefolder.
  - **Bend**: phase distortion.
  - **Sync**: hard-sync ratio.
  - **Mutate**: seeded variation; the same **Seed** always gives the same wave.
- The **SHAPE** page has a scope showing the cycle the oscillators play.

How it works: the wave is rebuilt as 11 band-limited tables (1024 down to 1 harmonic) on a background
thread whenever a setting moves, and crossfaded in over 20 ms, so a change never clicks and high notes
don't alias. A preset's shapes are built as it loads, so its first note is already right. Shape waves are
scaled to a saw's loudness. Playing a Shape oscillator costs the same CPU as a saw. Memory is about 0.6
MB for both sides.

Randomize sets the Shaper (and, in each mood, sometimes chooses Shape for an oscillator) but never
touches your drawing, Draw Mix or Smooth. Lock A / Lock B keep a side's Shaper too. The drawing and
every Shaper setting are saved in presets and projects. User presets saved by Elver 1.0 still load,
with the Shaper at its defaults.

## Randomize

Pick a **Category** (Ambient, Pads & Winds, Keys & Plucks, Leads & Basses, Lo-Fi Psych, Industrial,
Noise & Chaos), then tap **Randomize**. It fills both sides, the blend and the modulation with a sound in that
mood, and usually picks a fitting skin. **Rand Amount** below 100 % only nudges the current sound toward a new
one. The **locks** (A, B, Filter, Env, Mod, Skin) keep a section as it is. **Undo** goes back one step.
Randomize never changes Volume or Output.

## Presets

7 categories × 12 slots: 4 factory presets and 8 user slots each. In the MPC's preset list they appear as
"AMB Deep Tide", "LEAD Acid Fin", and so on. An empty user slot loads the plain starting sound.
To save, open SAVE, tap **name a preset** (the name starts as e.g. "Lofi 03" and the category as the one you last
randomized in), type with the keys (del, < and > move and edit, clear empties), choose **Save In** and tap
**confirm save**. User presets are stored next to the plugin folder (`.sureeel Elver user presets.bin`) and
survive restarts and reinstalls. A saved preset remembers its skin; factory presets leave the skin alone.

Volume, Output and the skin are global: loading a preset does not change them. Every setting and the
selected preset are saved with your project.

## Skins

Four colour schemes for the brushed-metal panel (MOD page, or let Randomize choose):
**Neon Rain** (teal steel, neon orange), **Capsule Red** (red-black, off-white), **Monolith** (grey steel,
deep blue) and **Derelict** (near-black green, pale green).

## Q-Links

On the SHAPE pages the columns follow the sections. On the DRAW pages the first column is Draw X, Draw Y and
Draw Mix, so you can also draw with two encoders: X picks the point, Y sets its height.

In Program mode the 16 Q-Links are: Blend, Pad Y, A Cutoff, A Reso, B Cutoff, B Reso, A Env Amt, B Env Amt,
Randomize, Rand Amount, Undo, LFO1 Rate, LFO1 Depth, Drift, Drive, Volume. On each page the Q-Link
columns follow the four on-screen sections.

## CPU and voices

Up to 8 voices within a per-instance budget. Each sounding side of a voice costs 1 unit plus 1 per running
oscillator copy (unison copies count). The Force gets 24 units and the MPC 48. The starting sound (one saw,
one side) plays 8 voices on both. Big two-sided unison patches play fewer voices instead of overloading the
CPU, and the voice readout shows the limit. Desktop measurements suggest about 10-25 % of a Force core at
the budget limit; check the CPU meter on your device.

## Robustness

Non-finite samples and parameter values are ignored. The engine resets on suspend / resume and after a
state reload, and a watchdog restarts it if notes are held but the output stays exactly zero. MIDI from a
newly connected device (controller bursts, all-notes-off, program changes) never leaves Elver silent.

## Building

```
python tools/build.py plugins/Elver
python plugins/Elver/skin_src/make_skin.py      # only after changing the skin (needs Pillow)
```
Tests (desktop): `plugins/Elver/test/elver_test.cpp` (17 checks: notes, stuck notes, every control,
XY pad, randomize / undo / locks, save / reload, CPU meter, renders, hostile host).

Requires MPC OS 3.10.1 or later with External plugins enabled
(Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support).

## Licence

Original code, MIT © 2026 Sure Eel. The filter is a state-variable design after Andy Simper's (Cytomic)
published technical papers; the oscillators use PolyBLEP anti-aliasing (public technique). The Sure Eel
logo is not covered by the MIT licence.
