"""Summarise benchmark logs into markdown tables. Usage: python -I summarize.py <logdir> <suffix> [<suffix> ...]
Per run (suffix) we take per-runtime statistics, then report the median across runs."""
import sys, os, statistics, collections, math

logdir = sys.argv[1]
suffixes = sys.argv[2:] or ["r1"]
ORDER = ["fib", "mandel", "tables", "oop", "strings", "sha256", "framebuf", "serialize", "lzw", "compile"]


def parse(path):
    res = collections.defaultdict(list)
    rnd = 1
    if not os.path.exists(path):
        return res
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if p[0] == "ROUND":
            rnd = int(p[1])
        if p[0] == "RESULT" and len(p) >= 6:
            res[p[2]].append((rnd, int(p[3]), float(p[4]), p[5]))
        if p[0] == "MACHINE_ERROR" or "error" in line.lower() and p[0] != "RESULT":
            print("WARN in", os.path.basename(path), line.strip()[:200], file=sys.stderr)
    return res


med = lambda xs: statistics.median(xs) if xs else None
rows = collections.defaultdict(lambda: collections.defaultdict(list))
checks = collections.defaultdict(dict)
for suf in suffixes:
    L = lambda n: parse(os.path.join(logdir, f"{n}_{suf}.log"))
    logs = {k: L(k) for k in ["lua54", "luajit_jit", "luajit_jit_u32", "luajit_joff", "luajit_joff_hook", "luajit_jit_hook",
                              "cobalt_fresh", "cobalt_shared", "luajit_jit_profli10", "luajit_jit_proffi10"]}
    for b in ORDER:
        r = rows[b]
        its = logs["cobalt_fresh"][b]
        if its:
            r["cobalt_cold"].append(its[0][2])
            r["cobalt_warm"].append(med([t for (_, i, t, _) in its if i >= 4]))
            r["cobalt_best"].append(min(t for (_, i, t, _) in its))
            checks[b]["cobalt"] = its[0][3]
        sh = logs["cobalt_shared"][b]
        if sh:
            r["cobalt_shared"].append(med([t for (rd, i, t, _) in sh if rd == 2]))
        for key, log, skip_first in (("jit", "luajit_jit", True), ("jit_u32", "luajit_jit_u32", True), ("joff", "luajit_joff", False),
                                     ("joff_hook", "luajit_joff_hook", False), ("jit_hook", "luajit_jit_hook", True),
                                     ("jit_profl", "luajit_jit_profli10", True), ("jit_proff", "luajit_jit_proffi10", True),
                                     ("lua54", "lua54", False)):
            d = logs[log][b]
            if d:
                r[key].append(med([t for (_, i, t, _) in d if (i >= 2 or not skip_first)]))
                checks[b][key] = d[0][3]
                if key == "jit":
                    r["jit_first"].append(d[0][2])


def agg(b, k):
    v = rows[b].get(k)
    return med(v) if v else None


def f(x, nd=3):
    return "-" if x is None else f"{x:.{nd}f}"


print(f"Runs aggregated: {', '.join(suffixes)} (each cell = median over runs of the per-run statistic)\n")
cols = [("cobalt_cold", "Cobalt cold (1st iter, fresh JVM)"), ("cobalt_warm", "Cobalt warm (median iters 4-10)"),
        ("cobalt_best", "Cobalt best iter"), ("cobalt_shared", "Cobalt warm, shared JVM (2nd pass)"),
        ("jit_first", "LuaJIT JIT 1st iter"), ("jit", "LuaJIT JIT"), ("joff", "LuaJIT -joff"),
        ("joff_hook", "-joff + count hook/1000"), ("jit_hook", "JIT + count hook/1000"),
        ("jit_profl", "JIT + jit.profile 'l'"), ("jit_proff", "JIT + jit.profile 'f'"), ("lua54", "PUC Lua 5.4")]
print("| benchmark | " + " | ".join(c[1] for c in cols) + " |")
print("|---" * (len(cols) + 1) + "|")
for b in ORDER:
    print(f"| {b} | " + " | ".join(f(agg(b, c[0])) for c in cols) + " |")
print("\n(all times in seconds per run)\n")

sp = [("jit", "cobalt_warm", "JIT vs Cobalt warm"), ("jit", "cobalt_best", "JIT vs Cobalt best"),
      ("jit", "cobalt_shared", "JIT vs Cobalt shared-JVM"), ("jit", "cobalt_cold", "JIT vs Cobalt cold"),
      ("joff", "cobalt_warm", "-joff vs Cobalt warm"), ("joff", "cobalt_best", "-joff vs Cobalt best"),
      ("joff_hook", "cobalt_warm", "-joff+hook vs Cobalt warm"), ("jit_profl", "cobalt_warm", "JIT+profile'l' vs Cobalt warm"),
      ("lua54", "cobalt_warm", "PUC 5.4 vs Cobalt warm")]
print("Speedup = Cobalt time / other time (higher = faster than Cobalt)\n")
print("| benchmark | " + " | ".join(s[2] for s in sp) + " |")
print("|---" * (len(sp) + 1) + "|")
geo = collections.defaultdict(list)
for b in ORDER:
    cells = []
    for i, (a, base, _) in enumerate(sp):
        x, y = agg(b, a), agg(b, base)
        s = (y / x) if (x and y) else None
        if s: geo[i].append(s)
        cells.append("-" if s is None else f"{s:.1f}x")
    print(f"| {b} | " + " | ".join(cells) + " |")
gm = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")
print("| **geomean** | " + " | ".join(f"**{gm(geo[i]):.1f}x**" for i in range(len(sp))) + " |")
print("| min..max | " + " | ".join((f"{min(geo[i]):.1f}..{max(geo[i]):.1f}x" if geo[i] else "-") for i in range(len(sp))) + " |")

if agg("sha256", "jit_u32"):
    print(f"\nsha256 on LuaJIT JIT with a correct unsigned bit32 shim: {agg('sha256','jit_u32'):.3f} s vs {agg('sha256','jit'):.3f} s with bit32=bit (signed, CCLuaJIT behaviour)")
print("\nChecksums (must agree across runtimes):")
for b in ORDER:
    vals = set(checks[b].values())
    print(f"- {b}: {'OK' if len(vals) == 1 else 'MISMATCH'} {dict(checks[b]) if len(vals) != 1 else next(iter(vals), '')}")
