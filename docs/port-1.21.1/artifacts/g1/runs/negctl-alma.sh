#!/bin/bash
# G1 item 4 on AlmaLinux 8, the CI natives-job way: build the mutant in the scratch copy, run BridgeProbe by hand under
# -Xcheck:jni with stdout+stderr in one log, then jni-scan.sh on that log. The repo's natives/build is not touched.
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
G=<scratch>/g1
. $G/common.sh
REPO=<repo>
N=$G/negctl-lin
cd $REPO
grep -n "NEGATIVE CONTROL" $N/natives/src/jni_machine.c || { echo "no mutant source"; exit 1; }
step build-mutant bash -c "LUAJIT_SRC=$REPO/vendor/luajit bash $N/natives/build-natives.sh linux-x86_64 > $G/logs/negctl-alma-build.log 2>&1; rc=\$?; grep -E 'OK:' $G/logs/negctl-alma-build.log; exit \$rc"
out=/tmp/cclj-g1-negctl; rm -rf $out
"$JAVA_HOME/bin/javac" -d $out/classes common/src/main/java/io/github/mindbound/cclj/natives/*.java \
  common/src/test/java/io/github/mindbound/cclj/natives/NativeProbe.java \
  common/src/test/java/io/github/mindbound/cclj/natives/BridgeProbe.java || exit 2
"$JAVA_HOME/bin/java" -Xcheck:jni -Xmx256m -cp "$out/classes:$N/natives/build/out" io.github.mindbound.cclj.natives.BridgeProbe \
  $out/cache 2>&1 | tee $out/bridge-probe.log > /dev/null
echo "java exit: ${PIPESTATUS[0]}"
echo "log: $(wc -c < $out/bridge-probe.log) bytes"
bash natives/tests/jni-scan.sh $out/bridge-probe.log > $out/scan.txt 2>&1; echo "scan exit: $?"
grep -v '^  ' $out/scan.txt | head -5 | cut -c1-300
grep -a -E '^(FAIL|bridgeProbe:|library build)' $out/bridge-probe.log | cut -c1-300
head -c 2000000 $out/bridge-probe.log > $G/logs/negctl-alma-head.log
head -c 200000 $out/scan.txt > $G/logs/negctl-alma-scan-head.txt
rm -rf $out
