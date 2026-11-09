#!/bin/bash
# One-off (S1 step 7, review R1): Tier 0 under ThreadSanitizer on Ubuntu 26.04, with machine.c as it is now and with
# the busy write of 5381504 put back in cm_load and cm_resume (negr1/). Not part of machine-test.sh.
set -uo pipefail
G=<scratch>/g7
R=<repo>
TREE=$R/natives/build/linux-x86_64/luajit-assert
XCF=$(sed -n 's/^xcflags=//p' "$TREE/cclj-build.txt")
echo "host: $(. /etc/os-release; echo $PRETTY_NAME), $(gcc --version | head -1)"
echo "tree: $(sed -n 's/^luajit_commit=//p' "$TREE/cclj-build.txt"), xcflags: $XCF; stamp: $(grep compiler "$TREE/cclj-assert.stamp")"
for v in current negctl; do
  src=$R/natives/src; [ $v = negctl ] && src=$G/negr1/natives/src
  out=/tmp/cclj-tsan-$v; rm -rf $out; mkdir -p $out
  gcc -std=gnu99 -g -O1 -DCCLJ_TEST -DCCLJ_DEBUG -Wall -Wextra -funwind-tables $XCF -I"$TREE/src" -I"$src" \
    -fsanitize=thread -fno-omit-frame-pointer -o $out/machine_test "$src/machine.c" $R/natives/tests/machine/machine_test.c \
    "$TREE/src/libluajit.a" -lm -ldl -pthread > $out/build.log 2>&1 || { echo "$v: build failed"; tail -5 $out/build.log; continue; }
  echo "== $v: running under TSan"
  ( cd $out && TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" timeout 1800 ./machine_test > $out/run.log 2>&1 ); rc=$?
  cp $out/run.log $G/logs/ubuntu-tsan-$v.log
  echo "$v: exit $rc; data race reports: $(grep -c 'WARNING: ThreadSanitizer: data race' $out/run.log); other TSan reports: $(grep 'WARNING: ThreadSanitizer' $out/run.log | grep -vc 'data race')"
  grep -a -E '^machine_test:|re-entrancy: refused' $out/run.log | cut -c1-200
  grep -n -m1 -A14 'WARNING: ThreadSanitizer' $out/run.log | cut -c1-160
done
