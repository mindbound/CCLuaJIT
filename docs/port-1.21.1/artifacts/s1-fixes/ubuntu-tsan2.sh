#!/bin/bash
# One-off (S1 step 7, review R1): tsan_race.c under ThreadSanitizer, with machine.c as it is now and with the busy
# write of 5381504 put back in cm_load and cm_resume (negr1/). The whole Tier 0 program under TSan stops with a
# segmentation fault after the last check of section 2, before section 3 (the spec fuzz) prints its check.
set -uo pipefail
G=<scratch>/g7
R=<repo>
TREE=$R/natives/build/linux-x86_64/luajit-assert
XCF=$(sed -n 's/^xcflags=//p' "$TREE/cclj-build.txt")
echo "host: $(. /etc/os-release; echo $PRETTY_NAME), $(gcc --version | head -1)"
for v in current negctl; do
  src=$R/natives/src; [ $v = negctl ] && src=$G/negr1/natives/src
  out=/tmp/cclj-tsan2-$v; rm -rf $out; mkdir -p $out
  gcc -std=gnu99 -g -O1 -DCCLJ_TEST -DCCLJ_DEBUG -Wall -Wextra $XCF -I"$TREE/src" -I"$src" -fsanitize=thread \
    -fno-omit-frame-pointer -o $out/tsan_race "$src/machine.c" $G/tsan_race.c "$TREE/src/libluajit.a" -lm -ldl -pthread \
    > $out/build.log 2>&1 || { echo "$v: build failed"; tail -5 $out/build.log; continue; }
  [ -s $out/build.log ] && { echo "$v: compiler output:"; head -5 $out/build.log; }
  ( cd $out && TSAN_OPTIONS="halt_on_error=0" timeout 600 ./tsan_race > $out/run.log 2>&1 ); rc=$?
  cp $out/run.log $G/logs/ubuntu-tsan2-$v.log
  echo "== $v: exit $rc; data race reports: $(grep -c 'WARNING: ThreadSanitizer: data race' $out/run.log); other TSan warnings: $(grep 'WARNING: ThreadSanitizer' $out/run.log | grep -vc 'data race')"
  grep -a '^tsan_race:\|^round' $out/run.log | head -3
  grep -a -m1 -A12 'WARNING: ThreadSanitizer' $out/run.log | cut -c1-150
done
