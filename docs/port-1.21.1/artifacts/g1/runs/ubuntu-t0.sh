#!/bin/bash
# Tier 0 on Ubuntu 26.04 (gcc 15 with libasan): machine.c and machine_test.c with ASan and UBSan, linked with the
# libluajit.a that AlmaLinux 8 built (natives/build/linux-x86_64/luajit). $1 = label.
set -uo pipefail
G=<scratch>/g1
cd <repo>
echo "host: $(. /etc/os-release; echo $PRETTY_NAME), $(gcc --version | head -1)"
grep -o 'LUA_USE_ASSERT\|LUA_USE_APICHECK' natives/build/linux-x86_64/luajit/cclj-build.txt | sort -u | tr '\n' ' '; echo "(LuaJIT tree flags)"
bash natives/tests/machine-test.sh linux-x86_64 > $G/logs/ubuntu-tier0-$1.log 2>&1; echo "machine-test exit: $?"
grep -a -E '^== compiling|^== running|^note' $G/logs/ubuntu-tier0-$1.log | cut -c1-200
tail -2 $G/logs/ubuntu-tier0-$1.log
