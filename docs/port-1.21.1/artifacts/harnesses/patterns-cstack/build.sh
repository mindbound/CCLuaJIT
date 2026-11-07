#!/bin/bash
# usage: build.sh <tree> [extra XCFLAGS]  -- clean static build with the CCLuaJIT flags
T=$1; shift
cd "$T/src" && mingw32-make clean >/dev/null 2>&1
mingw32-make -j16 BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD $*" > ../build.log 2>&1
rc=$?
grep -iE "warning|error" ../build.log | grep -v "^mingw32-make" | head -20
tail -1 ../build.log
exit $rc
