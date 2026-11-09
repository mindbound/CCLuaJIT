#!/bin/bash
# Build libcclj for one platform: the patched LuaJIT from the vendor/luajit submodule as a static library, then
# the C sources in natives/src (SRCS below), each compiled to an object and linked with it into one self-contained
# JNI library.
#
# usage: natives/build-natives.sh <platform-id>
#   windows-x86_64   Git Bash + MinGW-w64 gcc and mingw32-make        -> cclj.dll
#   linux-x86_64     manylinux_2_28 or AlmaLinux 8 (glibc 2.28)        -> libcclj.so
#   linux-aarch64    the same on an arm64 host (informational for now)  -> libcclj.so
# The platform id must match the host; there are no cross builds. macOS is not supported yet.
#
# Output:       natives/build/out/natives/<platform-id>/<lib>  (Gradle puts natives/build/out into the mod jar)
#               natives/build/out/natives/<platform-id>/*.txt  (licence notices of the code linked into <lib>, and
#                                                               DEBUG-BUILD.txt for a CCLJ_DEBUG=1 build)
# Intermediate: natives/build/<platform-id>/luajit             (patched LuaJIT tree: src/libluajit.a, the luajit
#                                                               test executable; input of tests/run-harnesses.sh)
#               natives/build/<platform-id>/luajit-build.log
#               natives/build/<platform-id>/obj                (one <source>.o per source; input of
#                                                               tests/lint-natives.sh)
# Environment:  JAVA_HOME   JDK for jni.h (default: derived from javac on PATH)
#               LUAJIT_SRC  LuaJIT git tree (default: the vendor/luajit submodule, HEAD = cclj/v2.1)
#               GLIBC_MAX   Linux: highest allowed GLIBC_ symbol version (default 2.28, the manylinux_2_28 floor)
#               CCLJ_DEBUG  1: debug build, for local testing only (CI never sets it). LuaJIT gets
#                           -DLUA_USE_ASSERT -DLUA_USE_APICHECK; the sources in natives/src are compiled with
#                           -g -O1 -DCCLJ_DEBUG instead of -O2; the library is not stripped; DEBUG-BUILD.txt is
#                           written next to it. Unset, empty or 0: release build.
# The script ends with a dependency check and fails if the library imports, needs or exports anything unexpected.
set -euo pipefail

die() { echo "build-natives: $*" >&2; exit 1; }

PLATFORM=${1:-}
case "$PLATFORM" in
  windows-x86_64) LIBNAME=cclj.dll ;;
  linux-x86_64|linux-aarch64) LIBNAME=libcclj.so ;;
  *) die "usage: build-natives.sh <platform-id>   (windows-x86_64, linux-x86_64 or linux-aarch64)" ;;
esac
case "$(uname -s)" in
  MINGW*|MSYS*) HOST_OS=windows ;;
  Linux) HOST_OS=linux ;;
  *) HOST_OS="$(uname -s)" ;;
esac
case "$(uname -m)" in
  x86_64|amd64) HOST_ARCH=x86_64 ;;
  aarch64|arm64) HOST_ARCH=aarch64 ;;
  *) HOST_ARCH="$(uname -m)" ;;
esac
[ "$PLATFORM" = "$HOST_OS-$HOST_ARCH" ] || die "$PLATFORM cannot be built on this host ($HOST_OS-$HOST_ARCH)"

# Paths. On Windows, use C:/... paths: gcc and mingw32-make are native programs.
NATIVES=$(cd "$(dirname "$0")" && pwd)
[ "$HOST_OS" = windows ] && NATIVES=$(cygpath -m "$NATIVES")
ROOT=$(dirname "$NATIVES")
LUAJIT_SRC=${LUAJIT_SRC:-$ROOT/vendor/luajit}
WORK=$NATIVES/build/$PLATFORM
OBJDIR=$WORK/obj
OUTDIR=$NATIVES/build/out/natives/$PLATFORM
LIB=$OUTDIR/$LIBNAME

# The sources in natives/src that go into the library, in link order. Each one becomes $OBJDIR/<name>.o.
SRCS=(cclj.c machine.c jni_machine.c)
missing=()
for s in "${SRCS[@]}"; do [ -f "$NATIVES/src/$s" ] || missing+=("natives/src/$s"); done
[ ${#missing[@]} = 0 ] || die "missing source file(s): ${missing[*]} (SRCS in build-natives.sh: ${SRCS[*]})"

case "${CCLJ_DEBUG:-}" in
  ""|0) DEBUG=0 ;;
  1) DEBUG=1 ;;
  *) die "CCLJ_DEBUG must be 1 (debug build), 0 or unset (release build), not '$CCLJ_DEBUG'" ;;
esac

# The top of a LuaJIT git checkout. An uninitialized submodule is an empty directory inside CCLuaJIT's own work
# tree, where plain git commands would silently use the CCLuaJIT repository instead.
[ -f "$LUAJIT_SRC/src/luajit_rolling.h" ] && [ -z "$(git -C "$LUAJIT_SRC" rev-parse --show-prefix 2>/dev/null || echo x)" ] \
  || die "$LUAJIT_SRC is not a LuaJIT git checkout (run: git submodule update --init vendor/luajit)"

if [ -z "${JAVA_HOME:-}" ]; then
  JAVAC=$(command -v javac || true)
  [ -n "$JAVAC" ] || die "set JAVA_HOME to a JDK (jni.h is needed)"
  JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$JAVAC")")")
fi
[ "$HOST_OS" = windows ] && JAVA_HOME=$(cygpath -m "$JAVA_HOME")
JAVA_HOME=${JAVA_HOME%/}
JNI_OS=$([ "$HOST_OS" = windows ] && echo win32 || echo linux)
[ -f "$JAVA_HOME/include/jni.h" ] && [ -f "$JAVA_HOME/include/$JNI_OS/jni_md.h" ] \
  || die "no JNI headers in $JAVA_HOME/include (a JRE instead of a JDK?)"

echo "== libcclj for $PLATFORM"
echo "   LuaJIT:   $LUAJIT_SRC at $(git -C "$LUAJIT_SRC" rev-parse HEAD) ($(git -C "$LUAJIT_SRC" log -1 --format=%s HEAD))"
echo "   compiler: $(gcc --version | head -1)"
echo "   JDK:      $JAVA_HOME"
if [ "$DEBUG" = 1 ]; then
  echo "=================================================================================================="
  echo "== DEBUG BUILD (CCLJ_DEBUG=1): for local testing only. Do not ship it or build a mod jar with it."
  echo "== LuaJIT: -DLUA_USE_ASSERT -DLUA_USE_APICHECK. natives/src: -g -O1 -DCCLJ_DEBUG. Not stripped."
  echo "=================================================================================================="
fi

# 1. Patched LuaJIT, static. On Linux its objects must be position-independent to go into a shared object;
#    TARGET_CFLAGS reaches the target objects only, not the host build tools (minilua, buildvm). A debug build
#    adds LuaJIT's internal assertions and API checks (build-patched-luajit.sh's third argument goes to XCFLAGS).
# A failed build must not leave an older library, or older objects for tests/lint-natives.sh, behind.
rm -rf "$OUTDIR" "$OBJDIR"
mkdir -p "$WORK"
TCFLAGS=; [ "$HOST_OS" = linux ] && TCFLAGS=-fPIC
LJ_EXTRA=; [ "$DEBUG" = 1 ] && LJ_EXTRA="-DLUA_USE_ASSERT -DLUA_USE_APICHECK"
echo "== building the patched LuaJIT (log: $WORK/luajit-build.log)"
if ! TARGET_CFLAGS="$TCFLAGS" bash "$NATIVES/luajit-patches/build-patched-luajit.sh" "$LUAJIT_SRC" "$WORK/luajit" \
     "$LJ_EXTRA" > "$WORK/luajit-build.log" 2>&1; then
  tail -30 "$WORK/luajit-build.log" >&2
  die "LuaJIT build failed"
fi
LJ=$WORK/luajit/src
INFO=$WORK/luajit/cclj-build.txt
info() { sed -n "s/^$1=//p" "$INFO"; }
echo "   $(grep -c '^applying' "$WORK/luajit-build.log") patches applied: $(info patches)"
echo "   $("$LJ/luajit" -v)"

# 2. libcclj. -fvisibility=hidden + JNIEXPORT: only the Java_* functions are exported. LuaJIT is linked from
#    libluajit.a, and the C runtime support from static libgcc, so the library needs only system libraries.
#    Each source is compiled to its own object first: tests/lint-natives.sh checks the objects with nm.
#    -fno-common: a tentative definition ("int x;") becomes a B symbol, not C (GCC 8's default is -fcommon), so
#    the lint sees it as a writable static.
read -r -a XCFLAGS <<< "$(info xcflags)"
if [ "$DEBUG" = 1 ]; then OPT=(-g -O1 -DCCLJ_DEBUG); STRIP=(); else OPT=(-O2); STRIP=(-s); fi
CFLAGS=(-std=gnu99 "${OPT[@]}" -Wall -Wextra -funwind-tables "${XCFLAGS[@]}"
        -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/$JNI_OS" -I"$LJ"
        -DCCLJ_PLATFORM="\"$PLATFORM\"" -DCCLJ_LUAJIT_COMMIT="\"$(info luajit_commit)\"")
case "$HOST_OS" in
  windows) CCFLAGS=(-fno-common) ;;
  linux) CCFLAGS=(-fno-common -pthread -fPIC -fvisibility=hidden) ;;
esac
mkdir -p "$OBJDIR"
OBJS=()
echo "== compiling ${SRCS[*]} (objects: $OBJDIR)"
for s in "${SRCS[@]}"; do
  OBJS+=("$OBJDIR/${s%.c}.o")
  gcc "${CFLAGS[@]}" "${CCFLAGS[@]}" -c -o "$OBJDIR/${s%.c}.o" "$NATIVES/src/$s"
done
mkdir -p "$OUTDIR"
echo "== linking $LIB"
case "$HOST_OS" in
  windows)
    # JNIEXPORT is __declspec(dllexport); --exclude-all-symbols stops ld from exporting anything else. -static
    # links libgcc and the mingw-w64 runtime support in, so the DLL imports only KERNEL32 and the C runtime (msvcrt
    # or UCRT). No timestamp and a fixed preferred base (ld derives one from the output path otherwise), so the
    # same sources and toolchain give the same bytes; ASLR still relocates the DLL at load time.
    gcc "${CFLAGS[@]}" -shared -o "$LIB" "${OBJS[@]}" "$LJ/libluajit.a" \
      -static -static-libgcc -Wl,--no-undefined -Wl,--exclude-all-symbols \
      -Wl,--no-insert-timestamp -Wl,--image-base=0x180000000 "${STRIP[@]}" ;;
  linux)
    # --exclude-libs,ALL hides the symbols of libluajit.a and libgcc, so two Lua copies in one JVM cannot clash.
    # The version script also hides the linker's own __bss_start, _edata and _end (binutils 2.30 exports them).
    # -pthread: the self-test starts a thread (glibc 2.28 keeps pthread_create in libpthread.so.0).
    printf '{\n  global: Java_*; JNI_OnLoad; JNI_OnUnload;\n  local: *;\n};\n' > "$WORK/libcclj.map"
    gcc "${CFLAGS[@]}" -pthread -fPIC -fvisibility=hidden -shared -o "$LIB" "${OBJS[@]}" \
      "$LJ/libluajit.a" -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script="$WORK/libcclj.map" \
      -Wl,-z,noexecstack -Wl,-z,relro,-z,now -Wl,--no-undefined -lm -ldl "${STRIP[@]}" ;;
esac

# Licence notices, next to the library wherever it goes (CI artifact, mod jar). LuaJIT's COPYRIGHT (LuaJIT, Lua
# and dlmalloc) comes from the exported tree, so it always matches the code that was linked. The Windows DLL also
# contains mingw-w64 runtime code (gdtoa, math and startup files), whose notices are in LICENSES/. Line endings are
# normalized to LF, so a Windows checkout and CI produce the same files.
tr -d '\r' < "$WORK/luajit/COPYRIGHT" > "$OUTDIR/LuaJIT-COPYRIGHT.txt"
[ "$HOST_OS" = windows ] && tr -d '\r' < "$ROOT/LICENSES/MinGW-w64-runtime.txt" > "$OUTDIR/MinGW-w64-runtime.txt"
tr -d '\r' < "$ROOT/LICENSES/LuaJIT-COPYRIGHT.txt" | cmp -s - "$OUTDIR/LuaJIT-COPYRIGHT.txt" \
  || echo "warning: LICENSES/LuaJIT-COPYRIGHT.txt differs from COPYRIGHT of the LuaJIT that was built; update it" >&2

# A debug library carries its marker wherever it goes, so it cannot pass for a release build.
if [ "$DEBUG" = 1 ]; then
  {
    echo "DEBUG BUILD of $LIBNAME (CCLJ_DEBUG=1): for local testing only. Do not ship it."
    echo "LuaJIT:      $(info luajit_commit), XCFLAGS: $(info xcflags)"
    echo "natives/src: ${SRCS[*]}, compiled with ${OPT[*]} -fno-common"
    echo "not stripped; built by build-natives.sh $PLATFORM with $(gcc --version | head -1)"
  } > "$OUTDIR/DEBUG-BUILD.txt"
fi

# 3. Dependency check.
echo "== dependency check"
fail=0
case "$HOST_OS" in
  windows)
    imports=$(objdump -p "$LIB" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
    echo "imports (objdump -p):"; sed 's/^/  /' <<< "$imports"
    bad=$(grep -viE '^(KERNEL32|msvcrt|ucrtbase|api-ms-win-crt-[a-z0-9-]+)\.dll$' <<< "$imports" || true)
    [ -z "$bad" ] || { echo "UNEXPECTED IMPORT: $bad"; fail=1; }
    exports=$(objdump -p "$LIB" | tr -d '\r' | awk '/\[Ordinal\/Name Pointer\] Table/ {t = 1; next}
                                                   t && /^[[:space:]]*$/ {t = 0} t && /^[[:space:]]*\[/ {print $NF}')
    ;;
  linux)
    echo "ldd:"; ldd "$LIB" | sed 's/^/  /'
    needed=$(readelf -d "$LIB" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    bad=$(grep -vE '^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|ld-linux[-a-z0-9_.]*\.so\.[0-9]+)$' \
          <<< "$needed" || true)
    [ -z "$bad" ] || { echo "UNEXPECTED NEEDED: $bad"; fail=1; }
    max=$(objdump -T "$LIB" | grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -1 || true)
    [ -n "$max" ] || { echo "NO GLIBC_ SYMBOL VERSIONS (objdump -T): not a glibc build?"; fail=1; }
    limit=GLIBC_${GLIBC_MAX:-2.28}
    echo "max GLIBC symbol version (objdump -T): $max (allowed: $limit)"
    [ "$(printf '%s\n%s\n' "$max" "$limit" | sort -V | tail -1)" = "$limit" ] || { echo "GLIBC TOO NEW: $max"; fail=1; }
    stack=$(readelf -lW "$LIB" | awk '$1 == "GNU_STACK" {print $(NF - 1)}')  # flags column, before the alignment
    echo "GNU_STACK flags: $stack"
    [ "$stack" = RW ] || { echo "EXECUTABLE STACK REQUESTED"; fail=1; }
    exports=$(nm -D --defined-only "$LIB" | awk '{print $NF}')
    ;;
esac
echo "exports:"; sed 's/^/  /' <<< "$exports"
bad=$(grep -vE '^(Java_|JNI_On(Load|Unload)$)' <<< "$exports" || true)
[ -z "$bad" ] || { echo "UNEXPECTED EXPORT: $bad"; fail=1; }
[ -n "$exports" ] || { echo "NO EXPORTS"; fail=1; }
if [ "$fail" != 0 ]; then
  rm -rf "$OUTDIR"  # so Gradle cannot package it
  die "dependency check failed for $LIB (removed)"
fi
echo "== OK: $LIB ($(wc -c < "$LIB" | tr -d ' ') bytes, sha256 $(sha256sum "$LIB" | cut -c1-16)...)"
echo "   with $(cd "$OUTDIR" && ls | grep -v -x "$LIBNAME" | tr '\n' ' ')"
[ "$DEBUG" = 0 ] || echo "== DEBUG BUILD (CCLJ_DEBUG=1): see $OUTDIR/DEBUG-BUILD.txt. Do not ship it."
