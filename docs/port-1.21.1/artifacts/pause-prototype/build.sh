#!/bin/bash
# Builds pausehost.exe (release LuaJIT) and pausehost-assert.exe (LUA_USE_ASSERT+APICHECK LuaJIT).
# The host includes lj_obj.h, so it must use the same feature flags as the library.
set -e
cd "$(dirname "$0")"
# Must match the flags libluajit.a was built with (build-patched-luajit.sh); the 01-06 era build used only the first four LUAJIT flags.
FLAGS="-D_WIN32_WINNT=0x0A00 -DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT"
gcc -O2 -g -Wall -Wno-unused-function $FLAGS -Iluajit/src -o pausehost.exe pausehost.c cclj_co.c luajit/src/libluajit.a -lwinmm
gcc -O2 -g -Wall -Wno-unused-function $FLAGS -DLUA_USE_ASSERT -DLUA_USE_APICHECK -Iluajit-assert/src -o pausehost-assert.exe pausehost.c cclj_co.c luajit-assert/src/libluajit.a -lwinmm
echo built
