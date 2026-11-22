# Kashyyyk Fuzz

A modular fuzz pedal for Akai MPC and Force, by Sure Eel. Swap modeled parts on a virtual circuit board; the
clipping stage can never be bypassed, so every configuration is a fuzz. **Version 0.3.**

## 0.3: circuit families, stages, Sag, Feedback, presets

- **Six circuit families (Topology), each 1, 2 or 3 stages (Stages):** Face (the Fuzz Face pair, then
  single-transistor stages), Bender (a transistor stage into the Fuzz Face pair), Muff (transistor stages with diodes
  in their feedback), Op-Amp (op-amp stages with diodes in their feedback; 3 MHz gain-bandwidth, as a TL072), Octave
  (a driven preamp into a full-wave diode rectifier, then the stages) and Dual (a Face path and an Op-Amp path in
  parallel, mixed by **Dual Mix**). No family is tied to a stage count.
- **Q2:** the transistor for the stages after the first ("Same" = as Q1). **Coupling:** the caps between stages,
  10n / 47n / 100n film or 1u / 10u / 100u electrolytic. **Resistors:** metal film (quiet, exact) or carbon comp
  (+/-10 % values and excess noise: hiss in the pauses, grit on the notes).
- **Sag** starves the battery. Every stage was solved at 13 battery voltages from 9 V down to 3 V, each part biased
  at a fresh 9 V as a builder would, and the knob spans 9 V down to just past where the circuit's first stage to give
  up stops working (measured per part: a leaky OC44 Fuzz Face dies at 6.2 V, the other parts carry on to 3 V). The
  first half of the knob is subtle; near the top quiet notes sputter and gate first while hard notes break through.
  Loud notes also pull the battery down a little. 1 % is practically the fresh battery, never a gate.
- **Feedback:** a resonant loop from the output back into the first stage; at the top it squeals on its own. A
  safety limiter (instant attack, ceiling -0.9 dBFS) keeps it and everything else under full scale.
- **Buffer** (global): a buffered pedal in front, so the guitar's volume knob only changes the level (no clean-up).
- **29 factory presets** by family (named by sound, "inspired by"): Fuzz Face (Germanium Warmth, Silicon Aggression,
  Hendrix Clean-Up), Tone Bender (Mk I.5 Rasp, Supa Punch, British Buzz), Maestro FZ-1 (Sixties Sputter), Mosrite
  Fuzzrite (Surf Saw), Shin-ei / Univox Super-Fuzz (Octave Scoop, Super Splat), Octavia (Upper Octave), Big Muff
  (Triangle, Ram's Head, Op-Amp, Russian, Nano, Lead, Shoegaze Wall), ZVex Fuzz Factory (Factory Squeal, Starved
  Gate), Death By Audio Fuzz War (War Wall, Scorched Earth), Supersonic Fuzz Gun (Gun Blend, Sonic Gate), Apocalypse
  (Apocalypse Gate, Apocalypse Octave), boutique (OC44 Black Glass, 2N2613 Tribute), plus Germanium Face. The Death
  By Audio models follow documented behaviour; their schematics are not published.
- **Level matching** for every combination: an estimate measured offline for all 1800 circuit / stage / transistor /
  diode / arrangement combinations (times factors for the tone stack, input cap and coupling) applies at once; a
  background thread then measures the exact combination on four playing styles and refines it (remembered for 128
  combinations).
- **Fixed:** changing a part while playing made a burst of hiss for a few milliseconds (the anti-aliasing kept a
  value from the old curve). The anti-aliasing now works from positions only, so a changed curve cannot spike, and
  the level trim is applied before the output filters so a switch never boosts the tail of the previous sound.
- **Fixed:** a saved preset did not appear in the MPC's preset list until the plugin was reloaded; the plugin now
  asks the MPC to re-read the names after a save (as Hydra Eel and Elver do).
- **Q-Links, one meaning per column:** PEDAL (1) Fuzz, Tone, Level, Blend (2) Sag, Stages, Topology, Feedback (3) the
  four parts (4) Guitar Vol, Vol Follow, Input, Output. PARTS (1) the four parts (2) Tone Stack. CIRCUIT (1) Topology,
  Stages, Dual Mix (2) Q2, Coupling, Resistors, Buffer. SAVE (1) Name Pos, Letter. Program Mode uses the PEDAL layout.
- **Pages:** PEDAL (the pedal as before; SAG / STAGES / TOPOLOGY / FEEDBACK join under the big knobs; Guitar Vol
  moved to the global column with Vol Follow, Input and Output), PARTS, CIRCUIT (new), SAVE.

## 0.2: Random, Undo, Save, the logo

- **Diodes are in the circuit by default** (arrangement Pair; 0.1 defaulted to None, so the diode slot did nothing
  until an arrangement was picked). With None the board shows empty sockets. At high Fuzz the transistor stage
  already clips hard, so the diode type changes the sound subtly (the knee); the LED, the asymmetric arrangements
  (D9B, 2+1, Single) and lower Fuzz settings make it clearly audible - as in a real fuzz feeding a diode clipper.
- **SAVE page:** **RANDOM** picks new parts, Fuzz and Tone in safe ranges (never Input, Output, Guitar Vol or Vol
  Follow); **UNDO** restores the sound before the last Random. **SAVE** starts a name (Random 01 / User 01), Name Pos
  and Letter edit it (Q-Links 1 and 2), SAVE again stores it in the next of 16 user slots - listed in the MPC's
  preset list after the factory preset and kept across restarts (".sureeel Kashyyyk Fuzz user presets.bin" next to
  the plugin folder, written by a background thread). **CANCEL** leaves the editor.
- The **Sure Eel logo** on the pedal, between the title and the LED (and on the SAVE page). The logo is not
  MIT-licensed.
## Milestone 1

- **Stereo in, mono fuzz.** The fuzz is one engine fed (L + R) / 2; the dry signal keeps its stereo and is delayed
  to match the engine (35 samples, reported to the host), so **Blend** never combs.
- **Fuzz Face stage** (Q1 / Q2) at 4x oversampling, then a **diode clipper**, the **tone stack** (Muff, Flat,
  Mid Boost, Tilt, Bypass) and a level-matched output.
- **Three swappable slots**, each changing the model and the picture on the board:
  - Transistors: OC44, 2N2613, AC107 (germanium, leaky, soft) / 2N3904, 2N5088 (silicon, sharp, bright).
  - Diodes: 1N34A, 1N914, 1N6263, D9B (mismatched pair), LED; arranged as Pair, Single, 2+1 Asym or None.
  - Input cap: 1n5, 10n, 47n, 100uF.
- **Guitar Vol** (global): the guitar's pickup, 500k audio-taper volume pot and cable against the fuzz's real
  input impedance, so rolling back cleans up the way it does on a Fuzz Face (strongly with germanium and the fuzz
  up, where the input impedance is low).
- **Vol Follow** (global): Off, or Auto - the input meter follows the peaks of note attacks relative to the
  full-volume level that **LEARN** measured (play at full volume for 4 s) and drives the same model. In Auto the
  Guitar Vol knob is the floor (how far the follower may go): turn it down to let Auto work.
- Global, never changed by presets: Input, Output, Guitar Vol, Vol Follow and the learned reference.

## How close to analog

- **Diodes:** the Shockley equation with published SPICE values (Is, n, series R), the clipper solved exactly.
  Checked against ngspice: within 0.4 mV everywhere.
- **Transistors:** Ebers-Moll with reverse gain and germanium leakage in the published Fuzz Face circuit; the
  bias trim sets Q2's collector to 4.5 V by trimming its collector resistor, as builders do. DC operating point
  within 1.1 mV of ngspice; the large-signal transfer within 2 mV (at full fuzz, where the gain is in the
  thousands, the curves are 6 - 104 microvolts apart in input).
- The transistor stages are solved **quasi-statically** (the coupling and bypass capacitors hold their charge while
  the signal swings); their frequency effects are separate exact filters. The Big Muff stage's diodes sit behind
  their 1 uF capacitor, so they see the signal swing but never the bias.
- **Starved supply (0.3):** the Fuzz Face at 9, 7.5, 6 and 4.5 V against ngspice: every DC point within 1.1 mV,
  every transfer within tolerance (90 checks, 0 failures). The validation now also fails as it should when a
  transfer is off (0.2's transfer check could not fail - a comment had swallowed its counter; its printed
  differences were already within the limit).
- The exact solves are tabulated offline (`gen/gen_tables.cpp` writes `src/kf_tables.inc`, every solve converging),
  so the MPC only does table lookups with anti-aliasing (ADAA) at 4x oversampling - 8x for Octave and Dual on the
  64-bit MPC (the 32-bit Force runs everything at 4x).

## Measured (0.3)

- Aliasing at full fuzz (3.1 kHz): Face 51 - 59 dB below the harmonics, Bender 49, Muff 56, Op-Amp 47, and at 8x
  on the MPC Octave 48 and Dual 51. On the Force (4x) Octave measures 35 dB and Dual 43 dB - under the 45 dB aim.
- Level: swapping any one part moves the level by 0.8 dB or less; the 18 family / stage-count circuits sit within
  1.5 dB of each other; the 29 presets within 7.6 dB (Sag,
  Feedback and Blend are part of their sound); 97 % of 400 random combinations of every slot within a 6 dB window.
  The exceptions are mostly Octave with an LED rectifier, which gates the decay of a note, so its level depends on how
  it is played (up to 13 dB low on a riff).
- Switching a part or circuit while playing: the loudest 2 ms afterwards is at most 2.8 dB above the playing before
  it (the change of sound itself), with no burst.
- CPU (desktop, share of real time): the default Face 0.95 %; 3 stages about 2 %; Octave / Dual at 8x 2.6 - 5.9 %;
  the heaviest (Dual, 3 stages, Sag, Feedback, carbon comp) 8 - 9 %. 0.2 measured about 1.9x the desktop figure on
  the MPC, so the heaviest is about 15 - 18 % there. The Force runs everything at 4x (heaviest about 4.8 % on the
  desktop); its own readout is not measured yet.

## Pages

**PEDAL:** FUZZ, TONE, LEVEL, BLEND; SAG, STAGES, TOPOLOGY, FEEDBACK; the board with the parts (tap one to swap
it); right column: Guitar Vol, Vol Follow, LEARN, Input, Output, the readouts (CPU; the circuit, its battery
voltage and "lvl.." while a new combination is being level-matched). **PARTS:** every part as a picture button.
**CIRCUIT:** the family, stages and Dual Mix, Q2, coupling caps, resistors, Buffer. **SAVE:** Random, Undo, naming.

## Building and testing

```
python tools/build.py "plugins/Kashyyyk Fuzz"
zig c++ -std=c++17 -O2 -I src -o build/gen_tables.exe gen/gen_tables.cpp && build/gen_tables.exe src/kf_tables.inc
zig c++ -std=c++17 -O2 -I ../../framework -I src -o build/kf_test.exe test/kf_test.cpp && build/kf_test.exe build/renders
zig c++ -std=c++17 -O2 -I src -o build/validate_spice.exe gen/validate_spice.cpp && build/validate_spice.exe <ngspice_con.exe> <folder>
```

Needs MPC OS / Force OS 3.10.1 or later with Enhancements and External Plugin Support switched on.

## Licence

MIT. The oversampler, filters and ADAA follow Hydra Eel (same author, MIT).
