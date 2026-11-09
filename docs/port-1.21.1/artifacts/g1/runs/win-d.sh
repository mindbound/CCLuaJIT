#!/bin/bash
# Phase D, Windows (G1 item 6): CCLJ_DEBUG=1 build, lint, Tier 0 against the assert build of LuaJIT, the Gradle tasks
# of Tier 1 and Tier 2; then the release build again, lint, and the same Gradle tasks, so natives/build/out ends as a
# release build.
set -uo pipefail
G=<scratch>/g1
. $G/common.sh
cd <repo>
echo "commit: $(git rev-parse HEAD); dirty files: $(git status --porcelain | wc -l)"
save() { rm -rf "$G/logs/$1"; mkdir -p "$G/logs/$1"; cp -r common/build/headless/logs "$G/logs/$1/" 2>/dev/null; cp common/build/headless/*.tsv "$G/logs/$1/" 2>/dev/null; true; }
step build-debug bash -c "CCLJ_DEBUG=1 bash natives/build-natives.sh windows-x86_64 > $G/logs/win-build-debug.log 2>&1; rc=\$?; grep -E 'DEBUG BUILD|OK:' $G/logs/win-build-debug.log | head -4; exit \$rc"
cat natives/build/out/natives/windows-x86_64/DEBUG-BUILD.txt
grep -o 'LUA_USE_ASSERT\|LUA_USE_APICHECK' natives/build/windows-x86_64/luajit/cclj-build.txt | sort -u | tr '\n' ' '; echo
step lint-debug bash -c "bash natives/tests/lint-natives.sh windows-x86_64 > $G/logs/win-lint-debug.log 2>&1; rc=\$?; tail -1 $G/logs/win-lint-debug.log; exit \$rc"
step tier0-debug bash -c "bash natives/tests/machine-test.sh windows-x86_64 > $G/logs/win-tier0-debug.log 2>&1; rc=\$?; grep -a -m2 -E 'xcflags|LUA_USE' $G/logs/win-tier0-debug.log | cut -c1-200; tail -2 $G/logs/win-tier0-debug.log; exit \$rc"
step gradle-debug bash -c "./gradlew --continue :common:nativeProbe :common:bridgeProbe :common:machineSpecs > $G/logs/win-gradle-debug.log 2>&1; rc=\$?; grep -a -E '^(BUILD|bridgeProbe: [0-9]|machineSpecs: [0-9]|library build)|JNI scan|natives: ' $G/logs/win-gradle-debug.log | cut -c1-200; exit \$rc"
save win-gradle-debug
step build-release bash -c "bash natives/build-natives.sh windows-x86_64 > $G/logs/win-build-release2.log 2>&1; rc=\$?; grep -E 'OK:' $G/logs/win-build-release2.log; exit \$rc"
ls natives/build/out/natives/windows-x86_64/
step lint bash -c "bash natives/tests/lint-natives.sh windows-x86_64 > $G/logs/win-lint2.log 2>&1; rc=\$?; tail -1 $G/logs/win-lint2.log; exit \$rc"
step tier0 bash -c "bash natives/tests/machine-test.sh windows-x86_64 > $G/logs/win-tier0-2.log 2>&1; rc=\$?; tail -1 $G/logs/win-tier0-2.log; exit \$rc"
step gradle-release bash -c "./gradlew --continue :common:nativeProbe :common:bridgeProbe :common:machineSpecs > $G/logs/win-gradle-release2.log 2>&1; rc=\$?; grep -a -E '^(BUILD|bridgeProbe: [0-9]|machineSpecs: [0-9]|library build)|JNI scan|natives: ' $G/logs/win-gradle-release2.log | cut -c1-200; exit \$rc"
save win-gradle-release2
sha256sum natives/build/out/natives/windows-x86_64/cclj.dll
git status --porcelain | head
