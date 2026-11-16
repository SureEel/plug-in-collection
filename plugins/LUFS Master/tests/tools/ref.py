"""Independent reference meter for checking LUFS Master renders.

usage:
  ref.py measure <wav>...          integrated LUFS (pyloudnorm, BS.1770-4) + true peak (4x and 8x)
  ref.py ride <wav> <target>       short-term spread of a leveler render around the target
  ref.py tones <outdir>            write EBU Tech 3341-style test tones (float32 stereo, 44.1 kHz)
"""
import sys, wave
import numpy as np
import pyloudnorm as pyln
from scipy.signal import resample_poly


def read(path):
    import struct
    with open(path, 'rb') as f:
        d = f.read()
    pos, fmt, ch, sr, bits, data = 12, None, None, None, None, None
    while pos + 8 <= len(d):
        cid, ln = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt, ch, sr = struct.unpack('<HHI', d[pos + 8:pos + 16]); bits = struct.unpack('<H', d[pos + 22:pos + 24])[0]
            if fmt == 0xFFFE: fmt = struct.unpack('<H', d[pos + 32:pos + 34])[0]
        elif cid == b'data':
            data = d[pos + 8:pos + 8 + ln]
        pos += 8 + ln + (ln & 1)
    if fmt == 3: x = np.frombuffer(data, '<f4').astype(np.float64)
    elif bits == 16: x = np.frombuffer(data, '<i2') / 32768.0
    elif bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3)
        x = ((b[:, 0].astype(np.int32) << 8) | (b[:, 1].astype(np.int32) << 16) | (b[:, 2].astype(np.int32) << 24)) / 2147483648.0
    else: x = np.frombuffer(data, '<i4') / 2147483648.0
    x = x[:len(x) // ch * ch].reshape(-1, ch)
    if ch == 1: x = np.repeat(x, 2, axis=1)
    return x[:, :2], sr


def tp(x, k):
    return 20 * np.log10(max(np.max(np.abs(resample_poly(x[:, c], k, 1))) for c in range(2)) + 1e-12)


def measure(paths):
    for p in paths:
        x, sr = read(p)
        m = pyln.Meter(sr)
        print(f"{p.split(chr(92))[-1]:38s} I {m.integrated_loudness(x):7.2f} LUFS   TP4x {tp(x, 4):6.2f}   TP8x {tp(x, 8):6.2f} dBTP"
              f"   sample peak {20*np.log10(np.max(np.abs(x))+1e-12):6.2f}")


def ride(path, target):
    x, sr = read(path)
    m = pyln.Meter(sr)
    I = m.integrated_loudness(x)
    win, hop = 3 * sr, sr
    st = []
    for s in range(0, len(x) - win, hop):
        st.append(m.integrated_loudness(x[s:s + win]) if False else
                  -0.691 + 10 * np.log10(np.mean(np.sum(m._filters_apply(x[s:s + win]) ** 2, axis=1)) + 1e-20) if hasattr(m, '_filters_apply') else None)
    print(f"integrated {I:.2f}")


def kweight_short_term(x, sr):
    from scipy.signal import lfilter
    import math
    # BS.1770 K-weighting (same analogue prototype as libebur128)
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / sr); Vh = 10 ** (G / 20); Vb = Vh ** 0.4996667741545416; a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / sr); a0 = 1 + K / Q + K * K
    b2 = [1, -2, 1]; a2 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    y = lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)
    e = np.sum(y ** 2, axis=1)
    win, hop = 3 * sr, sr // 10
    c = np.concatenate([[0], np.cumsum(e)])
    starts = np.arange(0, len(e) - win, hop)
    return -0.691 + 10 * np.log10((c[starts + win] - c[starts]) / win + 1e-20)


def ride2(path, target, inpath):
    x, sr = read(path)
    xi, _ = read(inpath)
    st = kweight_short_term(x, sr)
    sti = kweight_short_term(xi, sr)
    I_in = pyln.Meter(sr).integrated_loudness(xi)
    active = sti > I_in - 10          # passages the leveler should ride
    quiet = sti <= I_in - 10           # passages it should hold
    dev = st[active] - target
    gain = st - sti                    # effective gain per window
    print(f"{path.split(chr(92))[-1]}: output integrated {pyln.Meter(sr).integrated_loudness(x):.2f}")
    print(f"  active passages: {100*np.mean(np.abs(dev) <= 1.0):.0f}% within +-1 LU of target, "
          f"{100*np.mean(np.abs(dev) <= 2.0):.0f}% within +-2 LU, median {np.median(dev):+.2f} LU")
    if quiet.any():
        print(f"  quiet passages ({100*np.mean(quiet):.0f}% of time): input ST median {np.median(sti[quiet]):.1f}, "
              f"effective gain median {np.median(gain[quiet]):+.1f} dB, max {np.max(gain[quiet]):+.1f} dB")
    print(f"  effective gain on active passages: median {np.median(gain[active]):+.1f} dB, "
          f"10-90% {np.percentile(gain[active],10):+.1f} .. {np.percentile(gain[active],90):+.1f} dB")


def tones(outdir):
    import struct
    sr = 44100

    def write(name, x):
        x = x.astype('<f4')
        with open(f"{outdir}\\{name}.wav", 'wb') as f:
            data = x.tobytes()
            f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, 2, sr, sr * 8, 8, 32)
                    + b'data' + struct.pack('<I', len(data)) + data)

    def sine(db, secs, f=1000.0):
        t = np.arange(int(secs * sr)) / sr
        s = 10 ** (db / 20) * np.sin(2 * np.pi * f * t)
        return np.stack([s, s], axis=1)
    write('t1_sine_m23', sine(-23, 20))                                        # expect -23.0
    write('t2_sine_m33', sine(-33, 20))                                        # expect -33.0
    write('t3_gating', np.concatenate([sine(-36, 10), sine(-23, 60), sine(-36, 10)]))   # expect -23.0 (rel gate)
    write('t4_gating2', np.concatenate([sine(-72, 10), sine(-36, 10), sine(-23, 60), sine(-36, 10), sine(-72, 10)]))  # -23.0
    # true-peak: sine at fs/4 with 45 degree phase: samples at 0.707 of the real peak (-3 dB under-read)
    t = np.arange(20 * sr) / sr
    s = 10 ** (-6 / 20) * np.sin(2 * np.pi * (sr / 4) * t + np.pi / 4)
    write('t5_tp_m6', np.stack([s, s], axis=1))                                # true peak -6.0, sample peak -9.0


def drums(outdir):
    """Peaky, unmastered-style material: kick, snare, hats, bass and pad with a quiet break."""
    import struct
    sr, secs, bpm = 44100, 90, 96
    rng = np.random.default_rng(7)
    n = secs * sr
    out = np.zeros((n, 2))
    beat = int(sr * 60 / bpm)
    t = np.arange(int(0.4 * sr)) / sr
    kick = 0.95 * np.sin(2 * np.pi * (50 + 120 * np.exp(-t * 30)) * t) * np.exp(-t * 9)
    snare = (rng.standard_normal(len(t)) * 0.55 + 0.3 * np.sin(2 * np.pi * 190 * t)) * np.exp(-t * 22)
    hat = rng.standard_normal(int(0.05 * sr)) * 0.12 * np.exp(-np.arange(int(0.05 * sr)) / sr * 90)
    for b in range(0, n // beat):
        if 40 * sr <= b * beat < 52 * sr:      # 12 s break: pad only
            continue
        s = b * beat
        def add(x, pos, pan=0.0, gain=1.0):
            e = min(n, pos + len(x)); out[pos:e, 0] += x[:e - pos] * gain * (1 - pan); out[pos:e, 1] += x[:e - pos] * gain * (1 + pan)
        add(kick, s, 0.0, 1.0 if b % 4 != 3 else 0.8)
        if b % 2 == 1: add(snare, s, 0.05)
        for h in range(2): add(hat, s + h * beat // 2, 0.3 if h else -0.3)
    tt = np.arange(n) / sr
    bass = 0.18 * np.sin(2 * np.pi * 55 * tt) * (0.6 + 0.4 * np.sin(2 * np.pi * tt * bpm / 60))
    pad = 0.03 * (np.sin(2 * np.pi * 220 * tt) + np.sin(2 * np.pi * 277.2 * tt) + np.sin(2 * np.pi * 329.6 * tt))
    out[:, 0] += bass + pad; out[:, 1] += bass + pad * 0.9
    out *= 0.89 / np.max(np.abs(out))       # peaks at -1 dBFS
    x = out.astype('<f4')
    data = x.tobytes()
    with open(f"{outdir}\\drums.wav", 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, 2, sr, sr * 8, 8, 32)
                + b'data' + struct.pack('<I', len(data)) + data)


if __name__ == '__main__':
    if sys.argv[1] == 'drums': drums(sys.argv[2])
    if sys.argv[1] == 'measure': measure(sys.argv[2:])
    elif sys.argv[1] == 'ride': ride2(sys.argv[2], float(sys.argv[3]), sys.argv[4])
    elif sys.argv[1] == 'tones': tones(sys.argv[2])
