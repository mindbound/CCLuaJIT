#!/bin/bash
# Run inside WSL/Linux after build-linux.sh. Logs: linux/logs/.
cd "$(dirname "$0")"
mkdir -p logs
fails=0
for t in t1_parallel t2_nesting t3_semantics t4_cboundary t5_abort t6_depth t7_bench; do
  ./pausehost-linux ../tests/$t.lua > logs/$t.log 2>&1; rc=$?
  echo "$t: rc=$rc $(grep -c 'RESULT.*PASS' logs/$t.log) pass, $(grep -c 'RESULT.*FAIL' logs/$t.log) fail"
  [ $rc -ne 0 ] && fails=$((fails+1))
done
for t in t1_parallel t2_nesting t3_semantics; do
  PAUSE_MIN_US=20 PAUSE_MAX_US=60 ./pausehost-linux ../tests/$t.lua > logs/fast-$t.log 2>&1; rc=$?
  echo "fast-$t: rc=$rc $(grep -c 'RESULT.*PASS' logs/fast-$t.log) pass"; [ $rc -ne 0 ] && fails=$((fails+1))
done
for i in 1 2 3 4 5; do
  ./pausehost-linux ../tests/t5_abort.lua > logs/t5_abort-rep$i.log 2>&1 || fails=$((fails+1))
done
grep -h "abort latency" logs/t5_abort*.log | awk '{print $3}' | sort -n | awk '{a[NR]=$1} END {print "hard-abort latency over " NR " runs: min " a[1] " us, median " a[int((NR+1)/2)] " us, max " a[NR] " us"}'
NAIVE=1 ./pausehost-linux ../tests/t1_parallel.lua > logs/naive-t1_parallel.log 2>&1; echo "naive control: rc=$? (expected 1) $(grep RESULT logs/naive-t1_parallel.log)"
./pausehost-linux --native-nest > logs/native-nest.log 2>&1; echo "native nesting: rc=$? (139 = SIGSEGV) $(tail -1 logs/native-nest.log)"
./pausehost-linux --wrapper-nest > logs/wrapper-nest.log 2>&1; echo "wrapper nesting, no limit: rc=$? $(tail -1 logs/wrapper-nest.log)"
grep -h "BENCH" logs/t7_bench.log
grep -h "bytes/level" logs/t6_depth.log | sed 's/^ *result: //'
echo "failing runs: $fails"
