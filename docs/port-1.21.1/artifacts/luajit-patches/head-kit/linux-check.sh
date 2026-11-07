#!/bin/bash
# Linux (WSL Ubuntu) check: build 3c4f9fe2+01-08 and ea9eb7ed+01-09, run the differential parse corpus,
# and the POSIX preemption, pattern-interrupt and C-stack harnesses on the gated HEAD build.
set -uo pipefail
W=$(cd "$(dirname "$0")" && pwd)
ORIG=<repo>/docs/port-1.21.1/artifacts/luajit-patches
H=$W/artifacts/harnesses
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'
L=$W/linux; mkdir -p $L
echo "== builds"
LUAJIT_REV=3c4f9fe2 $ORIG/build-patched-luajit.sh $W/luajit $L/ref-3c4f9fe2 > $L/build-ref.txt 2>&1; echo "ref build exit $? ($(grep -c '^applying' $L/build-ref.txt) patches)"
LUAJIT_REV=ea9eb7ed $W/artifacts/luajit-patches/build-patched-luajit.sh $W/luajit $L/head-gated > $L/build-gated.txt 2>&1; echo "gated build exit $? ($(grep -c '^applying' $L/build-gated.txt) patches, $(grep -c warning $L/build-gated.txt) warnings)"
echo "== differential parse (Linux)"
cd $W/difftest
for b in ref-3c4f9fe2 head-gated; do
  mkdir -p $L/out/$b/romdump
  for p in hand rom soup gram rommut nlsplit; do
    dd=; [ $p = rom ] && dd=$L/out/$b/romdump
    $L/$b/src/luajit parsediff.lua packs/$p.pack $L/out/$b/$p.txt $dd > /dev/null 2>&1 || echo "  $b $p exit $?"
  done
done
python3 -I compare.py $L/out/ref-3c4f9fe2 $L/out/head-gated | grep -v '^  DIFF\|^    [AB]:'
echo "-- Linux gated vs Windows gated (same corpus):"
python3 -I compare.py $L/out/head-gated $W/difftest/out/head-gated | tail -n 1
T=$L/head-gated/src
echo "== preemption harness (harness_posix.c, method intr)"
gcc -O2 -DHAVE_LUAJIT_INTERRUPT -I$T $H/preempt/harness_posix.c $T/libluajit.a -lm -ldl -lpthread -o $L/harness || exit 1
{ for c in $($L/harness intr error list); do timeout 10 $L/harness intr error "$c"; [ $? -eq 124 ] && echo "  RESULT TIMEOUT"; done; } > $L/preempt.txt 2>&1
echo "  $(grep -c 'RESULT OK' $L/preempt.txt) OK, $(grep -c 'RESULT HANG\|TIMEOUT' $L/preempt.txt) hang/timeout ($(grep -B4 'RESULT HANG' $L/preempt.txt | grep '^CASE' | tr '\n' ' '))"
echo "== pattern interrupt from a second pthread (plinux.c)"
gcc -O2 -I$T $H/patterns-cstack/linux/plinux.c $T/libluajit.a -lm -ldl -lpthread -o $L/plinux && timeout 120 $L/plinux > $L/plinux.txt 2>&1; echo "  exit $?"; sed 's/^/  /' $L/plinux.txt
echo "== C-stack harness (linux/cstack.c, 1 MB threads, 128 KB margin)"
gcc -O2 -DHAVE_CSTACKGUARD -I$T $H/patterns-cstack/linux/cstack.c $T/libluajit.a -lm -ldl -lpthread -o $L/cstack || exit 1
python3 -I $H/patterns-cstack/harness/run_cstack.py $L/cstack 1024 128 > $L/cstack.txt 2>&1
echo "  $(grep -c 'no crash' $L/cstack.txt) completed, $(grep -c 'RESULT CRASH' $L/cstack.txt) crashed"
echo "== compat_check"
$T/luajit $W/artifacts/lua/compat_check.lua 2>&1 | grep -v 'no file' | sed 's/^/  /'
