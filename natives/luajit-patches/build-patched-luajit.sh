#!/bin/bash
# Build the CCLuaJIT-flavoured LuaJIT: static library + luajit executable (for the harnesses).
# usage: build-patched-luajit.sh <luajit-git-tree> <out-dir> [extra XCFLAGS]
#   e.g. natives/luajit-patches/build-patched-luajit.sh vendor/luajit natives/build/linux-x86_64/luajit
#        build-patched-luajit.sh vendor/luajit <out> "-DLUA_USE_ASSERT -DLUA_USE_APICHECK"   (test build)
# Exports a clean copy of LUAJIT_REV (default HEAD; the vendor/luajit submodule is on cclj/v2.1), applies every
# 0*.patch in this directory in order (01-08) and builds with the recommended flags (DESIGN.md section 2).
# Environment:
#   LUAJIT_REV      revision to export (default HEAD). Needs a git checkout as <luajit-git-tree>.
#   TARGET_CFLAGS   extra flags for the target objects only (not the host build tools), e.g. -fPIC so that
#                   the static library can be linked into a shared object on Linux.
# On Windows (Git Bash + MinGW) it uses mingw32-make; elsewhere it uses make.
# Writes <out-dir>/cclj-build.txt (key=value: revision, commit, flags, patches) for build-natives.sh and
# tests/run-harnesses.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$1; OUT=$2; EXTRA=${3:-}
REV=${LUAJIT_REV:-HEAD}
TCFLAGS=${TARGET_CFLAGS:-}
# INTERRUPT     patch 01 (+07): thread-safe luaJIT_interrupt(), interruptible pattern matching and sort
# LUA52COMPAT   table.pack/unpack, goto (kept usable as an identifier by patch 06), etc.
# DISABLE_FFI   no ffi library (sandbox)
# DISABLE_BCLOAD patch 03: every loader refuses bytecode (sandbox)
# CSTACKGUARD   patch 08: luaJIT_setcstacklimit(), catchable "C stack overflow" instead of a crash
# DISABLE_PROFILE / DISABLE_VMEVENT: the sandbox removes `jit`, so compile the profiler (SIGPROF) and
#               jit.attach callbacks out as defence in depth
XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT${EXTRA:+ $EXTRA}"
# Never convert line endings: the patches and DynASM expect LF. A Windows git (core.autocrlf=true, or the
# text=auto rule of an enclosing repository such as CCLuaJIT itself) would otherwise write CRLF.
GIT_LF=(-c core.autocrlf=false -c core.eol=lf)

[ -f "$SRC/src/luajit_rolling.h" ] || { echo "$SRC is not a LuaJIT tree (no src/luajit_rolling.h)" >&2; exit 1; }
# OUT is deleted and recreated: refuse anything that could be a source tree or a system directory.
case "$OUT" in ""|/|.|..|"$SRC"|"$SRC"/) echo "refusing to use '$OUT' as the output directory" >&2; exit 1 ;; esac
rm -rf "$OUT"; mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# Export a clean copy of the tree: with git archive from the top of a git checkout, else by copying a plain directory.
# (--show-prefix is empty only at the top of a work tree. An uninitialized submodule is a plain empty directory
# inside the enclosing repository and fails the luajit_rolling.h check above.)
if [ -z "$(git -C "$SRC" rev-parse --show-prefix 2>/dev/null || echo x)" ]; then
  COMMIT=$(git -C "$SRC" rev-parse --verify "$REV^{commit}")
  git "${GIT_LF[@]}" -C "$SRC" archive --format=tar "$COMMIT" | tar -xf - -C "$OUT"
else
  [ "$REV" = HEAD ] || { echo "LUAJIT_REV needs a git checkout as <luajit-git-tree>" >&2; exit 1; }
  COMMIT=unknown
  cp -r "$SRC"/. "$OUT"/
fi
cd "$OUT"
# git apply must work as a plain patch tool here. Inside a repository (CCLuaJIT's own working tree when OUT is
# natives/build/...) it reads paths of git-format patches (01, 07, 08) as relative to the top level, silently
# skips them because they are outside the current directory, and still exits 0. So stop the repository search
# at OUT, and fail on any "Skipped patch" line (LC_ALL=C: that message is translated in other locales).
CEILING=$(dirname "$OUT")
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) CEILING=$(cygpath -m "$CEILING") ;; esac
PATCHES=()
for p in "$HERE"/0*.patch; do
  echo "applying $(basename "$p")"
  if command -v git >/dev/null 2>&1; then
    if ! LC_ALL=C GIT_CEILING_DIRECTORIES="$CEILING" git "${GIT_LF[@]}" apply -v -p1 --whitespace=nowarn "$p" \
         > "$OUT/.apply.log" 2>&1 || grep -q '^Skipped patch' "$OUT/.apply.log"; then
      cat "$OUT/.apply.log" >&2; echo "patch $(basename "$p") did not apply" >&2; exit 1
    fi
  else
    patch -p1 --no-backup-if-mismatch < "$p"
  fi
  PATCHES+=("$(basename "$p")")
done
rm -f "$OUT/.apply.log"
MAKE=make; command -v mingw32-make >/dev/null 2>&1 && [ "${OS:-}" = "Windows_NT" ] && MAKE=mingw32-make
{
  echo "luajit_rev=$REV"
  echo "luajit_commit=$COMMIT"
  echo "xcflags=$XCFLAGS"
  echo "target_cflags=$TCFLAGS"
  echo "patches=${PATCHES[*]}"
} > "$OUT/cclj-build.txt"
cd src
"$MAKE" -j"$(nproc 2>/dev/null || echo 4)" BUILDMODE=static XCFLAGS="$XCFLAGS" TARGET_CFLAGS="$TCFLAGS"
echo "built: $OUT/src/libluajit.a and $OUT/src/luajit(.exe) from $COMMIT"
