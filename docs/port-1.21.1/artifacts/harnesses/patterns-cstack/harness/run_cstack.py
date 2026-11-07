"""Run every cstack case in its own process; report crashes with real exit codes.
usage: python -I run_cstack.py <exe> [stack_kb] [margin_kb] [noguard]
"""
import subprocess, sys, os

exe = os.path.abspath(sys.argv[1])
extra = sys.argv[2:]
cases = subprocess.run([exe, "list"], capture_output=True, text=True).stdout.split()
summary = []
for c in cases:
    try:
        p = subprocess.run([exe, c] + extra, capture_output=True, text=True, timeout=120)
        out, rc = p.stdout, p.returncode & 0xFFFFFFFF
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or ""), "TIMEOUT"
    sys.stdout.write(out)
    if rc != 0:
        msg = "TIMEOUT" if rc == "TIMEOUT" else "exit code 0x%08X%s" % (rc, " (STATUS_STACK_OVERFLOW)" if rc == 0xC00000FD else " (SIGSEGV)" if rc == 0xFFFFFFF5 or rc == 139 else "")
        sys.stdout.write("  RESULT CRASH: %s\n" % msg)
        summary.append((c, "CRASH " + msg))
    else:
        first = [l for l in out.splitlines() if l.strip().startswith("run 1:")]
        summary.append((c, first[0].strip()[7:] if first else "?"))
    sys.stdout.flush()
print("\nSUMMARY (%s %s)" % (os.path.basename(exe), " ".join(extra)))
for c, s in summary:
    print("  %-28s %s" % (c, s))
