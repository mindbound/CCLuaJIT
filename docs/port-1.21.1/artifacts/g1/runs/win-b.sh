#!/bin/bash
# Phase B, Windows: the Gradle tasks of G1 items 2, 3 and 5 (release library), then 3 runs of bridgeProbe without
# -Xcheck:jni (item 8), then the test class path for the WSL runs of MachineSpecs.
set -uo pipefail
G=<scratch>/g1
S=$(dirname "$G")
. $G/common.sh
cd <repo>
echo "commit: $(git rev-parse HEAD); dirty files: $(git status --porcelain | wc -l)"
save() { rm -rf "$G/logs/$1"; mkdir -p "$G/logs/$1"; cp -r common/build/headless/logs "$G/logs/$1/" 2>/dev/null; cp common/build/headless/*.tsv "$G/logs/$1/" 2>/dev/null; true; }
step gradle-release bash -c "./gradlew --continue :common:nativeProbe :common:bridgeProbe :common:machineSpecs :common:headlessSmoke :common:headlessSuite > $G/logs/win-gradle-release.log 2>&1; rc=\$?; grep -a -E '^(> Task|BUILD|bridgeProbe:|machineSpecs:|ns per call|jni scan|.*suite)' $G/logs/win-gradle-release.log | grep -v 'UP-TO-DATE\|NO-SOURCE' | cut -c1-200; exit \$rc"
save win-gradle-release
for i in 1 2 3; do
  step gradle-nocheckjni-$i bash -c "./gradlew :common:bridgeProbe -PcheckJni=false > $G/logs/win-gradle-nocheckjni-$i.log 2>&1; rc=\$?; grep -a -E '^(BUILD|bridgeProbe:|ns per call|java )' $G/logs/win-gradle-nocheckjni-$i.log | cut -c1-260; exit \$rc"
  cp common/build/headless/logs/bridgeProbe.log "$G/logs/win-gradle-nocheckjni-$i.task.log"
done
step print-cp bash -c "./gradlew -q -I $S/print-cp.gradle :common:printTestClasspath | sed -n 's/^CP=//p' > $G/cp-g1.txt; wc -c < $G/cp-g1.txt"
sha256sum natives/build/out/natives/windows-x86_64/cclj.dll common/build/resources/main/natives/windows-x86_64/cclj.dll common/build/resources/main/natives/linux-x86_64/libcclj.so
