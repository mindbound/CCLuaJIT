"""Alternate builds round-robin; per round each build runs patbench (best of 3); report medians.
usage: python -I benchrun.py <rounds> <name=luajit.exe> ...  (bench script path via env PATBENCH)"""
import subprocess, sys, os, statistics
rounds = int(sys.argv[1])
builds = [a.split('=', 1) for a in sys.argv[2:]]
script = os.environ['PATBENCH']
res = {n: {} for n, _ in builds}
for r in range(rounds):
    for n, exe in (builds if r % 2 == 0 else builds[::-1]):
        out = subprocess.run([exe, script, '3'], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            k, t, v = line.split('\t')
            res[n].setdefault(k, []).append(float(t))
names = [n for n, _ in builds]
keys = list(res[names[0]].keys())
print('%-20s ' % 'benchmark' + ' '.join('%10s' % n for n in names) + ''.join('  %12s' % ('%s/%s' % (n, names[0])) for n in names[1:]))
ratios = {n: [] for n in names[1:]}
for k in keys:
    meds = [statistics.median(res[n][k]) for n in names]
    row = '%-20s ' % k + ' '.join('%10.4f' % m for m in meds)
    for n, m in zip(names[1:], meds[1:]):
        rr = m / meds[0] if meds[0] > 0 else float('nan')
        ratios[n].append(rr)
        row += '  %+11.1f%%' % ((rr - 1) * 100)
    print(row)
import math
for n in names[1:]:
    g = math.exp(sum(math.log(x) for x in ratios[n]) / len(ratios[n]))
    print('geomean %s vs %s: %+.2f%%  (runs per build: %d rounds x best-of-3)' % (n, names[0], (g - 1) * 100, rounds))
