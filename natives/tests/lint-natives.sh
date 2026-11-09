#!/bin/bash
# Static checks on natives/src and on the objects that build-natives.sh compiles from it (M1.md section 3.14).
# They enforce rules of the machine bridge that the compiler does not check:
#   include graph     The compile-unit split (M1.md section 3.3 item 2): machine.c, the code that calls Lua, never
#                     sees JNI, and jni_machine.c, the code that calls JNI, never sees Lua. machine.c and
#                     cclj_machine.h do not include jni.h; jni_machine.c includes none of lua.h, lauxlib.h,
#                     lualib.h, luajit.h and lj_*; cclj_machine.h includes only <stddef.h> and <stdint.h>.
#                     Includes of headers that exist in natives/src are followed. Every #include counts, also one
#                     inside #if 0. A computed #include (a macro name) fails, because it cannot be checked.
#   forbidden names   Identifiers that natives/src must not use, except the M0 self-test cclj.c (M1.md section 3.8,
#                     the "never" row, and section 3.9, J3). Scanned: every *.c and *.h, and every other file of
#                     natives/src that the include graph of machine.c, jni_machine.c or cclj_machine.h reaches
#                     (an included .inc, for example). Comments and the contents of string and character
#                     literals are removed first, so a comment that explains a rule does not fail. An identifier
#                     fails if it contains a forbidden name, so "Critical" catches every Get/Release*Critical.
#                     nm also checks that machine.o and jni_machine.o reference none of the forbidden Lua
#                     functions, however the source spells the call (a macro, token pasting).
#   writable statics  No mutable static data (M1.md section 3.13, I4): nm shows no b, B, d, D, C, G, S, V or v
#                     symbol in machine.o and jni_machine.o. V and v are weak data ("__attribute__((weak)) int x").
#                     Section symbols are ignored: MinGW objects always have .bss and .data. Only names that
#                     objdump -h lists as sections of the object count as section symbols, so MinGW's
#                     ".weak.<name>.<default>" data symbol still fails. C is the type GCC 8 gives a tentative
#                     definition without -fno-common; build-natives.sh compiles with -fno-common, which makes it B.
#   objects current   No file in natives/src is newer than the objects, so the nm checks do not read old code.
#   no CCLJ_TEST      The Tier 0 test hooks (cm_test_*, compiled in only with -DCCLJ_TEST by machine-test.sh) are
#                     in no object and not in the library, and no file of natives/src (the same files as for the
#                     forbidden names, cclj.c included) defines CCLJ_TEST.
#                     This applies to release and CCLJ_DEBUG=1 builds alike: build-natives.sh never sets it.
#
# usage: natives/tests/lint-natives.sh <platform-id>
#   Reads natives/src, the objects in natives/build/<platform-id>/obj and the library in
#   natives/build/out/natives/<platform-id>/. Run natives/build-natives.sh <platform-id> first.
#   Any host with bash, awk, grep, sed and binutils nm and objdump that read the objects (MinGW binutils for
#   windows-x86_64).
# Prints each check and a summary. Exit 0: every check passed. 1: a check failed. 2: usage error or missing input.
set -euo pipefail
export LC_ALL=C

die() { echo "lint-natives: $*" >&2; exit 2; }

PLATFORM=${1:-}
case "$PLATFORM" in
  windows-x86_64) LIBNAME=cclj.dll ;;
  linux-x86_64|linux-aarch64) LIBNAME=libcclj.so ;;
  *) die "usage: lint-natives.sh <platform-id>   (windows-x86_64, linux-x86_64 or linux-aarch64)" ;;
esac

# Paths. On Windows, use C:/... paths: nm is a native program.
NATIVES=$(cd "$(dirname "$0")/.." && pwd)
case "$(uname -s)" in MINGW*|MSYS*) NATIVES=$(cygpath -m "$NATIVES") ;; esac
SRC=$NATIVES/src
OBJDIR=$NATIVES/build/$PLATFORM/obj
OUTDIR=$NATIVES/build/out/natives/$PLATFORM
LIB=$OUTDIR/$LIBNAME

# Inputs. One object per source that build-natives.sh compiles (its SRCS).
OBJS=(cclj.o machine.o jni_machine.o)
STATIC_FREE=(machine.o jni_machine.o)
missing=()
for f in machine.c jni_machine.c cclj_machine.h; do [ -f "$SRC/$f" ] || missing+=("source $SRC/$f"); done
for o in "${OBJS[@]}"; do [ -f "$OBJDIR/$o" ] || missing+=("object $OBJDIR/$o"); done
[ -f "$LIB" ] || missing+=("library $LIB")
if [ ${#missing[@]} != 0 ]; then
  printf 'lint-natives: missing %s\n' "${missing[@]}" >&2
  die "missing input (build first: bash natives/build-natives.sh $PLATFORM)"
fi
command -v nm >/dev/null 2>&1 || die "nm (binutils) not found"
command -v objdump >/dev/null 2>&1 || die "objdump (binutils) not found"

# strip_c <keep-strings> <file>: the file with C comments removed (each one becomes a space, line breaks stay, so
# line numbers stay valid). With <keep-strings> 0, the contents of string and character literals are removed too
# ("..." becomes ""). A literal or // comment continues on the next line only after a backslash at the end.
strip_c() {
  awk -v keep="$1" '
    { line = $0; sub(/\r$/, "", line); n = length(line); out = ""; i = 1
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (st == 0) {
          if (d == "/*") { st = 1; out = out " "; i += 2 }
          else if (d == "//") { st = 2; i += 2 }
          else { if (c == "\"") st = 3; else if (c == "\047") st = 4; out = out c; i++ }
        } else if (st == 1) {
          if (d == "*/") { st = 0; i += 2 } else i++
        } else if (st == 2) {
          i++
        } else {
          q = (st == 3) ? "\"" : "\047"
          if (c == "\\") { if (keep) out = out d; i += 2 }
          else if (c == q) { st = 0; out = out c; i++ }
          else { if (keep) out = out c; i++ }
        }
      }
      if (st >= 2 && substr(line, n, 1) != "\\") st = 0
      print out }' "$2"
}

# includes <file>: one line per #include directive: the operand as written (<x.h>, "x.h", or a macro name).
includes() {
  strip_c 1 "$1" | sed -nE 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*([^[:space:]]+).*/\1/p'
}

# include_graph <file in natives/src>: "<operand> <includer>" for every #include reachable from the file, following
# "x.h" includes of headers that exist in natives/src.
include_graph() {
  local queue=("$1") seen=" " f inc name
  while [ ${#queue[@]} != 0 ]; do
    f=${queue[0]}; queue=("${queue[@]:1}")
    case "$seen" in *" $f "*) continue ;; esac
    seen="$seen$f "
    while IFS= read -r inc; do
      echo "$inc $f"
      case "$inc" in
        \"*\") name=${inc#\"}; name=${name%\"}; [ -f "$SRC/$name" ] && queue+=("$name") ;;
      esac
    done < <(includes "$SRC/$f")
  done
}

# src_files <file in natives/src>...: every *.c and *.h of natives/src, the files given, and every file of
# natives/src that they include, directly or not (the "x" includes that include_graph follows), sorted.
src_files() {
  local path f inc from name
  {
    for path in "$SRC"/*.c "$SRC"/*.h; do
      if [ -f "$path" ]; then echo "${path##*/}"; fi
    done
    for f in "$@"; do
      echo "$f"
      while read -r inc from; do
        case "$inc" in
          \"*\") name=${inc#\"}; name=${name%\"}; if [ -f "$SRC/$name" ]; then echo "$name"; fi ;;
        esac
      done < <(include_graph "$f")
    done
  } | sort -u
}

# header_name <operand>: the file name of an include operand without brackets, quotes or directories.
header_name() { local h=${1#[<\"]}; h=${h%[>\"]}; echo "${h##*/}"; }

# Check bookkeeping: begin <title>; bad <detail> for each problem; end prints PASS or FAIL with the details.
checks=0; failed=0; problems=()
begin() { title=$1; problems=(); checks=$((checks + 1)); }
bad() { problems+=("$1"); }
end() {
  if [ ${#problems[@]} = 0 ]; then
    echo "  PASS  $title"
  else
    echo "  FAIL  $title"; printf '          %s\n' "${problems[@]}"; failed=$((failed + 1))
  fi
}

echo "== lint-natives for $PLATFORM"
echo "   sources: $SRC"
echo "   objects: $OBJDIR"
if [ -f "$OUTDIR/DEBUG-BUILD.txt" ]; then
  echo "   library: $LIB (DEBUG build: $OUTDIR/DEBUG-BUILD.txt)"
else
  echo "   library: $LIB (release build)"
fi

echo "== include graph"
for f in machine.c cclj_machine.h; do
  begin "$f does not include jni.h"
  while read -r inc from; do
    case "$inc" in
      [\<\"]*) [ "$(header_name "$inc")" != jni.h ] || bad "$from includes $inc" ;;
      *) bad "$from has a computed #include $inc, which cannot be checked" ;;
    esac
  done < <(include_graph "$f")
  end
done
begin "jni_machine.c includes none of lua.h, lauxlib.h, lualib.h, luajit.h, lj_*"
while read -r inc from; do
  case "$inc" in
    [\<\"]*) case "$(header_name "$inc")" in
               lua.h|lauxlib.h|lualib.h|luajit.h|lj_*) bad "$from includes $inc" ;;
             esac ;;
    *) bad "$from has a computed #include $inc, which cannot be checked" ;;
  esac
done < <(include_graph jni_machine.c)
end
begin "cclj_machine.h includes only <stddef.h> and <stdint.h>"
while read -r inc; do
  case "$inc" in
    "<stddef.h>"|"<stdint.h>") ;;
    *) bad "cclj_machine.h includes $inc" ;;
  esac
done < <(includes "$SRC/cclj_machine.h")
end

echo "== forbidden names (natives/src except cclj.c; comments and literals removed)"
FORBIDDEN="FindClass NewGlobalRef NewWeakGlobalRef NewStringUTF GetStringUTFChars Critical ThrowNew
           lua_pushlightuserdata luaL_openlibs lua_sethook luaL_traceback"
FORBIDDEN_LUA="lua_pushlightuserdata luaL_openlibs lua_sethook luaL_traceback"   # the ones nm can see
# The *.c and *.h files, and every other file of natives/src that the bridge sources include.
ALL_SRC=()
while IFS= read -r f; do ALL_SRC+=("$f"); done < <(src_files machine.c jni_machine.c cclj_machine.h)
scanned=()
for f in "${ALL_SRC[@]}"; do
  if [ "$f" != cclj.c ]; then scanned+=("$f"); fi
done
begin "none of $(echo $FORBIDDEN | sed 's/ /, /g') in ${scanned[*]}"
for f in "${scanned[@]}"; do
  while IFS= read -r hit; do bad "$f:$hit"; done < <(
    strip_c 0 "$SRC/$f" | grep -noE '[A-Za-z_][A-Za-z0-9_]*' | awk -F: -v names="$FORBIDDEN" '
      BEGIN { n = split(names, fb, /[[:space:]]+/) }
      { for (k = 1; k <= n; k++) if (fb[k] != "" && index($2, fb[k])) { print $1 ": " $2; break } }')
done
end

echo "== objects"
begin "objects are newer than every file in natives/src"
for o in "${OBJS[@]}"; do
  for path in "$SRC"/*; do
    [ -f "$path" ] || continue
    if [ "$path" -nt "$OBJDIR/$o" ]; then
      bad "$o is older than ${path##*/} (rebuild: bash natives/build-natives.sh $PLATFORM)"
    fi
  done
done
end
# nm_symbols <file> [nm options]: "<type> <name>" per symbol; no output for a file without symbols (a stripped
# library). Lines of nm that are not symbols (messages) have no one-letter type and are dropped.
nm_symbols() {
  local out
  if ! out=$(nm "${@:2}" "$1" 2>&1); then
    case "$out" in *"no symbols"*) return 0 ;; esac
    echo "$out" >&2; return 1
  fi
  awk 'NF >= 2 && $(NF - 1) ~ /^[A-Za-z?-]$/ {print $(NF - 1), $NF}' <<< "$out"
}
# The section of each symbol (ELF objdump -t: "<addr> <flags> <section> <size> <name>"), only to explain a failure.
section_of() { awk -v n="$2" 'NF >= 4 && $NF == n && $(NF - 2) ~ /^\./ {print $(NF - 2); exit}' <<< "$1"; }
for o in "${STATIC_FREE[@]}"; do
  begin "no writable static data in $o (nm: no b B d D C G S V v symbol, section symbols aside)"
  syms=$(nm_symbols "$OBJDIR/$o") || die "nm cannot read $OBJDIR/$o"
  table=$(objdump -t "$OBJDIR/$o" 2>/dev/null || true)
  # The section names of the object (objdump -h: "<idx> <name> <size> ..."), between spaces.
  sections=" $(objdump -h "$OBJDIR/$o" | awk '$1 ~ /^[0-9]+$/ && NF >= 2 {printf "%s ", $2}') " \
    || die "objdump cannot read $OBJDIR/$o"
  while read -r type name; do
    case "$type" in
      [bBdDCGSVv])
        case "$sections" in *" $name "*) continue ;; esac   # a section symbol (MinGW: .bss, .data)
        sec=$(section_of "$table" "$name")
        case "$sec" in
          .data.rel.ro*) bad "$type $name (in $sec: a const table of pointers needs relocations under -fPIC, so it is writable data; use a char array or a switch)" ;;
          "") bad "$type $name" ;;
          *) bad "$type $name (in $sec)" ;;
        esac ;;
    esac
  done <<< "$syms"
  end
done
begin "no reference to $(echo $FORBIDDEN_LUA | sed 's/ /, /g') in ${STATIC_FREE[*]} (nm)"
for o in "${STATIC_FREE[@]}"; do
  syms=$(nm_symbols "$OBJDIR/$o") || die "nm cannot read $OBJDIR/$o"
  while read -r type name; do
    for fb in $FORBIDDEN_LUA; do
      if [ "$name" = "$fb" ]; then bad "$o: $type $name"; fi
    done
  done <<< "$syms"
done
end

echo "== no CCLJ_TEST"
begin "no cm_test_ symbol in ${OBJS[*]}"
for o in "${OBJS[@]}"; do
  syms=$(nm_symbols "$OBJDIR/$o") || die "nm cannot read $OBJDIR/$o"
  while read -r type name; do
    case "$name" in *cm_test_*) bad "$o: $type $name" ;; esac
  done <<< "$syms"
done
end
begin "no cm_test_ symbol in $LIBNAME"
syms=$(nm_symbols "$LIB") || die "nm cannot read $LIB"
if [ "$PLATFORM" != windows-x86_64 ]; then
  syms+=$'\n'$(nm_symbols "$LIB" -D) || die "nm -D cannot read $LIB"
fi
while read -r type name; do
  case "$name" in *cm_test_*) bad "$type $name" ;; esac
done <<< "$syms"
end
begin "no file of natives/src defines CCLJ_TEST (${ALL_SRC[*]})"
for f in "${ALL_SRC[@]}"; do
  while IFS= read -r hit; do bad "$f:$hit"; done < <(
    strip_c 1 "$SRC/$f" | grep -nE '^[[:space:]]*#[[:space:]]*define[[:space:]]+CCLJ_TEST([^A-Za-z0-9_]|$)' || true)
done
end

echo "== $checks checks, $failed failed"
if [ "$failed" != 0 ]; then
  echo "lint-natives: FAILED" >&2
  exit 1
fi
echo "lint-natives: OK"
