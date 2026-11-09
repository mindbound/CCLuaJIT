#!/bin/bash
# Phase D, AlmaLinux 8 (G1 item 6): CCLJ_DEBUG=1 build, lint, Tier 0 against the assert build of LuaJIT, Tier 1 by
# hand, Tier 2 by hand (Alma; Ubuntu runs its own script against this library). Phase E (alma-e.sh) builds the
# release library again.
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
G=<scratch>/g1
. $G/common.sh
cd <repo>
step build-debug bash -c "CCLJ_DEBUG=1 bash natives/build-natives.sh linux-x86_64 > $G/logs/alma-build-debug.log 2>&1; rc=\$?; grep -E 'DEBUG BUILD|OK:|max GLIBC' $G/logs/alma-build-debug.log | head -4; exit \$rc"
cat natives/build/out/natives/linux-x86_64/DEBUG-BUILD.txt
grep -o 'LUA_USE_ASSERT\|LUA_USE_APICHECK' natives/build/linux-x86_64/luajit/cclj-build.txt | sort -u | tr '\n' ' '; echo
step lint-debug bash -c "bash natives/tests/lint-natives.sh linux-x86_64 > $G/logs/alma-lint-debug.log 2>&1; rc=\$?; tail -1 $G/logs/alma-lint-debug.log; exit \$rc"
step tier0-debug bash -c "bash natives/tests/machine-test.sh linux-x86_64 > $G/logs/alma-tier0-debug.log 2>&1; rc=\$?; grep -a -m2 -E 'xcflags|LUA_USE' $G/logs/alma-tier0-debug.log | cut -c1-200; tail -2 $G/logs/alma-tier0-debug.log; exit \$rc"
step tier1-debug bash $G/probe-linux.sh alma-tier1-debug -Xcheck:jni -Xmx256m
step tier2-debug bash $G/specs-g1.sh $JAVA_HOME alma-debug
