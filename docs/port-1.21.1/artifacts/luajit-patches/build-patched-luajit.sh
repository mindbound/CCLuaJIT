#!/bin/bash
# Build the CCLuaJIT-flavoured LuaJIT (static library + luajit executable for testing).
# usage: build-patched-luajit.sh <pristine-luajit-tree> <out-dir> [extra XCFLAGS]
#   e.g. build-patched-luajit.sh vendor/luajit build/luajit-cc
#        LUAJIT_REV=3c4f9fe2 build-patched-luajit.sh vendor/luajit build/luajit-cc   (recommended pin)
#        build-patched-luajit.sh vendor/luajit build/luajit-dbg "-DLUA_USE_ASSERT -DLUA_USE_APICHECK"
# Applies every 0*.patch in this directory in order (01-08) and builds with the recommended flags.
# On Windows (Git Bash + MinGW) it uses mingw32-make; elsewhere it uses make.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$1; OUT=$2; EXTRA=${3:-}
REV=${LUAJIT_REV:-HEAD}
rm -rf "$OUT"; mkdir -p "$OUT"
# Export a clean copy of the tree (works for a git checkout or a plain directory).
if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
  git -C "$SRC" archive --format=tar "$REV" | tar -xf - -C "$OUT"
else
  [ "$REV" = HEAD ] || { echo "LUAJIT_REV needs a git checkout as <pristine-luajit-tree>" >&2; exit 1; }
  cp -r "$SRC"/. "$OUT"/
fi
cd "$OUT"
for p in "$HERE"/0*.patch; do
  echo "applying $(basename "$p")"
  git apply -p1 --whitespace=nowarn "$p" 2>/dev/null || patch -p1 --no-backup-if-mismatch < "$p"
done
MAKE=make; command -v mingw32-make >/dev/null 2>&1 && [ "${OS:-}" = "Windows_NT" ] && MAKE=mingw32-make
cd src
# INTERRUPT     patch 01 (+07): thread-safe luaJIT_interrupt(), interruptible pattern matching and sort
# LUA52COMPAT   table.pack/unpack, goto (kept usable as an identifier by patch 06), etc.
# DISABLE_FFI   no ffi library (sandbox)
# DISABLE_BCLOAD patch 03: every loader refuses bytecode (sandbox)
# CSTACKGUARD   patch 08: luaJIT_setcstacklimit(), catchable "C stack overflow" instead of a crash
# DISABLE_PROFILE / DISABLE_VMEVENT: the sandbox removes `jit`, so compile the profiler (SIGPROF) and
#               jit.attach callbacks out as defence in depth
"$MAKE" -j"$(nproc 2>/dev/null || echo 4)" BUILDMODE=static \
  XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT $EXTRA"
echo "built: $OUT/src/libluajit.a and $OUT/src/luajit(.exe)"
