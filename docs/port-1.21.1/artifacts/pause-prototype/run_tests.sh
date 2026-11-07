#!/bin/bash
# Runs the pause-protocol test suite. Logs go to logs/. Exit code = number of failing runs.
# Usage: ./run_tests.sh [quick]
cd "$(dirname "$0")"
mkdir -p logs
fails=0
run() {  # run <logname> <env...> -- <exe> <args...>
  local log=logs/$1.log; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  echo "=== $log: ${envs[*]} $*"
  env "${envs[@]}" "$@" > "$log" 2>&1
  local rc=$?
  grep -E "^RESULT" "$log" | sed 's/^/    /'
  if [ $rc -ne 0 ]; then echo "    -> exit $rc"; fails=$((fails+1)); fi
}
for t in t1_parallel t2_nesting t3_semantics t4_cboundary t5_abort t6_depth t7_bench; do
  run "$t" -- ./pausehost.exe tests/$t.lua
done
[ "$1" = quick ] && exit $fails
# Assertion/API-check build of LuaJIT (catches API misuse, broken frames, stack accounting).
for t in t1_parallel t2_nesting t3_semantics t4_cboundary t5_abort t6_depth; do
  run "assert-$t" -- ./pausehost-assert.exe tests/$t.lua
done
# Interpreter only (JIT off): different pause points, hook every 10^4 instructions.
for t in t1_parallel t2_nesting t3_semantics t4_cboundary t5_abort t6_depth; do
  run "jitoff-$t" JIT=0 SCALE=0.25 -- ./pausehost.exe tests/$t.lua
done
run "jitoff-t7_bench" JIT=0 -- ./pausehost.exe tests/t7_bench.lua micro
# With the xpcall/load checkpoint wrappers.
for t in t1_parallel t3_semantics t4_cboundary t5_abort; do
  run "ckpt-$t" CHECKPOINT=1 -- ./pausehost.exe tests/$t.lua
done
# Very frequent pauses (every 20-60 us) on the scheduler and nesting tests.
run "fast-t1_parallel" PAUSE_MIN_US=20 PAUSE_MAX_US=60 -- ./pausehost.exe tests/t1_parallel.lua
run "fast-t2_nesting" PAUSE_MIN_US=20 PAUSE_MAX_US=60 -- ./pausehost.exe tests/t2_nesting.lua
run "fast-t3_semantics" PAUSE_MIN_US=20 PAUSE_MAX_US=60 -- ./pausehost.exe tests/t3_semantics.lua
# Abort latency statistics: test 5 ten times.
for i in 1 2 3 4 5 6 7 8 9 10; do
  run "t5_abort-rep$i" -- ./pausehost.exe tests/t5_abort.lua
done
grep -h "abort latency" logs/t5_abort-rep*.log logs/t5_abort.log logs/assert-t5_abort.log logs/jitoff-t5_abort.log \
  | awk '{print $3}' | sort -n | awk '{a[NR]=$1} END {print "    hard-abort latency over " NR " runs: min " a[1] " us, median " a[int((NR+1)/2)] " us, max " a[NR] " us"}'
# Control: the naive scheme (plain hook-yield) must FAIL the scheduler test.
run "naive-t1_parallel" NAIVE=1 -- ./pausehost.exe tests/t1_parallel.lua
fails=$((fails-1))  # The naive control is expected to fail.
# Crash demos (separate processes): unlimited nesting overflows a 1 MB stack.
./pausehost.exe --native-nest > logs/native-nest.log 2>&1; echo "=== native nesting: exit $? (process crashed; PowerShell shows 0xC00000FD = stack overflow); $(tail -1 logs/native-nest.log)"
./pausehost.exe --wrapper-nest > logs/wrapper-nest.log 2>&1; echo "=== wrapper nesting without limits: exit $?; $(tail -1 logs/wrapper-nest.log)"
echo "failing runs (excluding the naive control): $fails"
exit $fails
