# LUFS Master

A loudness meter, leveler and true-peak limiter for Akai MPC and Force standalone hardware, by
Sure Eel (manufacturer "sureeel"). Measure a pass, let it set the gain, and the next pass lands on
your LUFS target. For mastering and for live broadcast.

Runs on 32-bit (armv7) and 64-bit (aarch64) MPC and Force hardware. Needs MPC OS / Force OS 3.10.1
or later with **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin
Support** switched on. To bypass it, switch the plugin off on the MPC.

## How to use it

1. Insert LUFS Master last on the track or master you want to deliver.
2. Pick a **Target** (-14 streaming, -16 Apple Music / podcasts, -23 EBU R128, -24 ATSC A/85, or
   Custom) and a **Ceiling** (true peak, default -1.0 dBTP).
3. **Measure**, **Live Speed Off**, then play the song once. Measure only listens. It shows the input's
   integrated loudness and the **Suggested Gain** (target minus input).
4. Stop the transport. **Auto-set** refines the suggestion with a model of the limiter, writes it
   into **Input Gain** and switches to **Manual**. The status line shows "APPLIED +x.x dB".
5. Play or record again. This pass starts corrected and **Output Integrated** should land on the
   target. **Clear** sets Input Gain back to 0 dB; **Undo** restores the gain and mode from before.

For **live use**, choose Measure with **Live Speed Fast** (live broadcast) or **Slow** (mastering)
and the gain follows the input automatically, within ±**Max Change**:
- **Fast** follows the 3 s loudness and pulls sudden loud bursts down in about a second. It comes back
  up within a few seconds.
- **Slow** follows a 15 s average and never moves faster than 1 dB per second.
- Both hold their gain through pauses and fades, and ramp smoothly, sample by sample.
- When the transport stops, auto-set also writes Input Gain in Fast / Slow but stays in Measure.
  The next pass starts its ride from that gain.

In **Manual**, the Input Gain knob sets the gain. In Measure it is shown as "(manual)" and not used.

A pass is one run of the MPC transport. Each pass starts a new measurement (input and output
integrated, Suggested Gain). **Reset** starts a new one by hand. **Refresh** clears the max
readouts (true peaks, limiter max, averages).

Every setting, Input Gain, the auto-set indicator and its Undo are saved with the project.

## MASTER page

| Area | What it shows / does |
|---|---|
| MODE | Manual / Measure |
| INPUT LEVEL · dBFS | raw input sample peak before any gain, -48..0 dBFS, with a 2 s peak hold (white LED and number) |
| LIVE SPEED | Fast (live broadcast) / Off (measure only) / Slow (mastering) |
| OUTPUT INTEGRATED · LUFS | this pass at the output, after gain and limiter; the bar spans target -24 .. +12 LU with a line at the target |
| Input Gain, Max Change, Ceiling, Custom LUFS | knobs (dB, ±dB, dBTP, LUFS) |
| GAIN CHANGE · dB | the gain being applied now (manual or automatic), bar -24..+24 dB; LIMITER · dB below it |
| LUFS OVER TIME | output loudness of the last 3 s, live in every mode, and its distance to the target (LU) |
| TARGET | double-tap to choose a preset target |
| SUGGESTED GAIN · dB | gain that puts this pass on target; OUTPUT TRUE PEAK MAX · dBTP |
| AUTO-SET | status (waiting for stop / calculating / APPLIED / cleared / undone / pass too short), Clear, Undo, Reset, Refresh |
| HELP | touch any meter or label to see what it does |

**METERS**: output loudness over time (48 columns of 5 s, the last 4 minutes, drawn against the
target line; updates whenever audio plays, in every mode), and every reading as a labelled number:
input and output momentary, short-term, integrated (this pass), ungated average and true-peak max;
gain change, limiter, limiter max, suggested gain and ceiling.

Q-Links (MASTER, rows top-down): Mode, Live Speed, Input Gain, Target / Ceiling, Custom LUFS, Max
Change / Clear, Undo, Reset, Refresh. Program mode: Input Gain, Ceiling, Target, Custom LUFS, Mode,
Live Speed, Max Change, Clear, Undo, Reset, Refresh, then readouts.

## How it works

- **Meter**: ITU-R BS.1770-4 / EBU R128 (K-weighting for the actual sample rate, double
  precision; 400 ms momentary, 3 s short-term, integrated with -70 LUFS absolute and -10 LU
  relative gates).
- **Limiter**: one gain for both channels on a delayed copy of the signal. No clipper, no
  saturation. Peaks are found on an 8× oversampled copy with a 64-tap interpolator, so
  inter-sample peaks are caught. The gain dips into each peak along an S-curve over 2.9 ms and
  recovers in 150 ms, aiming 0.12 dB under the Ceiling. **Latency: 160 samples** (3.6 ms at 44.1
  kHz), reported to the host.
- **Auto-set**: from 100 ms records of the pass (loudness and true peak, never audio), a
  block-rate model of the limiter predicts the output loudness for a gain. The gain is refined
  until the prediction is within 0.02 LU of the target.

## Tested (desktop, before installing)

`test/lufs_test.cpp`, 16 checks, with an independent BS.1770 meter and an independent 32×
true-peak meter:
- **Ceiling holds** at 0, -1, -3, -6 and -12 dBTP on an inter-sample-peak torture signal (fs/4
  sine at 45°, square wave, full-scale noise), loud music pushed +12 dB and varying music. The
  worst case is 0.05 dB under the ceiling.
- **Fast** makes 90 % of an 8 dB cut in 1.1 s and is back within 1 dB in 5.5 s. **Slow** never moves
  faster than 0.4 dB/s. The ride's largest change between two samples is 0.0003 dB.
- **Auto-set and second pass**: quiet (-32 LUFS), loud (-6 LUFS, +7.6 dBTP peaks) and varying
  material, to -14 and -23. The second pass landed within 0.01 LU of the target in five cases and
  +0.35 LU in one (varying material at -14, with heavy limiting).
- Clear, Undo, project save / reload, opening a 1.0 project (a Locked gain becomes Input Gain in
  Manual), LUFS over time in every mode with and without the transport, and units on every value.
- SDK host simulator: PASS at 128- and 32-frame blocks, 0.8 % of a desktop core (roughly 12-21 % on
  the Force's 32-bit ARM by the SDK rule of thumb). Restore test and hostile-host test: all pass.

## Build and test

```
python tools/build.py "plugins/LUFS Master"
python "plugins/LUFS Master/skin_src/make_skin.py"     # after layout changes (needs Pillow)
zig c++ -std=c++17 -O2 -I framework -o lufs_test.exe "plugins/LUFS Master/test/lufs_test.cpp"
lufs_test.exe "plugins/LUFS Master/build/native/lufs-master.dll" [wav output folder]
```
`test/neon_shim/` lets the NEON code path run on a PC for comparison (`-D__ARM_NEON`).

## Licence

Original code, MIT © 2026 Sure Eel. K-weighting constants are the published BS.1770 prototype
values. Built with the MIT-licensed MPC Plugin SDK framework.
