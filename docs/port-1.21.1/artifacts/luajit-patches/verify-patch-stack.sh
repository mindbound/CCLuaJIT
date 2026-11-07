#!/bin/bash
# Build the patched LuaJIT (01-08, recommended flags) and run the regression harnesses against it.
# Windows (Git Bash + MinGW gcc) only for now: the pattern harness uses the Win32 API.
# On Linux use ../harnesses/preempt/harness_posix.c (with win32_on_posix.h) and ../harnesses/patterns-cstack/linux/.
#
# usage: verify-patch-stack.sh <pristine-luajit-git-tree> <work-dir>
#   e.g. LUAJIT_REV=3c4f9fe2 verify-patch-stack.sh vendor/luajit /tmp/cclj-verify
# Output: <work-dir>/results/*.txt and a one-line verdict per suite on stdout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ART=$(cd "$HERE/.." && pwd)
SRC=$1; W=$2
mkdir -p "$W/results" "$W/bin" "$W/lua"
R=$W/results; B=$W/bin

"$HERE/build-patched-luajit.sh" "$SRC" "$W/luajit" > "$R/build.txt" 2>&1 || { echo "BUILD FAILED (see $R/build.txt)"; exit 1; }
T=$W/luajit/src
echo "build: OK ($(grep -c '^applying' "$R/build.txt") patches applied)"

# Preemption harness: 25 loop shapes + 3 cases that must still hang without the sandbox's removals.
gcc -O2 -DHAVE_LUAJIT_INTERRUPT -I"$T" "$ART/harnesses/preempt/harness.c" "$T/libluajit.a" -lwinmm -o "$B/harness.exe" || exit 1
{
  for c in $("$B/harness.exe" intr error list | tr -d '\r'); do
    timeout 10 "$B/harness.exe" intr error "$c"; [ $? -eq 124 ] && echo "  RESULT TIMEOUT (process killed after 10 s)"
  done
  echo "== yield";  timeout 60 "$B/harness.exe" intr yield x
  for c in pcall_swallow xpcall_handler_loop coroutine_swallow; do
    echo "== hardabort $c error"; timeout 20 "$B/harness.exe" intr hardabort $c 300
    echo "== hardabort $c yield"; timeout 20 "$B/harness.exe" intr hardabort $c 300 yield
  done
} > "$R/preempt.txt" 2>&1
ok=$(grep -c 'RESULT OK' "$R/preempt.txt"); hang=$(grep -c 'RESULT HANG\|TIMEOUT' "$R/preempt.txt")
echo "preempt harness: $ok OK, $hang hang (expected: 33 OK; 2 hang = X_finalizer_loop and X_lua_hook_loop, which the sandbox removes)"

# Pattern / sort interrupt harness (patch 07).
cp "$ART/harnesses/patterns-cstack/lua/"*.lua "$W/lua/"
"$T/luajit.exe" "$W/lua/genkiller.lua" "$W/lua" 100000 "$W/lua/killer100k.txt"
"$T/luajit.exe" "$W/lua/genkiller.lua" "$W/lua" 32000 "$W/lua/killer32k.txt"
gcc -O2 -I"$T" "$ART/harnesses/patterns-cstack/harness/pharness.c" "$T/libluajit.a" -lwinmm -o "$B/pharness.exe" || exit 1
{
  export PH_DIR=$W
  for m in error pcall hard hardyield pause; do
    for c in $("$B/pharness.exe" $m list | tr -d '\r'); do
      timeout 60 "$B/pharness.exe" $m $c; rc=$?
      [ $rc -eq 124 ] && echo "CASE $c [$m]" && echo "  RESULT TIMEOUT (killed after 60 s)"
    done
  done
} > "$R/pharness.txt" 2>&1
echo "pattern harness: $(grep -c 'RESULT OK' "$R/pharness.txt") OK, $(grep -c 'TIMEOUT\|FAIL\|HANG' "$R/pharness.txt") timeout/fail/hang (expected: 74 OK, 0)"

# C-stack guard harness (patch 08): every recursion shape must end in a catchable error, never a crash.
gcc -O2 -DHAVE_CSTACKGUARD -I"$T" "$ART/harnesses/patterns-cstack/harness/cstack.c" "$T/libluajit.a" -o "$B/cstack.exe" || exit 1
python -I "$ART/harnesses/patterns-cstack/harness/run_cstack.py" "$B/cstack.exe" 1024 128 > "$R/cstack.txt" 2>&1
echo "cstack harness: $(grep -c 'no crash' "$R/cstack.txt") completed, $(grep -c 'RESULT CRASH' "$R/cstack.txt") crashed (expected: 31 completed, 0 crashed)"

# Lua-level compatibility checks.
"$T/luajit.exe" "$ART/lua/compat_check.lua" > "$R/compat.txt" 2>&1
echo "compat_check:"; sed "s/^/  /" "$R/compat.txt"
