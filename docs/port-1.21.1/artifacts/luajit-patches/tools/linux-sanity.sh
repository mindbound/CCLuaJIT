#!/bin/bash
# Linux x64 (WSL) sanity check of a LuaJIT tree + CCLuaJIT patches 01-08.
# usage: linux-sanity.sh <plain-luajit-tree (LF)> <out-dir> <artifacts-dir> <syntax_probe.lua>
set -uo pipefail
SRC=$1; O=$2; ART=$3; PROBE=$4
mkdir -p "$O/results" "$O/bin"; R=$O/results; B=$O/bin
"$ART/luajit-patches/build-patched-luajit.sh" "$SRC" "$O/luajit" > "$R/build.txt" 2>&1 || { echo "BUILD FAILED"; tail -20 "$R/build.txt"; exit 1; }
T=$O/luajit/src
echo "build: OK ($(grep -c '^applying' "$R/build.txt") patches applied), $(grep -c 'warning:' "$R/build.txt") warnings; $("$T/luajit" -v)"
# Preemption harness (POSIX shim). Same case loop as verify-patch-stack.sh.
gcc -O2 -DHAVE_LUAJIT_INTERRUPT -I"$T" "$ART/harnesses/preempt/harness_posix.c" "$T/libluajit.a" -lpthread -lm -ldl -o "$B/harness" || exit 1
{
  for c in $("$B/harness" intr error list); do
    timeout 10 "$B/harness" intr error "$c"; [ $? -eq 124 ] && echo "  RESULT TIMEOUT (process killed after 10 s)"
  done
  echo "== yield";  timeout 60 "$B/harness" intr yield x
  for c in pcall_swallow xpcall_handler_loop coroutine_swallow; do
    echo "== hardabort $c error"; timeout 20 "$B/harness" intr hardabort $c 300
    echo "== hardabort $c yield"; timeout 20 "$B/harness" intr hardabort $c 300 yield
  done
} > "$R/preempt.txt" 2>&1
echo "preempt harness (Linux): $(grep -c 'RESULT OK' "$R/preempt.txt") OK, $(grep -c 'RESULT HANG\|TIMEOUT' "$R/preempt.txt") hang: $(awk '/^CASE /{c=$2} /RESULT HANG|TIMEOUT/{printf "%s ", c}' "$R/preempt.txt")"
# Pattern/sort interrupts from a second pthread (patch 07).
gcc -O2 -I"$T" "$ART/harnesses/patterns-cstack/linux/plinux.c" "$T/libluajit.a" -lpthread -lm -ldl -o "$B/plinux" || exit 1
timeout 120 "$B/plinux" > "$R/plinux.txt" 2>&1
echo "plinux: $(grep -c 'fired=1' "$R/plinux.txt")/5 fired, $(grep -c 'usable afterwards: hell0 w0rld' "$R/plinux.txt")/5 usable afterwards"
# C-stack guard (patch 08), 1 MB pthread stacks, 128 KB margin.
gcc -O2 -DHAVE_CSTACKGUARD -I"$T" "$ART/harnesses/patterns-cstack/linux/cstack.c" "$T/libluajit.a" -lpthread -lm -ldl -o "$B/cstack" || exit 1
python3 -I "$ART/harnesses/patterns-cstack/harness/run_cstack.py" "$B/cstack" 1024 128 > "$R/cstack.txt" 2>&1
echo "cstack harness (Linux): $(grep -c 'no crash' "$R/cstack.txt") completed, $(grep -c 'RESULT CRASH' "$R/cstack.txt") crashed"
"$T/luajit" "$ART/lua/compat_check.lua" > "$R/compat.txt" 2>&1; echo "compat_check:"; grep -av "no file" "$R/compat.txt" | sed 's/^/  /'
"$T/luajit" "$PROBE" > "$R/syntax-probe.txt" 2>&1; echo "syntax probe: $(tail -1 "$R/syntax-probe.txt")"
