"""Side-by-side table of the handwritten cases: 3c4f9fe2+01-08, gated HEAD, ungated HEAD, Cobalt 0.9.9."""
import sys
def load(path):
    r = {}
    for ln in open(path, encoding="latin-1").read().split("\n"):
        if ln and not ln.startswith("@@END"):
            cid, rest = ln.split("\t", 1)
            if rest.startswith("OK"):
                rest = "OK"
            else:
                rest = rest.split("\t", 1)[1]
                # drop the "<chunkname>:" prefix so Cobalt's shortened names compare
                rest = rest.split(":", 1)[1] if ":" in rest else rest
            r[cid] = rest
    return r
ref, gat, ng, cob = (load("out/%s/hand.txt" % b) for b in ("ref-3c4f9fe2", "head-gated", "head-nogate", "cobalt-0.9.9"))
mode = sys.argv[1]
ids = [c for c in ref if c.startswith(mode)]
print("%-34s | %-6s | %-60s | %-45s | %s" % ("case", "ref==gated", "3c4f9fe2+01-08 (= gated HEAD)", "ungated HEAD+01-08", "Cobalt 0.9.9"))
for c in ids:
    same = "same" if ref[c] == gat[c] else "DIFF"
    print("%-34s | %-6s | %-60s | %-45s | %s" % (c, same, ref[c][:60], ng[c][:45], cob[c][:70]))
if mode == "l52.":
    print("\nl52 cases where Cobalt's accept/reject differs from 3c4f9fe2+01-08:")
    for c in ids:
        if (ref[c] == "OK") != (cob[c] == "OK"):
            print("  %-32s ref: %-50s cobalt: %s" % (c, ref[c][:50], cob[c][:70]))
else:
    print("\nnew.* accepted by Cobalt: %s" % [c for c in ids if cob[c] == "OK"])
