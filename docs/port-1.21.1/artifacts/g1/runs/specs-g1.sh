#!/bin/bash
# MachineSpecs (Tier 2) under WSL with the class path Gradle printed on Windows, the Linux library taken from
# natives/build/out (first on the class path), JVM arguments as the machineSpecs task. $1 = JAVA_HOME, $2 = label.
set -uo pipefail
G=<scratch>/g8
cd <repo>
CP=$(tr -d '\r\n' < "$G/cp-g8.txt" | tr '\\;' '/:' | sed -e 's|C:/|/mnt/c/|g')
out=/tmp/cclj-g8-specs-$2; rm -rf "$out"; mkdir -p "$out/tmp"
"$1/bin/java" -version 2>&1 | head -1
sha256sum natives/build/out/natives/linux-x86_64/libcclj.so
[ -f natives/build/out/natives/linux-x86_64/DEBUG-BUILD.txt ] && echo "library: DEBUG build" || echo "library: release build"
"$1/bin/java" -Xcheck:jni -Djava.io.tmpdir="$out/tmp" -Dcct.version=1.121.0 -Dheadless.bootTimeout=15 \
  -Dccluajit.nativeDir="$out/natives" -cp "natives/build/out:$CP" io.github.mindbound.cclj.headless.MachineSpecs \
  --report "$G/logs/specs-$2.tsv" > "$G/logs/specs-$2.log" 2>&1
echo "java exit: $?"
grep -a -E '^(FAIL |problem:|machineSpecs:|natives:)|ns per call' "$G/logs/specs-$2.log" | cut -c1-240
echo "ok lines: $(grep -a -c '^ok ' "$G/logs/specs-$2.log")"
bash natives/tests/jni-scan.sh "$G/logs/specs-$2.log"; echo "scan exit: $?"
rm -rf "$out"
