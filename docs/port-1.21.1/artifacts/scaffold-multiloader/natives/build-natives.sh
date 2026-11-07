#!/usr/bin/env bash
# Build ONE self-contained JNI library for the current host platform, with LuaJIT linked in statically.
#
#   build-natives.sh <luajit-src-tree> <out-dir> <jni-source.cpp>...
#
# Output: <out-dir>/<platform>/<libname>, where platform is one of
#   windows-x86_64 (cclj.dll), linux-x86_64 / linux-aarch64 (libcclj.so), macos-x86_64 / macos-aarch64 (libcclj.dylib)
# matching NativeLoader.platformId()/libraryFileName().
#
# Requirements: JAVA_HOME (for jni.h), a C/C++ toolchain:
#   Windows: MinGW-w64 g++ + mingw32-make (MSYS2 "mingw-w64-x86_64-toolchain", or WinLibs); run from Git Bash / MSYS2.
#   Linux:   gcc/g++ + make (build inside a manylinux_2_28 container for an old-glibc baseline).
#   macOS:   Xcode command line tools (clang/clang++ + make).
set -euo pipefail

LUAJIT="$(cd "$1" && pwd)"; shift
OUT="$1"; shift
SRCS=("$@")
: "${JAVA_HOME:?JAVA_HOME must point at a JDK (for jni.h)}"
JOBS="${JOBS:-4}"

uname_s="$(uname -s)"; uname_m="$(uname -m)"
case "$uname_m" in
  x86_64|amd64) arch=x86_64 ;;
  aarch64|arm64) arch=aarch64 ;;
  *) echo "unsupported arch $uname_m" >&2; exit 1 ;;
esac
case "$uname_s" in
  MINGW*|MSYS*|CYGWIN*) os=windows ;;
  Linux) os=linux ;;
  Darwin) os=macos ;;
  *) echo "unsupported OS $uname_s" >&2; exit 1 ;;
esac
platform="$os-$arch"
mkdir -p "$OUT/$platform"
OUT="$(cd "$OUT/$platform" && pwd)"
echo "== building natives for $platform into $OUT"

# 1) LuaJIT as a static, position-independent archive.
MAKE=make; [ "$os" = windows ] && command -v mingw32-make >/dev/null && MAKE=mingw32-make
( cd "$LUAJIT/src" && $MAKE clean >/dev/null 2>&1 || true )
case "$os" in
  windows) ( cd "$LUAJIT/src" && $MAKE -j"$JOBS" BUILDMODE=static ) ;;
  linux)   ( cd "$LUAJIT/src" && $MAKE -j"$JOBS" BUILDMODE=static CFLAGS="-fPIC" ) ;;
  macos)   ( cd "$LUAJIT/src" && $MAKE -j"$JOBS" BUILDMODE=static MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}" ) ;;
esac
LIB="$LUAJIT/src/libluajit.a"
test -f "$LIB"

COMMON=(-O2 -std=c++17 -shared -I"$JAVA_HOME/include" -I"$LUAJIT/src")

# 2) The JNI library. Only Java_* (JNIEXPORT) symbols are exported; LuaJIT's lua_* stay internal so two copies of
#    Lua in one JVM (another mod, LWJGL, ...) can never clash.
case "$os" in
  windows)
    # JNIEXPORT == __declspec(dllexport) on win32, which switches off ld's export-all; x64 has no stdcall
    # decoration so --kill-at is unnecessary. -static pulls libstdc++/libgcc/winpthread in: imports are only
    # KERNEL32.dll + msvcrt.dll (or api-ms-win-crt-* with a UCRT toolchain).
    g++ "${COMMON[@]}" -I"$JAVA_HOME/include/win32" -o "$OUT/cclj.dll" "${SRCS[@]}" "$LIB" \
        -static -static-libgcc -static-libstdc++ -Wl,--no-undefined -s
    objdump -p "$OUT/cclj.dll" | grep -E "DLL Name" ;;
  linux)
    g++ "${COMMON[@]}" -fPIC -fvisibility=hidden -I"$JAVA_HOME/include/linux" -o "$OUT/libcclj.so" "${SRCS[@]}" "$LIB" \
        -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL -Wl,-z,noexecstack -Wl,--no-undefined -ldl -lm -s
    readelf -d "$OUT/libcclj.so" | grep -E "NEEDED|SONAME" || true
    objdump -T "$OUT/libcclj.so" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | sed 's/^/max glibc symbol version: /' ;;
  macos)
    clang++ "${COMMON[@]}" -I"$JAVA_HOME/include/darwin" -o "$OUT/libcclj.dylib" "${SRCS[@]}" "$LIB" \
        -mmacosx-version-min="${MACOSX_DEPLOYMENT_TARGET:-11.0}" -fvisibility=hidden \
        -Wl,-exported_symbol,'_Java_*' -Wl,-exported_symbol,'_JNI_OnLoad' -Wl,-exported_symbol,'_JNI_OnUnload'
    otool -L "$OUT/libcclj.dylib" ;;
esac
ls -la "$OUT"
