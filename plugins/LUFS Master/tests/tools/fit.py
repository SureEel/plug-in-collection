"""Fit the Lock limiter-loss model to measured renders.

cal.csv rows: key,target,lockGainDb,predicted,pluginOut,grMax,lockedWavPath  (state.bin next to the wav)
For each case: blocks (energy, peak) from the plugin's saved chunk; actual output loudness from pyloudnorm.
"""
import struct, sys
import numpy as np
import pyloudnorm as pyln
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))   # ref.py lives here
from ref import read

ABS = 10 ** ((-70 + 0.691) / 10)


def blocks_from_state(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'MPCP'
    ver, n = struct.unpack('<II', d[4:12]); pos = 12
    for _ in range(n):
        L = d[pos]; pos += 1 + L + 4 + (4 if ver >= 2 else 0)
    extra_len = struct.unpack('<I', d[pos:pos + 4])[0]; pos += 4
    e = d[pos:pos + extra_len]
    cnt = struct.unpack('<I', e[22:26])[0]
    arr = np.frombuffer(e[26:26 + cnt * 8], '<f4').reshape(-1, 2).astype(np.float64)
    return arr[:, 0], arr[:, 1]


def integrated(win_e):
    a = win_e[win_e > ABS]
    if len(a) == 0: return None
    rel = a.mean() * 0.1
    b = a[a > rel]
    return -0.691 + 10 * np.log10(b.mean())


def windows(en, pk):
    we = (en[3:] + en[2:-1] + en[1:-2] + en[:-3]) / 4
    wp = np.maximum(np.maximum(pk[3:], pk[2:-1]), np.maximum(pk[1:-2], pk[:-3]))
    return we, wp


def model_alpha(en, pk, gdb, ceil_lin, alpha):
    we, wp = windows(en, pk)
    G = 10 ** (gdb / 20)
    r = wp * G / ceil_lin
    f = np.where(r > 1, r ** (-2 * alpha), 1.0)
    return integrated(we * G * G * f)


def model_block_release(en, pk, gdb, ceil_lin, k, rel_keep):
    """Block-rate limiter simulation: required GR per block, released into the next blocks;
    each block's energy drops by k * (its GR in dB)."""
    G = 10 ** (gdb / 20)
    over = np.maximum(0.0, 20 * np.log10(np.maximum(pk * G / ceil_lin, 1e-12)))
    gr = np.empty_like(over); prev = 0.0
    for i, o in enumerate(over):
        prev = max(o, prev * rel_keep); gr[i] = prev
    e = en * G * G * 10 ** (-k * gr / 10)
    we = (e[3:] + e[2:-1] + e[1:-2] + e[:-3]) / 4
    return integrated(we)


def model_crest(en, pk, gdb, ceil_lin, a, b):
    """Loss fraction depends on the block's peak-to-loudness ratio: dense material (low crest)
    loses almost all of the overshoot, peaky material a smaller share."""
    G = 10 ** (gdb / 20)
    over = np.maximum(0.0, 20 * np.log10(np.maximum(pk * G / ceil_lin, 1e-12)))
    crest = 20 * np.log10(np.maximum(pk, 1e-9)) - 10 * np.log10(np.maximum(en, 1e-20))   # dB
    frac = np.clip(a - b * (crest - 10), 0.05, 1.0)
    e = en * G * G * 10 ** (-frac * over / 10)
    we = (e[3:] + e[2:-1] + e[1:-2] + e[:-3]) / 4
    return integrated(we)


def main(csv):
    cases = []
    for line in open(csv, encoding='utf-8-sig'):
        key, target, g, pred, pout, grm, wav = line.strip().split(',', 6)
        en, pk = blocks_from_state(wav.replace('_locked.wav', '_state.bin'))
        x, sr = read(wav)
        actual = pyln.Meter(sr).integrated_loudness(x)
        cases.append((key, float(target), float(g), float(pred), actual, en, pk))
    ceil_lin = 10 ** ((-1.0 - 0.05) / 20)

    def report(name, fn):
        errs = []
        for key, t, g, pred, actual, en, pk in cases:
            errs.append(fn(en, pk, g, ceil_lin) - actual)
        errs = np.array(errs)
        return np.max(np.abs(errs)), np.sqrt(np.mean(errs ** 2)), errs

    print('current plugin prediction error (predicted - actual):')
    for key, t, g, pred, actual, en, pk in cases:
        print(f'  {key} {t:5.0f}  gain {g:+5.1f}  predicted {pred:6.2f}  actual {actual:6.2f}  err {pred-actual:+.2f}')

    best = []
    for alpha in np.arange(0.2, 1.01, 0.05):
        m, r, _ = report('a', lambda en, pk, g, c: model_alpha(en, pk, g, c, alpha)); best.append((m, r, f'alpha={alpha:.2f}'))
    for k in np.arange(0.3, 1.01, 0.05):
        for keep in [0.0, 0.2, 0.35, 0.5, 0.65]:
            m, r, _ = report('b', lambda en, pk, g, c: model_block_release(en, pk, g, c, k, keep)); best.append((m, r, f'block k={k:.2f} keep={keep}'))
    for a in np.arange(0.3, 1.01, 0.05):
        for b in [0.0, 0.02, 0.04, 0.06, 0.08, 0.1]:
            m, r, _ = report('c', lambda en, pk, g, c: model_crest(en, pk, g, c, a, b)); best.append((m, r, f'crest a={a:.2f} b={b}'))
    best.sort()
    print('best models by max error:')
    for m, r, name in best[:8]: print(f'  max {m:.2f}  rms {r:.2f}  {name}')
    best.sort(key=lambda x: x[1])
    print('best models by rms error:')
    for m, r, name in best[:5]: print(f'  max {m:.2f}  rms {r:.2f}  {name}')
    for name, fn in [('alpha 0.40 (current)', lambda en, pk, g, c: model_alpha(en, pk, g, c, 0.4))]:
        m, r, errs = report(name, fn); print(name, f'max {m:.2f} rms {r:.2f}')


if __name__ == '__main__':
    main(sys.argv[1])
