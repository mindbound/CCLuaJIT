#!/bin/bash
# Run the LuaJIT regression harnesses against a patched LuaJIT build: CI gate G0 and every change to the patches.
#
# usage: natives/tests/run-harnesses.sh <luajit-build-src-dir> [work-dir]
#   <luajit-build-src-dir>  src/ of a tree built by natives/luajit-patches/build-patched-luajit.sh: libluajit.a, the
#                           luajit executable and the headers. build-natives.sh leaves one in
#                           natives/build/<platform-id>/luajit/src. The tree root is accepted too.
#   [work-dir]              harness binaries and logs (default: natives/build/<platform-id>/harnesses, i.e. next
#                           to the LuaJIT tree)
#
# Windows (Git Bash or MSYS2 MINGW64 + MinGW-w64 gcc, Python 3 as `python`) and Linux (gcc, `python3`, 3.6 or
# newer). macOS (Apple clang as `gcc`) runs too, for the informational CI job only: cstack is skipped there (SKIP,
# not FAIL). The expected results below were measured on x86_64 Windows and Linux. In the first CI run's
# informational job, linux-aarch64 and macos-aarch64 gave the same results, except cstack (see below). Suites:
#   preempt   preemption harness: 28 loop shapes interrupted from a second thread, a yield test and 6 hard-abort
#             cases. Expected: 33 OK, and exactly X_finalizer_loop and X_lua_hook_loop hang (the sandbox removes
#             newproxy and debug.sethook, which they need).
#   patterns  patch 07, pattern matching and sort interrupted from a second thread. Windows: pharness, 74 OK.
#             Linux: plinux, 5/5 interrupted and 5/5 usable afterwards.
#   cstack    patch 08, 31 recursion shapes on 1 MB threads with a 128 KB red zone: 31 completed, 0 crashed.
#             Linux arm64 without patch 08's VM fast-path check (no LJ_HASCSTACKGUARD_FF): 27 completed, and
#             exactly the 4 cases that recurse only through the stock coroutine.resume/wrap crash with SIGSEGV.
#             Patch 08 checks that fast path only on x64 with GC64; see CSTACK_FF_GAP below.
#   compat    compat_check.lua output equals lua/compat_check.expected.
#   syntax    syntax_probe.lua: SUMMARY ok=45 bad=0 (v3.0 syntax rejected, Lua 5.2 syntax accepted).
#   fixcheck  fixcheck.lua -Ohotloop=1: the 6 upstream fixes of cclj/v2.1 are present (6 PASS, 0 FAIL).
# Exits 0 only if every suite gives exactly the expected result.
set -uo pipefail

die() { echo "run-harnesses: $*" >&2; exit 2; }

[ $# -ge 1 ] || die "usage: run-harnesses.sh <luajit-build-src-dir> [work-dir]"
case "$(uname -s)" in
  MINGW*|MSYS*) OS=windows; EXE=.exe; PY=python; LIBS=() ;;
  Linux) OS=linux; EXE=; PY=python3; LIBS=(-lpthread -lm -ldl) ;;
  Darwin) OS=macos; EXE=; PY=python3; LIBS=(-lpthread -lm -ldl) ;;
  *) die "unsupported OS $(uname -s): Windows (Git Bash or MSYS2 + MinGW), Linux or macOS" ;;
esac
# The suites run each case under coreutils `timeout` (exit status 124 when it fires). macOS has none unless
# coreutils is installed: use its `gtimeout`, or else the same thing in perl.
if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then
    timeout() { gtimeout "$@"; }
  else
    timeout() {
      perl -e 'my $s = shift; my $pid = fork; defined $pid or die "fork: $!";
               if (!$pid) { exec @ARGV or exit 127 }
               $SIG{ALRM} = sub { kill "TERM", $pid; sleep 1; kill "KILL", $pid; waitpid $pid, 0; exit 124 };
               alarm $s; waitpid $pid, 0; alarm 0;
               exit($? & 127 ? 128 + ($? & 127) : $? >> 8)' "$@"
    }
  fi
fi
# Native Windows programs (gcc, the harnesses, python) get C:/... paths.
abspath() { local p; p=$(cd "$1" && pwd) || return 1; if [ "$OS" = windows ]; then cygpath -m "$p"; else echo "$p"; fi; }

LJ=$(abspath "$1") || die "no such directory: $1"
[ -f "$LJ/libluajit.a" ] || { [ -f "$LJ/src/libluajit.a" ] && LJ=$LJ/src; } || true
[ -f "$LJ/libluajit.a" ] && [ -x "$LJ/luajit$EXE" ] && [ -f "$LJ/lj_obj.h" ] \
  || die "$1 is not a built LuaJIT src directory (libluajit.a, luajit$EXE, headers)"
TREE=$(dirname "$LJ")
HERE=$(abspath "$(dirname "$0")")
W=${2:-$(dirname "$TREE")/harnesses}
mkdir -p "$W" || die "cannot create $W"
W=$(abspath "$W")
B=$W/bin; R=$W/results
rm -rf "$B" "$R" "$W/lua"; mkdir -p "$B" "$R" "$W/lua"
LUAJIT=$LJ/luajit$EXE
if [ "$OS" != macos ]; then  # macOS skips the C-stack harness
  command -v "$PY" >/dev/null 2>&1 && "$PY" -I -c 'import sys; sys.exit(sys.version_info < (3, 6))' \
    || die "$PY (Python 3.6 or newer) is needed for the C-stack harness"
fi

# Compile with the feature flags of the library: the preemption and pattern harnesses read G(L)->vmstate through
# LuaJIT's internal lj_obj.h, whose global_State layout depends on them.
XCFLAGS=$(sed -n 's/^xcflags=//p' "$TREE/cclj-build.txt" 2>/dev/null)
[ -n "$XCFLAGS" ] || XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT"
read -r -a XCF <<< "$XCFLAGS"
cc() { local out=$1; shift; gcc -O2 "${XCF[@]}" -I"$LJ" "$@" "$LJ/libluajit.a" "${LIBS[@]}" -o "$out"; }

echo "== LuaJIT: $("$LUAJIT" -v) ($LJ)"
[ -f "$TREE/cclj-build.txt" ] && sed 's/^/   /' "$TREE/cclj-build.txt"
# The page size matters for the C-stack margin under a JVM (DESIGN.md 3.2). Not on Windows: Cygwin/MSYS getconf
# reports the 64 KiB allocation granularity there, not the page size.
if [ "$OS" = windows ]; then echo "== work dir: $W ($OS, $(uname -m))"
else echo "== work dir: $W ($OS, $(uname -m), page size $(getconf PAGESIZE 2>/dev/null || echo unknown))"; fi

FAILED=()
SKIPPED=()
SUMMARY=()
verdict() {  # verdict <suite> <0|1 passed> <details>
  local s=PASS
  [ "$2" = 1 ] || { s=FAIL; FAILED+=("$1"); }
  SUMMARY+=("$(printf '%-9s %s  %s' "$1" "$s" "$3")")
  printf '%-9s %s  %s\n' "$1" "$s" "$3"
}
skipped() {  # skipped <suite> <reason>: the suite cannot run on this platform; does not fail the run
  SKIPPED+=("$1")
  SUMMARY+=("$(printf '%-9s %s  %s' "$1" SKIP "$2")")
  printf '%-9s %s  %s\n' "$1" SKIP "$2"
}

# ---- preempt ----------------------------------------------------------------------------------------------------
echo "== preempt (log: $R/preempt.txt)"
if [ "$OS" = windows ]; then
  cc "$B/harness.exe" -DHAVE_LUAJIT_INTERRUPT "$HERE/preempt/harness.c" -lwinmm
else
  cc "$B/harness" -DHAVE_LUAJIT_INTERRUPT "$HERE/preempt/harness_posix.c"
fi
if [ -x "$B/harness$EXE" ]; then
  H=$B/harness$EXE
  {
    for c in $("$H" intr error list | tr -d '\r'); do
      timeout 10 "$H" intr error "$c"; [ $? -eq 124 ] && echo "  RESULT TIMEOUT (process killed after 10 s)"
    done
    echo "== yield"; timeout 60 "$H" intr yield x; [ $? -eq 124 ] && echo "  RESULT TIMEOUT (killed after 60 s)"
    for c in pcall_swallow xpcall_handler_loop coroutine_swallow; do
      echo "== hardabort $c error"; timeout 20 "$H" intr hardabort $c 300
      [ $? -eq 124 ] && echo "  RESULT TIMEOUT (killed after 20 s)"
      echo "== hardabort $c yield"; timeout 20 "$H" intr hardabort $c 300 yield
      [ $? -eq 124 ] && echo "  RESULT TIMEOUT (killed after 20 s)"
    done
  } > "$R/preempt.txt" 2>&1
  P=$(tr -d '\r' < "$R/preempt.txt")
  total=$(grep -c 'RESULT ' <<< "$P"); ok=$(grep -c 'RESULT OK' <<< "$P")
  hangs=$(awk '/^CASE |^YIELDTEST|^== / {c = $2} /RESULT (HANG|TIMEOUT)/ {print c}' <<< "$P" | sort | tr '\n' ' ')
  pass=0
  [ "$total" = 35 ] && [ "$ok" = 33 ] && [ "$hangs" = "X_finalizer_loop X_lua_hook_loop " ] && pass=1
  verdict preempt $pass "$ok OK of $total results, hangs: ${hangs:-none} (expected 33 OK of 35, hangs: X_finalizer_loop X_lua_hook_loop)"
else
  verdict preempt 0 "harness did not build"
fi

# ---- patterns ---------------------------------------------------------------------------------------------------
echo "== patterns (log: $R/patterns.txt)"
if [ "$OS" = windows ]; then
  "$LUAJIT" "$HERE/patterns/genkiller.lua" "$HERE/patterns" 100000 "$W/lua/killer100k.txt" \
    && "$LUAJIT" "$HERE/patterns/genkiller.lua" "$HERE/patterns" 32000 "$W/lua/killer32k.txt" \
    && cc "$B/pharness.exe" "$HERE/patterns/pharness.c" -lwinmm
  if [ -x "$B/pharness.exe" ] && [ -s "$W/lua/killer32k.txt" ]; then
    {
      export PH_DIR=$W
      for m in error pcall hard hardyield pause; do
        for c in $("$B/pharness.exe" $m list | tr -d '\r'); do
          timeout 60 "$B/pharness.exe" $m $c; rc=$?
          [ $rc -eq 124 ] && echo "CASE $c [$m]" && echo "  RESULT TIMEOUT (killed after 60 s)"
        done
      done
    } > "$R/patterns.txt" 2>&1
    P=$(tr -d '\r' < "$R/patterns.txt")
    total=$(grep -c 'RESULT ' <<< "$P"); ok=$(grep -c 'RESULT OK' <<< "$P")
    pass=0; [ "$total" = 74 ] && [ "$ok" = 74 ] && pass=1
    verdict patterns $pass "pharness: $ok OK of $total results (expected 74 OK of 74)"
  else
    verdict patterns 0 "pharness or its killer inputs did not build"
  fi
else
  cc "$B/plinux" "$HERE/patterns/plinux.c"
  if [ -x "$B/plinux" ]; then
    timeout 120 "$B/plinux" > "$R/patterns.txt" 2>&1; rc=$?
    fired=$(grep -c 'fired=1' "$R/patterns.txt"); usable=$(grep -c 'usable afterwards: hell0 w0rld' "$R/patterns.txt")
    cases=$(grep -c '^case ' "$R/patterns.txt")
    pass=0; [ $rc = 0 ] && [ "$cases" = 5 ] && [ "$fired" = 5 ] && [ "$usable" = 5 ] && pass=1
    verdict patterns $pass "plinux: $fired/$cases interrupted, $usable/$cases usable afterwards, exit $rc (expected 5/5, 5/5, exit 0)"
  else
    verdict patterns 0 "plinux did not build"
  fi
fi

# ---- cstack -----------------------------------------------------------------------------------------------------
echo "== cstack (log: $R/cstack.txt)"
if [ "$OS" = macos ]; then
  skipped cstack "cstack.c reads its thread's stack bounds with pthread_getattr_np, which macOS does not have (a pthread_get_stackaddr_np port is needed, PLAN.md 5.2 G0)"
elif cc "$B/cstack$EXE" -DHAVE_CSTACKGUARD "$HERE/cstack/cstack.c" && [ -x "$B/cstack$EXE" ]; then
  "$PY" -I "$HERE/cstack/run_cstack.py" "$B/cstack$EXE" 1024 128 > "$R/cstack.txt" 2>&1
  P=$(tr -d '\r' < "$R/cstack.txt")
  cases=$("$B/cstack$EXE" list | tr -d '\r' | grep -c .)
  done_=$(grep -c 'RESULT COMPLETED (no crash)' <<< "$P"); crashed=$(grep -c 'RESULT CRASH' <<< "$P")
  total=$(grep -c 'RESULT ' <<< "$P")
  results="$done_ completed, $crashed crashed, $total results for $cases cases"
  # Patch 08 checks the C stack in the interpreter's coroutine.resume/wrap fast path only where lj_arch.h defines
  # LJ_HASCSTACKGUARD_FF: x64 with GC64. On Linux arm64 the 4 cases in CSTACK_FF_GAP, which recurse only through
  # the stock coroutine.resume/wrap, overflow the native stack with SIGSEGV (first CI run, PLAN.md section 6).
  # Accept exactly that, and only there. x64 must have the check: losing it is a regression. Any other target
  # keeps the strict expectation, because nothing else has been measured. Preprocess lj_arch.h with the build's
  # XCFLAGS, as LuaJIT's Makefile does to choose the VM's features (tr: MinGW gcc writes CRLF).
  archdefs=$(gcc "${XCF[@]}" -E -dM "$LJ/lj_arch.h" 2> "$R/lj_arch-dM.err" | tr -d '\r')
  arch=$(sed -n 's/^#define LJ_ARCH_NAME "\(.*\)"$/\1/p' <<< "$archdefs")
  ff=0; grep -q '^#define LJ_HASCSTACKGUARD_FF 1$' <<< "$archdefs" && ff=1
  if [ -z "$arch" ]; then
    verdict cstack 0 "$results; could not preprocess $LJ/lj_arch.h to tell the target and whether patch 08's VM fast-path check is built in (gcc errors: $R/lj_arch-dM.err)"
  elif [ "$arch" = x64 ] && [ $ff = 0 ]; then
    verdict cstack 0 "$results; this x64 build lacks patch 08's VM fast-path check (lj_arch.h does not define LJ_HASCSTACKGUARD_FF: GC64 off, or patch 08 changed)"
  elif [ "$arch" = arm64 ] && [ $ff = 0 ] && [ "$OS" = linux ]; then
    CSTACK_FF_GAP="coroutine_precreated_chain coroutine_resume_nesting coroutine_wrap_nesting stock_resume_stays_resumable"
    # Every crashed case, sorted. A SIGSEGV shows as the bare name; any other crash (a TIMEOUT, another exit code)
    # carries its result in parentheses, so it never equals CSTACK_FF_GAP and the message names it.
    crashes=$(awk '/^CASE / {c = $2}
                   /RESULT CRASH: / {s = $0; sub(/.*RESULT CRASH: /, "", s)
                                     print (s ~ /^exit code 0x[0-9A-F]+ \(SIGSEGV\)$/ ? c : c " (" s ")")}' <<< "$P" \
                | LC_ALL=C sort | tr '\n' ' ')
    crashes=${crashes% }
    pass=0
    [ "$cases" = 31 ] && [ "$done_" = 27 ] && [ "$crashed" = 4 ] && [ "$total" = 31 ] && [ "$crashes" = "$CSTACK_FF_GAP" ] \
      && pass=1
    verdict cstack $pass "$done_ completed, $crashed crashed: ${crashes:-none}, $total results for $cases cases (Linux arm64 without patch 08's VM fast-path check, expected: 27 completed, 4 crashed with SIGSEGV: $CSTACK_FF_GAP)"
  else
    pass=0; [ "$cases" = 31 ] && [ "$done_" = 31 ] && [ "$crashed" = 0 ] && [ "$total" = 31 ] && pass=1
    verdict cstack $pass "$results (expected 31 completed, 0 crashed)"
  fi
else
  verdict cstack 0 "cstack did not build"
fi

# ---- compat, syntax, fixcheck ------------------------------------------------------------------------------------
echo "== compat, syntax, fixcheck (logs: $R/compat.txt, syntax.txt, fixcheck.txt)"
timeout 120 "$LUAJIT" "$HERE/lua/compat_check.lua" > "$R/compat.txt" 2>&1
tr -d '\r\000' < "$R/compat.txt" | grep -av $'^\t' > "$R/compat.normalized.txt"
tr -d '\r' < "$HERE/lua/compat_check.expected" | grep -v '^#' > "$R/compat.expected.txt"
if diff "$R/compat.expected.txt" "$R/compat.normalized.txt" > "$R/compat.diff.txt"; then
  verdict compat 1 "$(grep -c . "$R/compat.normalized.txt") lines equal compat_check.expected"
else
  sed 's/^/  /' "$R/compat.diff.txt"
  verdict compat 0 "differs from compat_check.expected (diff: $R/compat.diff.txt)"
fi

timeout 120 "$LUAJIT" "$HERE/lua/syntax_probe.lua" > "$R/syntax.txt" 2>&1
summary=$(tr -d '\r' < "$R/syntax.txt" | grep '^SUMMARY' | tail -1)
pass=0; grep -q '^SUMMARY ok=45 bad=0 ' <<< "$summary" && pass=1
verdict syntax $pass "${summary:-no SUMMARY line} (expected ok=45 bad=0)"

timeout 120 "$LUAJIT" -Ohotloop=1 "$HERE/lua/fixcheck.lua" > "$R/fixcheck.txt" 2>&1
P=$(tr -d '\r' < "$R/fixcheck.txt")
fpass=$(grep -c '^PASS ' <<< "$P"); ffail=$(grep -c '^FAIL ' <<< "$P")
pass=0; [ "$fpass" = 6 ] && [ "$ffail" = 0 ] && pass=1
verdict fixcheck $pass "$fpass PASS, $ffail FAIL (expected 6 PASS, 0 FAIL)"

echo
echo "== summary ($OS, $("$LUAJIT" -v | cut -d' ' -f1-2))"
printf '%s\n' "${SUMMARY[@]}"
if [ ${#FAILED[@]} -gt 0 ]; then
  echo "FAILED: ${FAILED[*]}"
  exit 1
fi
if [ ${#SKIPPED[@]} -gt 0 ]; then
  echo "ALL PASSED that ran (skipped on $OS: ${SKIPPED[*]})"
else
  echo "ALL PASSED"
fi
