#!/bin/bash
# G1 item 4 on Windows: build the mutant from a scratch copy, put it into natives/build/out for one
# ./gradlew :common:bridgeProbe, then restore the release library and check it.
set -uo pipefail
G=<scratch>/g8
. $G/common.sh
REPO=<repo>
N=$G/negctl-win
DLL=$REPO/natives/build/out/natives/windows-x86_64/cclj.dll
REL=80957d1e98f36281
cd $REPO
[ "$(sha256sum < $DLL | cut -c1-16)" = $REL ] || { echo "the release DLL is not the phase A build; stop"; exit 1; }
python "$(cygpath -w $G/negctl.py)" "$(cygpath -w $N)" || exit 1
diff -u natives/src/jni_machine.c $N/natives/src/jni_machine.c
step build-mutant bash -c "LUAJIT_SRC=$REPO/vendor/luajit bash $N/natives/build-natives.sh windows-x86_64 > $G/logs/negctl-win-build.log 2>&1; rc=\$?; grep -E 'OK:' $G/logs/negctl-win-build.log; exit \$rc"
MUT=$N/natives/build/out/natives/windows-x86_64/cclj.dll
[ -f $MUT ] || { echo "no mutant library"; exit 1; }
cp $DLL $G/release-cclj.dll
restore() { cp $G/release-cclj.dll $DLL; echo "restored: $(sha256sum < $DLL | cut -c1-64)"; }
trap restore EXIT
cp $MUT $DLL
echo "in natives/build/out now: mutant $(sha256sum < $DLL | cut -c1-16)"
echo "=== gradle bridgeProbe with the mutant ($(date +%H:%M:%S))"
./gradlew :common:bridgeProbe 2>&1 | grep -a -v -E '^(WARNING in native method|\s+at )' > $G/logs/negctl-win-gradle.log
echo "gradle exit: ${PIPESTATUS[0]}"
grep -a -E '^(bridgeProbe|FAIL|BUILD|\* What went wrong|Execution failed)|JNI scan' $G/logs/negctl-win-gradle.log | cut -c1-300 | head -30
TL=$REPO/common/build/headless/logs/bridgeProbe.log
echo "task log: $(wc -c < $TL) bytes; lines 'without checking exceptions ... from CallObjectMethod': $(grep -a -c 'JNI call made without checking exceptions when required to from CallObjectMethod' $TL); 'WARNING in native method' lines in all: $(grep -a -c '^WARNING in native method' $TL)"
grep -a -m3 '^WARNING in native method' $TL
head -c 2000000 $TL > $G/logs/negctl-win-task-head.log; rm -f $TL
trap - EXIT; restore
[ "$(sha256sum < $DLL | cut -c1-16)" = $REL ] && echo "release DLL back: OK" || echo "release DLL NOT restored"
echo "=== gradle bridgeProbe with the release library again ($(date +%H:%M:%S))"
./gradlew :common:bridgeProbe > $G/logs/negctl-win-after.log 2>&1; echo "gradle exit: $?"
grep -a -E '^(bridgeProbe:|BUILD)' $G/logs/negctl-win-after.log
sha256sum common/build/resources/main/natives/windows-x86_64/cclj.dll
