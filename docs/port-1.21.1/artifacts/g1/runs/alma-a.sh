#!/bin/bash
# Phase A, AlmaLinux 8: release build, lint, Tier 0, scan self-test, Tier 1 by hand with -Xcheck:jni, then 3 runs
# without it (D1).
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
G=<scratch>/g1
. $G/common.sh
cd <repo>
echo "commit: $(git rev-parse HEAD 2>/dev/null || echo '(git n/a)')"
echo "host: $(cat /etc/redhat-release), $(gcc --version | head -1), $(ldd --version | head -1)"
"$JAVA_HOME/bin/java" -version 2>&1 | head -2
step build-release bash -c "bash natives/build-natives.sh linux-x86_64 > $G/logs/alma-build-release.log 2>&1; rc=\$?; grep -iE 'OK:|max GLIBC|warning|error' $G/logs/alma-build-release.log | head; exit \$rc"
step lint bash -c "bash natives/tests/lint-natives.sh linux-x86_64 > $G/logs/alma-lint.log 2>&1; rc=\$?; tail -2 $G/logs/alma-lint.log; exit \$rc"
step tier0 bash -c "bash natives/tests/machine-test.sh linux-x86_64 > $G/logs/alma-tier0.log 2>&1; rc=\$?; grep -a -E '^note|sanitiz' $G/logs/alma-tier0.log | head -3; tail -2 $G/logs/alma-tier0.log; exit \$rc"
step scan-self-test bash -c "bash natives/tests/jni-scan.sh --self-test > $G/logs/alma-selftest.log 2>&1; rc=\$?; tail -1 $G/logs/alma-selftest.log; exit \$rc"
step tier1 bash $G/probe-linux.sh alma-tier1 -Xcheck:jni -Xmx256m
for i in 1 2 3; do step tier1-nocheckjni-$i bash $G/probe-linux.sh alma-tier1-nocheckjni-$i -Xmx256m; done
sha256sum natives/build/out/natives/linux-x86_64/libcclj.so
