#!/bin/bash
# Final verification of the 01-08 build. Writes logs into $W/final/.
W=<scratch>/work/proto-patterns-cstack
T=$W/tree/src
F=$W/final
mkdir -p $F
cd $W/harness
gcc -O2 -DHAVE_LUAJIT_INTERRUPT -I$T harness.c $T/libluajit.a -lwinmm -o harness-final.exe || exit 1
gcc -O2 -I$T pharness.c $T/libluajit.a -lwinmm -o pharness-final.exe || exit 1
gcc -O2 -DHAVE_CSTACKGUARD -I$T cstack.c $T/libluajit.a -o cstack-final.exe || exit 1
gcc -O2 -I$T runlua.c $T/libluajit.a -lwinmm -o runlua-final.exe || exit 1

echo "== preempt harness (intr error, all cases)" > $F/preempt.txt
./run_all.sh final intr >> $F/preempt.txt 2>&1
{
  echo "== yield"; timeout 60 ./harness-final.exe intr yield x
  echo "== nested"; timeout 20 ./harness-final.exe intr nested x
  for c in pcall_swallow xpcall_handler_loop coroutine_swallow; do
    echo "== hardabort $c error"; timeout 20 ./harness-final.exe intr hardabort $c 300
    echo "== hardabort $c yield"; timeout 20 ./harness-final.exe intr hardabort $c 300 yield
  done
} >> $F/preempt.txt 2>&1

echo "== compat_check.lua" > $F/compat.txt
$T/luajit.exe <repo>/docs/port-1.21.1/artifacts/lua/compat_check.lua >> $F/compat.txt 2>&1

./run_pharness.sh pharness-final.exe error pcall hard hardyield yieldbad pause gcstress > $F/pharness.txt 2>&1

python -I run_cstack.py cstack-final.exe 1024 128 > $F/cstack-1m-128k.txt 2>&1
python -I run_cstack.py cstack-final.exe 256 64 > $F/cstack-256k-64k.txt 2>&1

for seed in 1 2 3 4; do
  $W/luajit06.exe $W/lua/patfuzz.lua $seed 20000 > $W/fuzz/fb_$seed.txt
  STRESS_MS=1 ./runlua-final.exe $W/lua/patfuzz.lua $seed 20000 > $W/fuzz/ff_$seed.txt 2> $W/fuzz/ff_$seed.err
  if cmp -s $W/fuzz/fb_$seed.txt $W/fuzz/ff_$seed.txt; then r=IDENTICAL; else r=DIFFERENT; fi
  echo "seed $seed: $(wc -l < $W/fuzz/fb_$seed.txt) lines, base vs final(+1ms stress): $r; $(cat $W/fuzz/ff_$seed.err)"
done > $F/fuzz.txt
$T/luajit.exe $W/lua/chunkfuzz.lua > $W/fuzz/chunk_final.txt
$W/luajit06.exe $W/lua/chunkfuzz.lua > $W/fuzz/chunk_base.txt
if cmp -s $W/fuzz/chunk_base.txt $W/fuzz/chunk_final.txt; then echo "chunk-boundary differential: IDENTICAL ($(head -1 $W/fuzz/chunk_base.txt))"; else echo "chunk-boundary differential: DIFFERENT"; fi >> $F/fuzz.txt
echo "final verify done"
