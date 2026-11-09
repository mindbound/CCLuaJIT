#!/bin/bash
# Scan JVM output for the messages of -Xcheck:jni (M1.md section 3.14, fact F13).
#
# usage: natives/tests/jni-scan.sh <log>...      fails if any log contains a pattern, and prints the number of
#                                                matching lines and the first SHOW (100) of them in each log
#        natives/tests/jni-scan.sh --self-test   checks the scanner and the patterns against jni-scan-fixtures/
#
# The patterns are in jni-warnings.txt next to this script, one per line (the jniScan wrapper of the JavaExec tasks
# in common/build.gradle reads the same file). Each one is matched as a fixed string, case-sensitive, anywhere in
# a line: JDK 21 prints these texts exactly so (F13). Empty lines and a CR at a line end (a CRLF checkout) are
# ignored. A broken native method can print a warning on every JNI call, a million lines: so only the first SHOW
# matching lines of each log are printed, and the count is taken with grep -c.
#
# A log must hold stdout and stderr of the JVM (java ... 2>&1 | tee <log>): the WARNING and FATAL ERROR lines go
# to stdout, the MXCSR warning to stderr. The scan does not replace the exit-code check, which must fail on any
# non-zero exit: FATAL ERROR ends the JVM with exit code 1 on Windows and with SIGABRT (134) on Linux, and a crash
# may print nothing that matches. -Xcheck:jni prints nothing for leaked local refs.
#
# --self-test passes only if every jni-scan-fixtures/dirty-*.log fails the scan, every pattern occurs in at least
# one dirty-*.log (so a pattern with a typo fails), and clean.log (ordinary JVM and Gradle output with near misses)
# passes.
#
# Exit: 0 no match (self-test: passed). 1 a match (self-test: failed). 2 usage error, or a missing log or pattern
# file.
set -euo pipefail
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
PATTERNS=$HERE/jni-warnings.txt
FIXTURES=$HERE/jni-scan-fixtures
SHOW=100

die() { echo "jni-scan: $*" >&2; exit 2; }

# The patterns as grep arguments: -e <pattern> for each one.
[ -f "$PATTERNS" ] || die "pattern file not found: $PATTERNS"
PATS=(); NPATS=0
while IFS= read -r p || [ -n "$p" ]; do
  p=${p%$'\r'}
  [ -n "$p" ] || continue
  PATS+=(-e "$p"); NPATS=$((NPATS + 1))
done < "$PATTERNS"
[ "$NPATS" != 0 ] || die "no patterns in $PATTERNS"

# scan <log>...: prints "<log>:<line>:<text>" for the first SHOW matching lines of each log. Returns 0 if none
# matched, 1 otherwise.
scan() {
  local f out
  for f in "$@"; do [ -f "$f" ] || die "log not found: $f"; done
  if out=$(grep -a -H -n -F -m "$SHOW" "${PATS[@]}" -- "$@"); then
    echo "$out"; return 1
  else
    [ $? = 1 ] || die "grep failed on $*"
    return 0
  fi
}

if [ "${1:-}" = --self-test ]; then
  [ $# = 1 ] || die "usage: jni-scan.sh --self-test"
  [ -f "$FIXTURES/clean.log" ] || die "fixture not found: $FIXTURES/clean.log"
  dirty=()
  for f in "$FIXTURES"/dirty-*.log; do [ -f "$f" ] && dirty+=("$f"); done
  [ ${#dirty[@]} != 0 ] || die "no dirty-*.log fixtures in $FIXTURES"
  failed=0
  echo "== jni-scan self-test: $NPATS patterns, ${#dirty[@]} dirty fixtures, clean.log"
  for f in "${dirty[@]}"; do
    rc=0; out=$(scan "$f") || rc=$?
    if [ "$rc" = 1 ]; then
      echo "  PASS  ${f##*/} fails the scan ($(grep -c '' <<< "$out") matching line(s))"
    else
      echo "  FAIL  ${f##*/} does not fail the scan (exit $rc), but holds a message of -Xcheck:jni"; failed=1
    fi
  done
  while IFS= read -r p || [ -n "$p" ]; do
    p=${p%$'\r'}
    [ -n "$p" ] || continue
    if grep -q -a -F -e "$p" -- "${dirty[@]}"; then
      echo "  PASS  pattern occurs in a dirty fixture: $p"
    else
      echo "  FAIL  pattern occurs in no dirty fixture: $p"; failed=1
    fi
  done < "$PATTERNS"
  rc=0; out=$(scan "$FIXTURES/clean.log") || rc=$?
  if [ "$rc" = 0 ]; then
    echo "  PASS  clean.log passes the scan"
  else
    echo "  FAIL  clean.log fails the scan (exit $rc):"; sed 's/^/          /' <<< "$out"; failed=1
  fi
  if [ "$failed" != 0 ]; then echo "jni-scan: self-test FAILED" >&2; exit 1; fi
  echo "jni-scan: self-test OK"
  exit 0
fi

case "${1:-}" in
  "") die "usage: jni-scan.sh <log>...   or   jni-scan.sh --self-test" ;;
  -*) die "unknown option $1 (usage: jni-scan.sh <log>...   or   jni-scan.sh --self-test)" ;;
esac
rc=0; out=$(scan "$@") || rc=$?
case "$rc" in
  0) echo "jni-scan: OK, none of the $NPATS patterns in $# log(s): $*" ;;
  1) total=0
     for f in "$@"; do n=$(grep -a -c -F "${PATS[@]}" -- "$f") || true; total=$((total + n)); done
     echo "jni-scan: -Xcheck:jni reported problems: $total matching line(s) in $# log(s). At most $SHOW per log:"
     echo "$out"
     echo "jni-scan: FAILED" >&2
     exit 1 ;;
  *) exit 2 ;;  # scan has printed the reason
esac
