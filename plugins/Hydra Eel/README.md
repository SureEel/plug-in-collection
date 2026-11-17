# Hydra Eel

A spaghetti western guitar rig for Akai MPC and Force standalone hardware, by Sure Eel
(manufacturer **sureeel** on the device): twangy tremolo, spring reverb and gritty fuzz, in the
spirit of Morricone, plus compressors for funk and jazz. Needs MPC OS / Force OS 3.10.1+ with
**Preferences > Enhancements (Beta) > Enable Enhancements** and **External Plugin Support** on.

## Signal chain

Input > **Drive** > **Mod** > **Delay** > **Reverb** > Mono/Stereo > Output > -1 dBFS limiter.
Each section has its own On switch (it crossfades, no clicks, and costs nothing when off);
**Bypass** passes the input through untouched (bit-exact, time-aligned).

## Pages and controls

Touch any control's name or a meter to see an explanation in the help window. Each page's
sections are its Q-Link banks (top to bottom as on screen).

| Page | Controls |
|---|---|
| MAIN | Input, Output (global: presets, randomize and undo never change them), Bypass, Stereo (mono / stereo); Randomize, Undo, Rand Amount, status; Save Preset, Confirm, Cancel, preset name; Name Pos, Letter |
| DRIVE | Drive On; **Drive Type**, cleanest to dirtiest: opto (LA-2A style: slow, transparent), smooth (Ross style), fet (1176 style: quick, a little bite), punch (Dyna Comp style: clicky), germanium (Fuzz Face style), gated (Tone Bender style), sustain (Big Muff style), octave (Octavia-style octave-up fuzz); **Amount** (compression or gain), **Tone**, **Level**, **Gate Bias** (compressors: a noise gate; fuzzes: starve, on a smooth curve: the low half adds only a little sag, the sputtering, splatty sound arrives near the top; quiet notes are never chopped dead); **Drive Mix** (dry blend: parallel compression, or blended fuzz) |
| MOD | Mod On; **Rate Mode** Hz / Sync (or hold the Rate knob at its end, see below); **Mod Mode**: trem / vib or rotary / phaser; **Blend** (trem: tremolo > both > vibrato; rotary: rotary > both > phaser); **Wave** (sine, triangle, square, for trem / vib); **Rate**, **Depth**; **Rotor** slow / fast (rotary: ramps between speeds like a real one; Rate sets the fast speed); speed readout |
| DELAY | Delay On; **Delay Type** digital (clean) / analog (darker repeats, a little wobble, soft saturation in the feedback, like a bucket-brigade pedal); **Sync** ms / tempo; the **Time** knob covers milliseconds or, in tempo mode, the divisions (1/64 .. 2/1, dotted and triplets, at the MPC tempo); **Feedback**, **Delay Mix**; actual delay time readout |
| REVERB | Reverb On; **Reverb Type** plate / spring / shimmer; **Decay**, **Rev Tone**, **Reverb Mix** |

**Hz / ms <-> Sync on the Rate and Time knobs.** A touch screen has no hard stops, so the rule is:
hold the knob at its end for about 0.7 s after moving it there. Free (Hz or ms) flips to Sync at the
bottom; Sync flips back at the top. While you hold, the value reads "hold -> Sync" (or "-> Hz" /
"-> ms"); afterwards "Sync 1/8D" or "Hz 5.50" / "ms 380". The flip keeps the speed: Sync starts on
the division nearest the free value, and back. A quick pass through the end, loading a preset or a
project, randomize and undo never flip. The selectors (Rate Mode, Sync) switch directly too. Tempo
comes from the MPC with every audio block; without one, Sync falls back to the free scale and the
value says "no tempo".
**Randomize** sets new values within musical ranges (never bypass, input, output, stereo, Sync or
the section switches; the limiter is always on); **Rand Amount** 0-100 % (low nudges, 100 %
rerolls); **Undo** restores exactly. **Save Preset** opens naming with an auto-name ("Random 01"
after a randomize, else "User 01"): Name Pos picks a character (shown in [brackets]), Letter
changes it, Confirm (or Save again) stores it, Cancel drops it. User presets join the preset list
after the factory presets and are kept in `VST2 Plugins/.sureeel Hydra Eel user presets.bin`, so
they survive restarts and reinstalls. Buttons act on every tap; the status lines show the result.

Program Mode Q-Links: Amount, Tone, Level, Gate Bias, Blend, Rate, Depth, Rotor, Time, Feedback,
Delay Mix, Decay, Reverb Mix, Randomize, Save, Output.

## Presets

Init; spaghetti western: **Lonesome Trail**, **Showdown**, **Desert Wind**, **Ennio Twang**,
**Ghost Town**, **Duel at Dawn**, **Tumbleweed**, **Octave Outlaw**, **Big Sky Fuzz**; funk and jazz on
the compressors: **Funk Chicken**, **Jazz Box**, **Clean Strut**, **Smooth Cat**. The compressor and
fuzz positions are inspired by those classic units' behaviour; they are not models of their
circuits, and the names are trademarks of their owners.

## How it works

- **Compressors**: a level detector, a soft-knee static curve in dB, and attack / release applied to
  the gain reduction every 4 samples, with the gain interpolated per sample (no stepping, no ripple:
  tested under 0.5 % distortion on a loud low E). Opto: power detector, ~6 ms attack, release that
  stretches from ~240 ms to ~730 ms after long compression (program dependent), ratio 3. Smooth:
  ~6 ms / ~660 ms, ratio 6, slightly dark. FET: peak detector, ~0.5 ms attack, ~180 ms release,
  ratio 8, a little asymmetric bite. Punch: ~2 ms attack (the pick click gets through), ~480 ms
  release, ratio 14, bright top. Automatic make-up gain; Drive Mix blends in the dry signal.
- **Fuzzes**: germanium, gated and octave run at 8x the sample rate, sustain at 4x (63-tap half-band
  at the base rate, -86 dB from 26 kHz, then 15- and 7-tap stages); every clipping or rectifying
  stage uses first-order anti-derivative anti-aliasing. A 3.1 kHz note at full gain folds back at
  -88 dB (germanium), -77 dB (gated), -83 dB (sustain), -61 dB (octave). Germanium (Fuzz Face
  style): one hot stage, asymmetric soft clip. Gated (Tone Bender style): a soft stage, coupling
  high-pass, a near-hard second stage with its own gate. Sustain (Big Muff style): two diode stages
  with a low-pass between and the mid-scoop tone stack. Octave (Octavia style): a 12 dB/octave
  low-cut at 160 Hz so the rectifier tracks cleanly, a saturating input stage, a full-wave
  rectifier, DC removed, a fuzz clip, then two smoothing low-passes (6 and 9 kHz); level matched to
  the other fuzzes. Starve (Gate Bias): s = knob^2.2; a sag that drops the gain on loud notes, a
  bias offset, and a soft gate (v^3 / (v^2 + d^2)) on the amplified signal before the clipper,
  with d a fraction of the playing level - measured: 1 % no change, 50 % -1.4 dB, 100 % -14 dB
  with clear sputter; a -40 dBFS note still sounds (-11 dB) and nothing clicks.- **Tremolo / vibrato**: tremolo is a pulse-shaped dip with make-up (full depth swings ~48 dB with
  no drop in average level); square chops (2 ms slew). Vibrato is a swept delay with 4-point
  Hermite interpolation; depth follows a curve from subtle (0.3 % pitch) to dramatic (12.7 %, about
  2 semitones). The delay follows the integral of the chosen shape, so the PITCH is a sine, a
  triangle, or a true two-pitch square. In stereo the right tremolo runs a quarter cycle ahead.- **Rotary**: 800 Hz crossover; horn with Doppler (moving delay) and loudness change heard by two
  mics on opposite sides, drum with loudness change; the horn reaches speed in ~0.7 s, the drum in
  ~4 s. **Phaser**: 6 first-order all-pass stages per side with feedback, stereo quadrature.
- **Delay**: up to 4 s, stereo (right side 11 ms later for width); analog: 2.6 kHz low-pass and
  90 Hz high-pass in the feedback (two poles: each repeat clearly darker), soft saturation, +-0.06 %
  wobble (enough for a hint of tape, not a warble).
- **Reverbs** run at half the sample rate (their tails are dark; half the CPU). **Plate**: Jon
  Dattorro's plate (JAES 45(9), 1997) with its tank sweep cut to a quarter (a deep sweep is heard as
  a warble on held notes). **Spring**: the chirp happens once per note: a short dispersive all-pass
  chain on the input (the boing), a dense, dark tank with no sweep, and two dispersed drip echoes
  (31 and 63 ms) - no recirculating chirp, so held notes decay smoothly without pitch wobble. **Shimmer**: the plate with an
  octave-up pitch shifter feeding its output back into its input.
- **Limiter**: sample-peak look-ahead at -1 dBFS, 64 samples latency (reported to the host).

## CPU

Desktop measurement per 128-frame block (2.9 ms), `test/hydra_test.cpp`: all sections off 0.13 %;
default-style chain (8x germanium fuzz, tremolo, analog delay, spring) 1.18 %; worst case 1.21 %.
The SDK's rule of thumb (x15-25 on the 32-bit ARM models) puts these at roughly 18-29 % on a Force -
over the SDK's 15 % guideline - mainly the 8x fuzz oversampling, chosen for sound over CPU. Switching
a section off removes its cost; a compressor instead of a fuzz saves most of it. Check the real
figure in Device Info.
## Project save

Every parameter, the selected preset and the drive mix are saved with the project and restored
exactly, whichever order the MPC restores its own saved preset number in (tested with
`tools/restore_test.cpp` for all three orders).
## Build and test

```
python tools/build.py "plugins/Hydra Eel" --target native
python "plugins/Hydra Eel/skin_src/make_skin.py"
python tools/build.py "plugins/Hydra Eel"
.sdk-tools\zigenv\Lib\site-packages\ziglang\zig.exe c++ -std=c++17 -O2 -I framework -o hydra_test.exe "plugins/Hydra Eel/test/hydra_test.cpp"
hydra_test.exe "plugins/Hydra Eel/build/native/hydra-eel.dll" <scratch folder>
```

`skin_src/make_art.py` makes the recoloured logo and the page background from
`skin_src/reference/` (needs Pillow; the results are kept in `skin_src/assets/`).

## Licence

All DSP is original code (the plate follows Dattorro's published design). The SDK framework is MIT
licensed. The Sure Eel logo and the artwork made from it are not covered by the MIT licence.
