> The EBU test audio is not included in this repository. Download the EBU loudness test set (Tech 3341 / 3342) from the EBU website and unzip it into `tests/ebu/` to rerun these tests.

# LUFS Master vs the EBU loudness test set (v05)

Expected values: EBU Tech 3341 v4 (Nov 2023) Table 1, cases 1-23; EBU Tech 3342 v4 Table 1 (LRA).
All files 48 kHz. Plugin native build, fresh instance per file, Measure mode, readouts polled after every
audio block exactly as the screen would show them (one decimal). True peak measured in Bypass.
Run with `tests/tools/ebu_run.py` and the test host `test/wavhost.cpp` (`--meter` mode); the SDK
host simulator cannot feed WAV files, its own checks passed at 48 kHz (20/20, block 128 and 32).

## Meter results (one row per file / measure)

| Case | File | Measure | Expected | Plugin | Error | ±0.3 | Spec (±0.1 / TP +0.2/−0.4) | Note |
|---|---|---|---|---|---|---|---|---|
| 1 | seq-3341-1-16bit.wav | M (all readings after 0.5 s, worst) | -23.0 | -23.0 | +0.0 | PASS | PASS | range -23.0 .. -23.0 |
| 1 | seq-3341-1-16bit.wav | S (all readings after 3.1 s, worst) | -23.0 | -23.0 | +0.0 | PASS | PASS | range -23.0 .. -23.0 |
| 1 | seq-3341-1-16bit.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 2 | seq-3341-2-16bit.wav | M (all readings after 0.5 s, worst) | -33.0 | -33.0 | +0.0 | PASS | PASS | range -33.0 .. -33.0 |
| 2 | seq-3341-2-16bit.wav | S (all readings after 3.1 s, worst) | -33.0 | -33.0 | +0.0 | PASS | PASS | range -33.0 .. -33.0 |
| 2 | seq-3341-2-16bit.wav | I | -33.0 | -33.0 | +0.0 | PASS | PASS |  |
| 3 | seq-3341-3-16bit-v02.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 4 | seq-3341-4-16bit-v02.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 5 | seq-3341-5-16bit-v02.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 7 | seq-3341-7_seq-3342-5-24bit.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 8 | seq-3341-2011-8_seq-3342-6-24bit-v02.wav | I | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 6 | seq-3341-6-5channels-16bit.wav | I (5.0 / 5.1) | -23.0 | -- |  | N/A | N/A | N/A: stereo insert; L+R only would read -28.0 |
| 6 | seq-3341-6-6channels-WAVEEX-16bit.wav | I (5.0 / 5.1) | -23.0 | -- |  | N/A | N/A | N/A: stereo insert; L+R only would read -28.0 |
| 9 | seq-3341-9-24bit.wav | S (all readings after 3 s, worst) | -23.0 | -23.0 | +0.0 | PASS | PASS | range -23.0 .. -23.0 |
| 10-1 | seq-3341-10-1-24bit.wav | max S | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 10-2 | seq-3341-10-2-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-3 | seq-3341-10-3-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-4 | seq-3341-10-4-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-5 | seq-3341-10-5-24bit.wav | max S | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 10-6 | seq-3341-10-6-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-7 | seq-3341-10-7-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-8 | seq-3341-10-8-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-9 | seq-3341-10-9-24bit.wav | max S | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 10-10 | seq-3341-10-10-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-11 | seq-3341-10-11-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-12 | seq-3341-10-12-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-13 | seq-3341-10-13-24bit.wav | max S | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 10-14 | seq-3341-10-14-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-15 | seq-3341-10-15-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-16 | seq-3341-10-16-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-17 | seq-3341-10-17-24bit.wav | max S | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 10-18 | seq-3341-10-18-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-19 | seq-3341-10-19-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 10-20 | seq-3341-10-20-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 11 seg 1 | seq-3341-11-24bit.wav | max S | -38.0 | -38.0 | +0.0 | PASS | PASS |  |
| 11 seg 2 | seq-3341-11-24bit.wav | max S | -37.0 | -37.1 | -0.1 | PASS | PASS |  |
| 11 seg 3 | seq-3341-11-24bit.wav | max S | -36.0 | -36.1 | -0.1 | PASS | PASS |  |
| 11 seg 4 | seq-3341-11-24bit.wav | max S | -35.0 | -35.1 | -0.1 | PASS | PASS |  |
| 11 seg 5 | seq-3341-11-24bit.wav | max S | -34.0 | -34.0 | +0.0 | PASS | PASS |  |
| 11 seg 6 | seq-3341-11-24bit.wav | max S | -33.0 | -33.1 | -0.1 | PASS | PASS |  |
| 11 seg 7 | seq-3341-11-24bit.wav | max S | -32.0 | -32.1 | -0.1 | PASS | PASS |  |
| 11 seg 8 | seq-3341-11-24bit.wav | max S | -31.0 | -31.1 | -0.1 | PASS | PASS |  |
| 11 seg 9 | seq-3341-11-24bit.wav | max S | -30.0 | -30.0 | +0.0 | PASS | PASS |  |
| 11 seg 10 | seq-3341-11-24bit.wav | max S | -29.0 | -29.1 | -0.1 | PASS | PASS |  |
| 11 seg 11 | seq-3341-11-24bit.wav | max S | -28.0 | -28.1 | -0.1 | PASS | PASS |  |
| 11 seg 12 | seq-3341-11-24bit.wav | max S | -27.0 | -27.1 | -0.1 | PASS | PASS |  |
| 11 seg 13 | seq-3341-11-24bit.wav | max S | -26.0 | -26.0 | +0.0 | PASS | PASS |  |
| 11 seg 14 | seq-3341-11-24bit.wav | max S | -25.0 | -25.1 | -0.1 | PASS | PASS |  |
| 11 seg 15 | seq-3341-11-24bit.wav | max S | -24.0 | -24.1 | -0.1 | PASS | PASS |  |
| 11 seg 16 | seq-3341-11-24bit.wav | max S | -23.0 | -23.1 | -0.1 | PASS | PASS |  |
| 11 seg 17 | seq-3341-11-24bit.wav | max S | -22.0 | -22.0 | +0.0 | PASS | PASS |  |
| 11 seg 18 | seq-3341-11-24bit.wav | max S | -21.0 | -21.1 | -0.1 | PASS | PASS |  |
| 11 seg 19 | seq-3341-11-24bit.wav | max S | -20.0 | -20.1 | -0.1 | PASS | PASS |  |
| 11 seg 20 | seq-3341-11-24bit.wav | max S | -19.0 | -19.1 | -0.1 | PASS | PASS |  |
| 12 | seq-3341-12-24bit.wav | M (all readings after 1 s, worst) | -23.0 | -23.0 | +0.0 | PASS | PASS | range -23.0 .. -23.0 |
| 13-1 | seq-3341-13-1-24bit.wav | max M | -23.0 | -23.0 | +0.0 | PASS | PASS | tone starts 0 ms in |
| 13-2 | seq-3341-13-2-24bit.wav | max M | -23.0 | -23.3 | -0.3 | PASS | FAIL | tone starts 20 ms in |
| 13-3 | seq-3341-13-3-24bit.wav.wav | max M | -23.0 | -23.6 | -0.6 | FAIL | FAIL | tone starts 40 ms in |
| 13-4 | seq-3341-13-4-24bit.wav.wav | max M | -23.0 | -23.5 | -0.5 | FAIL | FAIL | tone starts 60 ms in |
| 13-5 | seq-3341-13-5-24bit.wav.wav | max M | -23.0 | -23.2 | -0.2 | PASS | FAIL | tone starts 80 ms in |
| 13-6 | seq-3341-13-6-24bit.wav.wav | max M | -23.0 | -23.0 | +0.0 | PASS | PASS | tone starts 100 ms in |
| 13-7 | seq-3341-13-7-24bit.wav.wav | max M | -23.0 | -23.2 | -0.2 | PASS | FAIL | tone starts 120 ms in |
| 13-8 | seq-3341-13-8-24bit.wav.wav | max M | -23.0 | -23.5 | -0.5 | FAIL | FAIL | tone starts 140 ms in |
| 13-9 | seq-3341-13-9-24bit.wav.wav | max M | -23.0 | -23.7 | -0.7 | FAIL | FAIL | tone starts 160 ms in |
| 13-10 | seq-3341-13-10-24bit.wav.wav | max M | -23.0 | -24.0 | -1.0 | FAIL | FAIL | tone starts 180 ms in |
| 13-11 | seq-3341-13-11-24bit.wav.wav | max M | -23.0 | -24.2 | -1.2 | FAIL | FAIL | tone starts 200 ms in |
| 13-12 | seq-3341-13-12-24bit.wav.wav | max M | -23.0 | -24.0 | -1.0 | FAIL | FAIL | tone starts 220 ms in |
| 13-13 | seq-3341-13-13-24bit.wav.wav | max M | -23.0 | -23.7 | -0.7 | FAIL | FAIL | tone starts 240 ms in |
| 13-14 | seq-3341-13-14-24bit.wav.wav | max M | -23.0 | -23.5 | -0.5 | FAIL | FAIL | tone starts 260 ms in |
| 13-15 | seq-3341-13-15-24bit.wav.wav | max M | -23.0 | -23.2 | -0.2 | PASS | FAIL | tone starts 280 ms in |
| 13-16 | seq-3341-13-16-24bit.wav.wav | max M | -23.0 | -23.0 | +0.0 | PASS | PASS | tone starts 300 ms in |
| 13-17 | seq-3341-13-17-24bit.wav.wav | max M | -23.0 | -23.2 | -0.2 | PASS | FAIL | tone starts 320 ms in |
| 13-18 | seq-3341-13-18-24bit.wav.wav | max M | -23.0 | -23.5 | -0.5 | FAIL | FAIL | tone starts 340 ms in |
| 13-19 | seq-3341-13-19-24bit.wav.wav | max M | -23.0 | -23.7 | -0.7 | FAIL | FAIL | tone starts 360 ms in |
| 13-20 | seq-3341-13-20-24bit.wav.wav | max M | -23.0 | -24.0 | -1.0 | FAIL | FAIL | tone starts 380 ms in |
| 14 seg 1 | seq-3341-14-24bit.wav.wav | max M | -38.0 | -38.0 | +0.0 | PASS | PASS |  |
| 14 seg 2 | seq-3341-14-24bit.wav.wav | max M | -37.0 | -38.0 | -1.0 | FAIL | FAIL |  |
| 14 seg 3 | seq-3341-14-24bit.wav.wav | max M | -36.0 | -36.7 | -0.7 | FAIL | FAIL |  |
| 14 seg 4 | seq-3341-14-24bit.wav.wav | max M | -35.0 | -35.5 | -0.5 | FAIL | FAIL |  |
| 14 seg 5 | seq-3341-14-24bit.wav.wav | max M | -34.0 | -34.2 | -0.2 | PASS | FAIL |  |
| 14 seg 6 | seq-3341-14-24bit.wav.wav | max M | -33.0 | -33.0 | +0.0 | PASS | PASS |  |
| 14 seg 7 | seq-3341-14-24bit.wav.wav | max M | -32.0 | -32.2 | -0.2 | PASS | FAIL |  |
| 14 seg 8 | seq-3341-14-24bit.wav.wav | max M | -31.0 | -31.5 | -0.5 | FAIL | FAIL |  |
| 14 seg 9 | seq-3341-14-24bit.wav.wav | max M | -30.0 | -30.7 | -0.7 | FAIL | FAIL |  |
| 14 seg 10 | seq-3341-14-24bit.wav.wav | max M | -29.0 | -30.0 | -1.0 | FAIL | FAIL |  |
| 14 seg 11 | seq-3341-14-24bit.wav.wav | max M | -28.0 | -29.2 | -1.2 | FAIL | FAIL |  |
| 14 seg 12 | seq-3341-14-24bit.wav.wav | max M | -27.0 | -28.0 | -1.0 | FAIL | FAIL |  |
| 14 seg 13 | seq-3341-14-24bit.wav.wav | max M | -26.0 | -26.7 | -0.7 | FAIL | FAIL |  |
| 14 seg 14 | seq-3341-14-24bit.wav.wav | max M | -25.0 | -25.5 | -0.5 | FAIL | FAIL |  |
| 14 seg 15 | seq-3341-14-24bit.wav.wav | max M | -24.0 | -24.2 | -0.2 | PASS | FAIL |  |
| 14 seg 16 | seq-3341-14-24bit.wav.wav | max M | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 14 seg 17 | seq-3341-14-24bit.wav.wav | max M | -22.0 | -22.2 | -0.2 | PASS | FAIL |  |
| 14 seg 18 | seq-3341-14-24bit.wav.wav | max M | -21.0 | -21.5 | -0.5 | FAIL | FAIL |  |
| 14 seg 19 | seq-3341-14-24bit.wav.wav | max M | -20.0 | -20.7 | -0.7 | FAIL | FAIL |  |
| 14 seg 20 | seq-3341-14-24bit.wav.wav | max M | -19.0 | -20.0 | -1.0 | FAIL | FAIL |  |
| 15 | seq-3341-15-24bit.wav.wav | max true peak (dBTP) | -6.0 | -6.0 | +0.0 | PASS | PASS |  |
| 16 | seq-3341-16-24bit.wav.wav | max true peak (dBTP) | -6.0 | -6.0 | +0.0 | PASS | PASS |  |
| 17 | seq-3341-17-24bit.wav.wav | max true peak (dBTP) | -6.0 | -6.0 | +0.0 | PASS | PASS |  |
| 18 | seq-3341-18-24bit.wav.wav | max true peak (dBTP) | -6.0 | -6.0 | +0.0 | PASS | PASS |  |
| 19 | seq-3341-19-24bit.wav.wav | max true peak (dBTP) | 3.0 | 3.0 | +0.0 | PASS | PASS |  |
| 20 | seq-3341-20-24bit.wav.wav | max true peak (dBTP) | 0.0 | -0.1 | -0.1 | PASS | PASS |  |
| 21 | seq-3341-21-24bit.wav.wav | max true peak (dBTP) | 0.0 | -0.1 | -0.1 | PASS | PASS |  |
| 22 | seq-3341-22-24bit.wav.wav | max true peak (dBTP) | 0.0 | -0.1 | -0.1 | PASS | PASS |  |
| 23 | seq-3341-23-24bit.wav.wav | max true peak (dBTP) | 0.0 | -0.1 | -0.1 | PASS | PASS |  |
| cal | 1kHz Sine -20 LUFS-16bit.wav | M (all readings after 0.5 s, worst) | -20.0 | -20.0 | +0.0 | PASS | PASS | range -20.0 .. -20.0 |
| cal | 1kHz Sine -20 LUFS-16bit.wav | S (all readings after 3.1 s, worst) | -20.0 | -20.0 | +0.0 | PASS | PASS | range -20.0 .. -20.0 |
| cal | 1kHz Sine -20 LUFS-16bit.wav | I | -20.0 | -20.0 | +0.0 | PASS | PASS |  |
| cal | 1kHz Sine -26 LUFS-16bit.wav | M (all readings after 0.5 s, worst) | -26.0 | -26.0 | +0.0 | PASS | PASS | range -26.0 .. -26.0 |
| cal | 1kHz Sine -26 LUFS-16bit.wav | S (all readings after 3.1 s, worst) | -26.0 | -26.0 | +0.0 | PASS | PASS | range -26.0 .. -26.0 |
| cal | 1kHz Sine -26 LUFS-16bit.wav | I | -26.0 | -26.0 | +0.0 | PASS | PASS |  |
| cal | 1kHz Sine -40 LUFS-16bit.wav | M (all readings after 0.5 s, worst) | -40.0 | -40.0 | +0.0 | PASS | PASS | range -40.0 .. -40.0 |
| cal | 1kHz Sine -40 LUFS-16bit.wav | S (all readings after 3.1 s, worst) | -40.0 | -40.0 | +0.0 | PASS | PASS | range -40.0 .. -40.0 |
| cal | 1kHz Sine -40 LUFS-16bit.wav | I | -40.0 | -40.0 | +0.0 | PASS | PASS |  |
| ref | EBU-reference_listening_signal_pinknoise_500Hz_2kHz_R128.wav | I (mono, one channel) | -23.0 | -23.0 | +0.0 | PASS | PASS |  |
| 3342-1 | seq-3342-1-16bit.wav | LRA | 10.0 | -- |  | N/A | N/A | N/A: no LRA meter (integrated reads -22.6) |
| 3342-2 | seq-3342-2-16bit.wav | LRA | 5.0 | -- |  | N/A | N/A | N/A: no LRA meter (integrated reads -16.8) |
| 3342-3 | seq-3342-3-16bit.wav | LRA | 20.0 | -- |  | N/A | N/A | N/A: no LRA meter (integrated reads -20.0) |
| 3342-4 | seq-3342-4-16bit.wav | LRA | 15.0 | -- |  | N/A | N/A | N/A: no LRA meter (integrated reads -24.5) |
| 3342-5 | seq-3341-7_seq-3342-5-24bit.wav | LRA | 5.0 | -- |  | N/A | N/A | N/A: no LRA meter |
| 3342-6 | seq-3341-2011-8_seq-3342-6-24bit-v02.wav | LRA | 15.0 | -- |  | N/A | N/A | N/A: no LRA meter |

±0.3 tolerance: {'PASS': 87, 'N/A': 8, 'FAIL': 25}
spec tolerance: {'PASS': 78, 'N/A': 8, 'FAIL': 34}

## Lock and limiter (Measure -> Lock -> bounce), ceiling -1.0 dBTP

Bounces measured with an independent implementation using the BS.1770 published 48 kHz
coefficients (`tests/tools/lockcheck.py`); true peak with scipy 4x polyphase oversampling.
Raw data: `tests/ebu_lock_results.csv`.

file         target    gain predicted  out I (BS.1770 official)   error within 0.1   TP 4x  <= -1.0  GR max
prog7           -24    -1.0     -24.0                   -23.994  +0.006       PASS   -9.92     PASS     0.0
prog7           -23     0.0     -23.0                   -22.994  +0.006       PASS   -8.92     PASS     0.0
prog7           -16    +7.0     -16.0                   -15.995  +0.005       PASS   -1.92     PASS     0.0
prog7           -14    +9.0     -14.0                   -13.991  +0.009       PASS   -1.05     PASS    -1.2
prog7           -10   +14.4     -10.0                    -9.869  +0.131       FAIL   -1.04     PASS    -6.5
prog8           -24    -1.0     -24.0                   -24.001  -0.001       PASS   -3.64     PASS     0.0
prog8           -23     0.0     -23.0                   -23.002  -0.002       PASS   -2.64     PASS     0.0
prog8           -16    +7.3     -16.0                   -15.963  +0.037       PASS   -1.04     PASS    -5.7
prog8           -14    +9.8     -14.0                   -13.897  +0.103       FAIL   -1.04     PASS    -8.2
prog8           -10   +22.7     -10.0                    -9.840  +0.160       FAIL   -1.04     PASS   -21.1
sine20          -24    -4.0     -24.0                   -23.999  +0.001       PASS  -23.98     PASS     0.0
sine20          -23    -3.0     -23.0                   -22.999  +0.001       PASS  -22.98     PASS     0.0
sine20          -16    +4.0     -16.0                   -16.000  -0.000       PASS  -15.98     PASS     0.0
sine20          -14    +6.0     -14.0                   -14.000  -0.000       PASS  -13.98     PASS     0.0
sine20          -10   +10.0     -10.0                   -10.000  -0.000       PASS   -9.98     PASS     0.0
tones3342_4     -24    +0.5     -24.0                   -23.999  +0.001       PASS  -19.50     PASS     0.0
tones3342_4     -23    +1.5     -23.0                   -22.999  +0.001       PASS  -18.50     PASS     0.0
tones3342_4     -16    +8.5     -16.0                   -16.001  -0.001       PASS  -11.50     PASS     0.0
tones3342_4     -14   +10.5     -14.0                   -14.000  -0.000       PASS   -9.50     PASS     0.0
tones3342_4     -10   +14.5     -10.0                   -10.001  -0.001       PASS   -5.50     PASS     0.0
