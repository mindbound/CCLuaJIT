# Resolve the 55853647 cherry-pick conflict in src/lj_asm_x86.h the same way upstream merge 47fc4e39 did:
# keep the v2.1 code (ours), but replace "ra_noreg(...) ? ra_alloc1(...) : ra_scratch(...)" with ra_scratch(),
# and in asm_tobit move the asm_fuseload() after emit_rr() exactly as v2.1 already has it.
import re, sys
p = sys.argv[1]
raw = open(p, encoding="utf-8", newline="").read()
crlf = "\r\n" in raw
s = raw.replace("\r\n", "\n")
pat = r"<<<<<<< HEAD\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n"
def repl(m):
    ours = m.group(1)
    out = re.sub(r"Reg tmp = ra_noreg\(IR\(ir->op1\)->r\) \?\n\t      ra_alloc1\(as, ir->op1, RSET_FPR\) :\n\t      ra_scratch\(as, RSET_FPR\);\n",
                 "Reg tmp = ra_scratch(as, RSET_FPR);\n", ours)
    out = re.sub(r"Reg tmp = ra_noreg\(IR\(lref\)->r\) \? ra_alloc1\(as, lref, RSET_FPR\) :\n\t\t\t\t\t  ra_scratch\(as, RSET_FPR\);\n",
                 "Reg tmp = ra_scratch(as, RSET_FPR);\n", out)
    assert out != ours, "pattern not found"
    return out
s2, n = re.subn(pat, repl, s, flags=re.S)
assert n == 2, n
if crlf: s2 = s2.replace("\n", "\r\n")
open(p, "w", encoding="utf-8", newline="").write(s2)
print("resolved", n, "hunks")
