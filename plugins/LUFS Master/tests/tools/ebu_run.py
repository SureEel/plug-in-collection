"""Run the EBU loudness test set (v05) through LUFS Master and compare with EBU Tech 3341 v4 Table 1.

usage: ebu_run.py <wavhost.exe> <plugin.dll> <ebu folder> <work folder> <report.md>
"""
import csv, math, os, re, subprocess, sys

HOST, DLL, EBU, WORK, REPORT = sys.argv[1:6]
os.makedirs(WORK, exist_ok=True)
USER_TOL, SPEC_TOL = 0.3, 0.1


def run(fname, mono=False):
    prefix = os.path.join(WORK, re.sub(r'\W+', '_', fname))
    args = [HOST, DLL, os.path.join(EBU, fname), prefix, '--meter'] + (['--mono-left'] if mono else [])
    out = subprocess.run(args, capture_output=True, text=True).stdout
    m = re.search(r'RESULT integ=(\S+) tpmax=(\S+)', out)
    rows = []
    with open(prefix + '_series.csv') as f:
        for r in csv.DictReader(f):
            rows.append((float(r['t']), fnum(r['mom']), fnum(r['short']), fnum(r['integ'])))
    tp = m.group(2) if m else '--'
    return fnum(m.group(1)) if m else None, fnum(tp.replace('dB', '').strip()), rows


def fnum(s):
    try: return float(s)
    except (TypeError, ValueError): return None


def vals(rows, col, t0=0.0, t1=1e9):
    return [r[col] for r in rows if t0 <= r[0] <= t1 and r[col] is not None]


results = []   # (case, file, measure, expected, got, lo_tol, hi_tol, note)


def add(case, fname, measure, expected, got, tol_lo=None, tol_hi=None, note=''):
    results.append((case, fname, measure, expected, got, tol_lo, tol_hi, note))


def steady(case, fname, expected, col, name, t0, rows):
    v = vals(rows, col, t0, rows[-1][0] - 0.01)
    if not v: add(case, fname, name, expected, None); return
    worst = max(v, key=lambda x: abs(x - expected))
    add(case, fname, f'{name} (all readings after {t0:g} s, worst)', expected, worst, note=f'range {min(v):.1f} .. {max(v):.1f}')


# 1, 2: M, S, I
for case, fn, exp in [('1', 'seq-3341-1-16bit.wav', -23.0), ('2', 'seq-3341-2-16bit.wav', -33.0)]:
    I, tp, rows = run(fn)
    steady(case, fn, exp, 1, 'M', 0.5, rows)
    steady(case, fn, exp, 2, 'S', 3.1, rows)
    add(case, fn, 'I', exp, I)

# 3, 4, 5, 7, 8: I
for case, fn in [('3', 'seq-3341-3-16bit-v02.wav'), ('4', 'seq-3341-4-16bit-v02.wav'), ('5', 'seq-3341-5-16bit-v02.wav'),
                 ('7', 'seq-3341-7_seq-3342-5-24bit.wav'), ('8', 'seq-3341-2011-8_seq-3342-6-24bit-v02.wav')]:
    I, tp, rows = run(fn)
    add(case, fn, 'I', -23.0, I)

# 6: multichannel - not applicable to a stereo insert
for fn in ['seq-3341-6-5channels-16bit.wav', 'seq-3341-6-6channels-WAVEEX-16bit.wav']:
    I, tp, rows = run(fn)
    add('6', fn, 'I (5.0 / 5.1)', -23.0, None, note=f'N/A: stereo insert; L+R only would read {I}')

# 9: S constant after 3 s
I, tp, rows = run('seq-3341-9-24bit.wav')
steady('9', 'seq-3341-9-24bit.wav', -23.0, 2, 'S', 3.0, rows)

# 10-1..20: max S per file
for i in range(1, 21):
    fn = f'seq-3341-10-{i}-24bit.wav'
    I, tp, rows = run(fn)
    v = vals(rows, 2)
    add(f'10-{i}', fn, 'max S', -23.0, max(v) if v else None)

# 11: successive max S per 6 s segment
I, tp, rows = run('seq-3341-11-24bit.wav')
for i in range(20):
    v = vals(rows, 2, 6 * i + 0.001, 6 * i + 6)
    add(f'11 seg {i+1}', 'seq-3341-11-24bit.wav', 'max S', -38.0 + i, max(v) if v else None)

# 12: M constant after 1 s
I, tp, rows = run('seq-3341-12-24bit.wav')
steady('12', 'seq-3341-12-24bit.wav', -23.0, 1, 'M', 1.0, rows)

# 13-1..20: max M per file
for i in range(1, 21):
    fn = f'seq-3341-13-{i}-24bit.wav' if i <= 2 else f'seq-3341-13-{i}-24bit.wav.wav'
    I, tp, rows = run(fn)
    v = vals(rows, 1)
    add(f'13-{i}', fn, 'max M', -23.0, max(v) if v else None, note=f'tone starts {(i-1)*20} ms in')

# 14: successive max M per 0.8 s segment
I, tp, rows = run('seq-3341-14-24bit.wav.wav')
for i in range(20):
    v = vals(rows, 1, 0.8 * i + 0.001, 0.8 * i + 0.8)
    add(f'14 seg {i+1}', 'seq-3341-14-24bit.wav.wav', 'max M', -38.0 + i, max(v) if v else None)

# 15-23: true peak (+0.2 / -0.4 dBTP)
for case, exp in [(15, -6.0), (16, -6.0), (17, -6.0), (18, -6.0), (19, 3.0), (20, 0.0), (21, 0.0), (22, 0.0), (23, 0.0)]:
    fn = f'seq-3341-{case}-24bit.wav.wav'
    I, tp, rows = run(fn)
    add(str(case), fn, 'max true peak (dBTP)', exp, tp, -0.4, 0.2)

# calibration sines and the pink-noise reference
for fn, exp in [('1kHz Sine -20 LUFS-16bit.wav', -20.0), ('1kHz Sine -26 LUFS-16bit.wav', -26.0), ('1kHz Sine -40 LUFS-16bit.wav', -40.0)]:
    I, tp, rows = run(fn)
    steady('cal', fn, exp, 1, 'M', 0.5, rows)
    steady('cal', fn, exp, 2, 'S', 3.1, rows)
    add('cal', fn, 'I', exp, I)
I, tp, rows = run('EBU-reference_listening_signal_pinknoise_500Hz_2kHz_R128.wav', mono=True)
add('ref', 'EBU-reference_listening_signal_pinknoise_500Hz_2kHz_R128.wav', 'I (mono, one channel)', -23.0, I)

# Tech 3342: loudness range - not implemented in the plugin
for i, lra in [(1, 10), (2, 5), (3, 20), (4, 15)]:
    fn = f'seq-3342-{i}-16bit.wav'
    I, tp, rows = run(fn)
    add(f'3342-{i}', fn, 'LRA', lra, None, note=f'N/A: no LRA meter (integrated reads {I})')
add('3342-5', 'seq-3341-7_seq-3342-5-24bit.wav', 'LRA', 5, None, note='N/A: no LRA meter')
add('3342-6', 'seq-3341-2011-8_seq-3342-6-24bit-v02.wav', 'LRA', 15, None, note='N/A: no LRA meter')


def verdict(exp, got, lo, hi, tol):
    if got is None: return 'N/A'
    if lo is not None:   # asymmetric true-peak tolerance from the spec
        return 'PASS' if exp + lo - 1e-9 <= got <= exp + hi + 1e-9 else 'FAIL'
    return 'PASS' if abs(got - exp) <= tol + 1e-9 else 'FAIL'


lines = ['| Case | File | Measure | Expected | Plugin | Error | ±0.3 | Spec (±0.1 / TP +0.2/−0.4) | Note |',
         '|---|---|---|---|---|---|---|---|---|']
counts = {'user': {}, 'spec': {}}
for case, fn, meas, exp, got, lo, hi, note in results:
    vu = verdict(exp, got, lo, hi, USER_TOL) if lo is None else verdict(exp, got, lo, hi, None)
    vs = verdict(exp, got, lo, hi, SPEC_TOL)
    counts['user'][vu] = counts['user'].get(vu, 0) + 1
    counts['spec'][vs] = counts['spec'].get(vs, 0) + 1
    err = '' if got is None else f'{got - exp:+.1f}'
    g = '--' if got is None else f'{got:.1f}'
    lines.append(f'| {case} | {fn} | {meas} | {exp:.1f} | {g} | {err} | {vu} | {vs} | {note} |')
summary = f"\n±0.3 tolerance: {counts['user']}\nspec tolerance: {counts['spec']}\n"
open(REPORT, 'w', encoding='utf-8').write('\n'.join(lines) + '\n' + summary)
print('\n'.join(lines)); print(summary)
