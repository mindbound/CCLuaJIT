#!/bin/bash
# Phase D, Ubuntu 26.04 (G1 item 6): Tier 1 and Tier 2 by hand against the CCLJ_DEBUG=1 library that AlmaLinux 8 built.
set -uo pipefail
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
G=<scratch>/g1
. $G/common.sh
cd <repo>
echo "host: $(. /etc/os-release; echo $PRETTY_NAME)"
[ -f natives/build/out/natives/linux-x86_64/DEBUG-BUILD.txt ] || { echo "not a debug build; stop"; exit 1; }
step tier0-debug-sanitized bash $G/ubuntu-t0.sh debug
step tier1-debug bash $G/probe-linux.sh ubuntu-tier1-debug -Xcheck:jni -Xmx256m
step tier2-debug bash $G/specs-g1.sh $JAVA_HOME ubuntu-debug
