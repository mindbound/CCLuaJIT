"""Extract bios.lua and every rom/**/*.lua from a CC:T core jar into <outdir> (data only)."""
import os, sys, zipfile
PREFIX = "data/computercraft/lua/"
jar, outdir = sys.argv[1], sys.argv[2]
n = 0
with zipfile.ZipFile(jar) as z:
    names = [x for x in z.namelist() if x.startswith(PREFIX) and x.endswith(".lua")]
    names = [PREFIX + "bios.lua"] + sorted(x for x in names if x.startswith(PREFIX + "rom/"))
    with open(os.path.join(outdir, "MANIFEST.txt"), "w", newline="\n") as m:
        for name in names:
            rel = name[len(PREFIX):]
            dst = os.path.join(outdir, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(z.read(name))
            m.write(rel + "\n")
            n += 1
print("extracted %d files (%d rom/ + bios.lua)" % (n, n - 1))
