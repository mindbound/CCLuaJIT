#!/bin/bash
# usage: probe-linux.sh <log-name> [java args...]  (JAVA_HOME: a JDK 21). As CI's Tier 1 step: javac of the natives
# package + NativeProbe + BridgeProbe into /tmp, BridgeProbe against natives/build/out, stdout+stderr in one log,
# then jni-scan.sh on it.
set -uo pipefail
cd <repo>
G=<scratch>/g8
name=$1; shift
out=/tmp/cclj-g8-probe-$name
rm -rf "$out"
"$JAVA_HOME/bin/javac" -d "$out/classes" common/src/main/java/io/github/mindbound/cclj/natives/*.java \
  common/src/test/java/io/github/mindbound/cclj/natives/NativeProbe.java \
  common/src/test/java/io/github/mindbound/cclj/natives/BridgeProbe.java || { echo "javac failed"; exit 2; }
"$JAVA_HOME/bin/java" "$@" -cp "$out/classes:natives/build/out" io.github.mindbound.cclj.natives.BridgeProbe \
  "$out/cache" 2>&1 | tee "$G/logs/$name.log" > /dev/null
rc=$?
echo "java exit: $rc"
bash natives/tests/jni-scan.sh "$G/logs/$name.log"; src=$?
echo "scan exit: $src"
grep -a -E "^(FAIL|ns per call|bridgeProbe|library build|java )" "$G/logs/$name.log" | cut -c1-300
rm -rf "$out"
[ $rc = 0 ] && [ $src = 0 ]
