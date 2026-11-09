#!/bin/bash
# Phase E, AlmaLinux 8: the release library again (natives/build/out ends as a release build), lint, Tier 0, Tier 1.
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
G=<scratch>/g1
. $G/common.sh
cd <repo>
step build-release bash -c "bash natives/build-natives.sh linux-x86_64 > $G/logs/alma-build-release2.log 2>&1; rc=\$?; grep -E 'OK:|max GLIBC' $G/logs/alma-build-release2.log; exit \$rc"
ls natives/build/out/natives/linux-x86_64/
step lint bash -c "bash natives/tests/lint-natives.sh linux-x86_64 > $G/logs/alma-lint2.log 2>&1; rc=\$?; tail -1 $G/logs/alma-lint2.log; exit \$rc"
step tier0 bash -c "bash natives/tests/machine-test.sh linux-x86_64 > $G/logs/alma-tier0-2.log 2>&1; rc=\$?; tail -1 $G/logs/alma-tier0-2.log; exit \$rc"
step tier1 bash $G/probe-linux.sh alma-tier1-final -Xcheck:jni -Xmx256m
sha256sum natives/build/out/natives/linux-x86_64/libcclj.so
