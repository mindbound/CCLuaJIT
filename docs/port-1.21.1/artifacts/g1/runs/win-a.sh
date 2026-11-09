#!/bin/bash
# Phase A, Windows (Git Bash): release build, lint, Tier 0, scan self-test. No Gradle.
set -uo pipefail
G=<scratch>/g8
. $G/common.sh
cd <repo>
echo "commit: $(git rev-parse HEAD); dirty files: $(git status --porcelain | wc -l)"
echo "host: $(uname -s), $(gcc --version | head -1)"; java -version 2>&1 | head -2
step build-release bash -c "bash natives/build-natives.sh windows-x86_64 > $G/logs/win-build-release.log 2>&1; rc=\$?; grep -iE 'OK:|warning|error|imports|exports' $G/logs/win-build-release.log | head; exit \$rc"
step lint bash -c "bash natives/tests/lint-natives.sh windows-x86_64 > $G/logs/win-lint.log 2>&1; rc=\$?; tail -2 $G/logs/win-lint.log; exit \$rc"
step tier0 bash -c "bash natives/tests/machine-test.sh windows-x86_64 > $G/logs/win-tier0.log 2>&1; rc=\$?; grep -a -E '^note|sanitiz' $G/logs/win-tier0.log | head -3; tail -2 $G/logs/win-tier0.log; exit \$rc"
step scan-self-test bash -c "bash natives/tests/jni-scan.sh --self-test > $G/logs/win-selftest.log 2>&1; rc=\$?; tail -1 $G/logs/win-selftest.log; exit \$rc"
sha256sum natives/build/out/natives/windows-x86_64/cclj.dll
