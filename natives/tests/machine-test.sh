#!/bin/bash
# Tier 0 of S1: the native machine core (natives/src/machine.c) without a JVM (docs/port-1.21.1/M1.md section 3.15).
#
# usage: natives/tests/machine-test.sh <platform-id>
#   windows-x86_64   Git Bash or MSYS2 MINGW64 + MinGW-w64 gcc and mingw32-make
#   linux-x86_64     AlmaLinux 8 / manylinux_2_28, or a current distro
#   linux-aarch64    the same on arm64
# The platform id must match the host.
#
# 1. Builds an assert and API-check LuaJIT (-DLUA_USE_ASSERT -DLUA_USE_APICHECK) from the vendor/luajit submodule
#    with the patches, into natives/build/<platform-id>/luajit-assert. A tree built earlier is reused when its
#    cclj-build.txt names the current LuaJIT commit and the assert flags, and its stamp (the patches, the build
#    script, the compiler and, on Linux, the C library) still matches.
# 2. Compiles machine.c and tests/machine/machine_test.c with -DCCLJ_TEST (the cm_test_* hooks), -DCCLJ_DEBUG (the
#    stack-balance checks) and the xcflags of that tree, and links them with its libluajit.a. Any compiler or
#    linker warning stops the script. On Linux it first tries -fsanitize=address,undefined. When the sanitized
#    program cannot be built (no libasan, as on AlmaLinux 8), the sanitizers are dropped with a note. When it
#    builds but fails its --smoke run (one machine end to end), the script fails and prints the sanitizer report:
#    a script cannot tell a bug in machine.c from a problem of ASan with LuaJIT's GC64 mode (M1.md section 6).
#    Only when a person has found that the report is such a problem, and not a bug, rerun with
#    MACHINE_TEST_SANITIZE=0. MinGW has no ASan.
# 3. Runs the test under a timeout (MACHINE_TEST_TIMEOUT seconds, default 900).
# Output: natives/build/<platform-id>/machine-test/ (the program and its log), the LuaJIT build log next to it.
# Environment: LUAJIT_SRC (default: the vendor/luajit submodule), MACHINE_TEST_SANITIZE=0 to skip the sanitizers.
# Exit 0: every check passed. 1: a check failed, the sanitized smoke run failed, or the test did not finish.
# 2: usage error, build failure or compiler warnings.
set -euo pipefail

die() { echo "machine-test: $*" >&2; exit 2; }

PLATFORM=${1:-}
case "$PLATFORM" in
  windows-x86_64|linux-x86_64|linux-aarch64) ;;
  *) die "usage: machine-test.sh <platform-id>   (windows-x86_64, linux-x86_64 or linux-aarch64)" ;;
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
[ "$PLATFORM" = "$HOST_OS-$HOST_ARCH" ] || die "$PLATFORM cannot be tested on this host ($HOST_OS-$HOST_ARCH)"

# Paths. On Windows, use C:/... paths: gcc and the test program are native programs.
NATIVES=$(cd "$(dirname "$0")/.." && pwd)
[ "$HOST_OS" = windows ] && NATIVES=$(cygpath -m "$NATIVES")
ROOT=$(dirname "$NATIVES")
LUAJIT_SRC=${LUAJIT_SRC:-$ROOT/vendor/luajit}
WORK=$NATIVES/build/$PLATFORM
TREE=$WORK/luajit-assert
OUT=$WORK/machine-test
EXE=$OUT/machine_test$([ "$HOST_OS" = windows ] && echo .exe || true)
ASSERT_FLAGS="-DLUA_USE_ASSERT -DLUA_USE_APICHECK"
TIMEOUT=${MACHINE_TEST_TIMEOUT:-900}

[ -f "$LUAJIT_SRC/src/luajit_rolling.h" ] && [ -z "$(git -C "$LUAJIT_SRC" rev-parse --show-prefix 2>/dev/null || echo x)" ] \
  || die "$LUAJIT_SRC is not a LuaJIT git checkout (run: git submodule update --init vendor/luajit)"
command -v gcc >/dev/null 2>&1 || die "gcc not found"
command -v timeout >/dev/null 2>&1 || die "timeout (coreutils) not found"

echo "== Tier 0 machine test for $PLATFORM"
echo "   compiler: $(gcc --version | head -1)"

# 1. The assert LuaJIT. The stamp records everything that the reuse decision depends on beyond cclj-build.txt.
COMMIT=$(git -C "$LUAJIT_SRC" rev-parse HEAD)
stamp() {
  echo "commit=$COMMIT"
  echo "extra=$ASSERT_FLAGS"
  echo "inputs=$(cat "$NATIVES"/luajit-patches/0*.patch "$NATIVES/luajit-patches/build-patched-luajit.sh" \
                 | sha256sum | cut -c1-64)"
  echo "compiler=$(gcc -dumpmachine) $(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion)"
  [ "$HOST_OS" = linux ] && echo "libc=$(ldd --version 2>&1 | head -1)"
  true
}
STAMP=$(stamp)
info() { sed -n "s/^$1=//p" "$TREE/cclj-build.txt"; }
reuse=0
if [ -f "$TREE/src/libluajit.a" ] && [ -f "$TREE/cclj-build.txt" ] && [ -f "$TREE/cclj-assert.stamp" ] &&
   [ "$(info luajit_commit)" = "$COMMIT" ] && [[ " $(info xcflags) " == *" $ASSERT_FLAGS "* ]] &&
   [ "$(cat "$TREE/cclj-assert.stamp")" = "$STAMP" ]; then
  reuse=1
fi
if [ "$reuse" = 1 ]; then
  echo "== reusing the assert LuaJIT in $TREE"
else
  echo "== building the assert LuaJIT into $TREE (log: $WORK/luajit-assert-build.log)"
  mkdir -p "$WORK"
  if ! TARGET_CFLAGS= bash "$NATIVES/luajit-patches/build-patched-luajit.sh" "$LUAJIT_SRC" "$TREE" "$ASSERT_FLAGS" \
       > "$WORK/luajit-assert-build.log" 2>&1; then
    tail -30 "$WORK/luajit-assert-build.log" >&2
    die "LuaJIT build failed"
  fi
  printf '%s\n' "$STAMP" > "$TREE/cclj-assert.stamp"
fi
echo "   LuaJIT $COMMIT, xcflags: $(info xcflags)"

# 2. The test program.
read -r -a XCF <<< "$(info xcflags)"
CFLAGS=(-std=gnu99 -g -O1 -DCCLJ_TEST -DCCLJ_DEBUG -Wall -Wextra -funwind-tables "${XCF[@]}" -I"$TREE/src" -I"$NATIVES/src")
SRCS=("$NATIVES/src/machine.c" "$NATIVES/tests/machine/machine_test.c")
LIBS=()
[ "$HOST_OS" = linux ] && LIBS=(-lm -ldl -pthread)
mkdir -p "$OUT"
rm -f "$EXE"
build() {   # build <output> [extra flags...]: compiler output goes to <output>.log
  local exe=$1
  shift
  gcc "${CFLAGS[@]}" "$@" -o "$exe" "${SRCS[@]}" "$TREE/src/libluajit.a" "${LIBS[@]}" > "$exe.log" 2>&1
}
report() {   # report <log>: the sanitizer report in the log (60 lines from its first error line), or its end
  local first
  first=$(grep -n -m1 -E 'ERROR: (Address|Leak)Sanitizer|runtime error:' "$1" | cut -d: -f1 || true)
  if [ -n "$first" ]; then sed -n "${first},$((first + 59))p" "$1"; else tail -40 "$1"; fi | sed 's/^/   /'
}
SANITIZED=0
if [ "$HOST_OS" = linux ] && [ "${MACHINE_TEST_SANITIZE:-1}" != 0 ]; then
  SAN=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  echo "== compiling with ${SAN[*]}"
  if ! build "$EXE" "${SAN[@]}"; then
    echo "note: the sanitized build does not compile or link here (no libasan?); sanitizers dropped:"
    sed 's/^/   /' "$EXE.log" | tail -10
  elif ! timeout 120 "$EXE" --smoke > "$OUT/smoke.log" 2>&1; then
    {
      echo "machine-test: FAILED: the sanitized program fails its smoke run (log: $OUT/smoke.log). The report:"
      report "$OUT/smoke.log"
      echo "machine-test: a report about machine.c or machine_test.c is a bug to fix. Only if it is a problem of" \
           "ASan with LuaJIT's GC64 mode (M1.md section 6), rerun with MACHINE_TEST_SANITIZE=0."
    } >&2
    exit 1
  else
    SANITIZED=1
  fi
fi
if [ "$SANITIZED" = 0 ]; then
  echo "== compiling without sanitizers"
  build "$EXE" || { cat "$EXE.log" >&2; die "compile failed"; }
fi
if [ -s "$EXE.log" ]; then
  sed 's/^/   /' "$EXE.log" >&2
  die "compiler warnings (above): machine.c and machine_test.c must compile without any"
fi

# 3. Run.
echo "== running $EXE (timeout ${TIMEOUT} s$([ "$SANITIZED" = 1 ] && echo ", sanitized" || true))"
set +e
timeout "$TIMEOUT" "$EXE" 2>&1 | tee "$OUT/machine-test.log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" = 124 ]; then
  echo "machine-test: FAILED: no result within $TIMEOUT s" >&2
  exit 1
elif [ "$rc" != 0 ]; then
  echo "machine-test: FAILED (exit $rc; log: $OUT/machine-test.log)" >&2
  exit 1
fi
echo "machine-test: OK ($PLATFORM$([ "$SANITIZED" = 1 ] && echo ", with ASan and UBSan" || true))"
