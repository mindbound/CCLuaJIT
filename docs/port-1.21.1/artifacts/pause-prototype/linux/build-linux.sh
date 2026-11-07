#!/bin/bash
# Run inside WSL/Linux: builds patched LuaJIT (static) and pausehost-linux.
set -e
cd "$(dirname "$0")"
FL="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD"
if [ ! -f luajit/src/libluajit.a.linux ]; then
  (cd luajit/src && make clean >/dev/null && make -j24 BUILDMODE=static XCFLAGS="$FL" >../build-linux.log 2>&1 && cp libluajit.a libluajit.a.linux)
fi
gcc -O2 -g -Wall -Wno-unused-function -D_GNU_SOURCE $FL -Iluajit/src -o pausehost-linux ../pausehost.c ../cclj_co.c luajit/src/libluajit.a.linux -lm -ldl -lpthread
echo built-linux
