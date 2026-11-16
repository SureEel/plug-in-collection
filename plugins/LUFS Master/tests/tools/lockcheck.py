import sys, csv
sys.path.insert(0, sys.argv[1])
sys.argv = [sys.argv[0], sys.argv[1], '']
from kcheck_lib import integ, official
from ref import read, tp
rows = list(csv.DictReader(open(sys.argv[1] + r'\ebu_lock.csv', encoding='utf-8-sig')))
print(f"{'file':12s} {'target':>6s} {'gain':>7s} {'predicted':>9s} {'out I (BS.1770 official)':>25s} {'error':>7s} {'within 0.1':>10s} {'TP 4x':>7s} {'<= -1.0':>8s} {'GR max':>7s}")
for r in rows:
    x, sr = read(sys.argv[1] + rf"\ebu_lock\{r['file']}_{r['target'].lstrip('-')}_locked.wav")
    I = integ(x, sr, official); t = float(r['target']); tp4 = tp(x, 4)
    print(f"{r['file']:12s} {t:6.0f} {r['lock_gain']:>7s} {r['predicted']:>9s} {I:25.3f} {I-t:+7.3f} {'PASS' if abs(I-t) <= 0.1 else 'FAIL':>10s} {tp4:7.2f} {'PASS' if tp4 <= -1.0 else 'FAIL':>8s} {r['grmax']:>7s}")
