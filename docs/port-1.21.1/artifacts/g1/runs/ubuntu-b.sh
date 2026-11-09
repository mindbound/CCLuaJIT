#!/bin/bash
# Phase B, Ubuntu 26.04: Tier 1 by hand with the library that AlmaLinux 8 built, with -Xcheck:jni and 3 runs without.
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
G=<scratch>/g1
. $G/common.sh
cd <repo>
echo "host: $(. /etc/os-release; echo $PRETTY_NAME), $(ldd --version | head -1)"
"$JAVA_HOME/bin/java" -version 2>&1 | head -2
sha256sum natives/build/out/natives/linux-x86_64/libcclj.so
step tier1 bash $G/probe-linux.sh ubuntu-tier1 -Xcheck:jni -Xmx256m
for i in 1 2 3; do step tier1-nocheckjni-$i bash $G/probe-linux.sh ubuntu-tier1-nocheckjni-$i -Xmx256m; done
