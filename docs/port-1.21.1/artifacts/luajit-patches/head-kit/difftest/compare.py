"""Compare parsediff.lua outputs of two builds.

usage: python -I compare.py <out-dir-A> <out-dir-B> [nogate-out-dir]
Prints, per pack: case count, OK/ERR counts on each side, and every case whose line differs
(compile success, error text, or bytecode dump length+FNV hash). For rom, also compares the full
string.dump() files byte for byte. With [nogate-out-dir], also reports how many new.* cases the
ungated HEAD build accepts (they should all be new syntax, except the few that upstream rejects too).
Exit status 1 if anything differs.
"""
import os
import sys

A, B = sys.argv[1], sys.argv[2]
NG = sys.argv[3] if len(sys.argv) > 3 else None
PACKS = ["hand", "rom", "soup", "gram", "rommut", "nlsplit"]


def load(d, p):
    res = {}
    order = []
    end = None
    with open(os.path.join(d, p + ".txt"), "rb") as f:
        for ln in f.read().decode("latin-1").split("\n"):
            if not ln:
                continue
            cid, rest = ln.split("\t", 1)
            if cid == "@@END":
                end = int(rest)
                continue
            res[cid] = rest
            order.append(cid)
    assert end == len(order), (d, p, end, len(order))
    return res, order


ndiff = 0
for p in PACKS:
    a, order = load(A, p)
    b, _ = load(B, p)
    assert set(a) == set(b)
    cnt = lambda r: (sum(v.startswith("OK\t") for v in r.values()), sum(v.startswith("ERR\t") for v in r.values()),
                     sum(v.startswith("THROW\t") for v in r.values()))
    diffs = [c for c in order if a[c] != b[c]]
    print("%-7s cases=%-6d A ok/err/throw=%s B ok/err/throw=%s differences=%d" % (p, len(order), cnt(a), cnt(b), len(diffs)))
    for c in diffs[:50]:
        print("  DIFF %s\n    A: %s\n    B: %s" % (c, a[c], b[c]))
    ndiff += len(diffs)
    if p == "rom":
        da, db = os.path.join(A, "romdump"), os.path.join(B, "romdump")
        fa, fb = sorted(os.listdir(da)), sorted(os.listdir(db))
        same = 0
        for fn in fa:
            if fn in fb and open(os.path.join(da, fn), "rb").read() == open(os.path.join(db, fn), "rb").read():
                same += 1
        print("  rom string.dump files: A=%d B=%d byte-identical=%d" % (len(fa), len(fb), same))
        if not (len(fa) == len(fb) == same):
            ndiff += 1

if NG:
    a, order = load(A, "hand")
    g, _ = load(NG, "hand")
    news = [c for c in order if c.startswith("new.")]
    acc = [c for c in news if g[c].startswith("OK\t")]
    rej_both = [c for c in news if not g[c].startswith("OK\t")]
    print("new.* cases: %d; ungated HEAD accepts %d; ungated HEAD rejects %d:" % (len(news), len(acc), len(rej_both)))
    for c in rej_both:
        print("  %s  ungated: %s" % (c, g[c]))
    ref_ok = [c for c in news if a[c].startswith("OK\t")]
    print("new.* cases accepted by A: %d %s" % (len(ref_ok), ref_ok))
print("TOTAL DIFFERENCES: %d" % ndiff)
sys.exit(1 if ndiff else 0)
