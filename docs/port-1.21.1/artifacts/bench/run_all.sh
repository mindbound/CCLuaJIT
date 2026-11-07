#!/usr/bin/env bash
# Runs the CPU benchmark suite on all runtimes; raw output goes to logs/<tag>.log
# Usage: bash run_all.sh <tag-suffix>
set -u
W="$(cd "$(dirname "$0")" && pwd -W)"
SUF="${1:-r1}"
CP="$(cat "$W/cobalt/classpath.txt")"
LJ="$W/luajit/src/luajit.exe"
PIN="$W/tools/pin.exe C03C03"   # 8 P-cores of the Core Ultra 9 285HX (EfficiencyClass 1)
PARAMS="FIB_N=34 MANDEL_I=600 TABLES_K=15 OOP_STEPS=8000 STRINGS_K=20 SHA_ROUNDS=160 FB_FRAMES=25000 SER_REPS=80 LZW_REPS=80 COMPILE_REPS=40"
# b10_compile.lua is generated: python -I gen_b10_compile.py <cc-tweaked-1.21.1-core-1.121.0.jar> lua/b10_compile.lua
BENCHES="b01_fib b02_mandel b03_tables b04_oop b05_strings b06_sha256 b07_framebuf b08_serialize b09_lzw b10_compile"
cd "$W/build"
mkdir -p "$W/logs"

echo "== PUC Lua 5.4" >&2
L="$W/logs/lua54_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN lua -e "ITERS=5 $PARAMS" $b.lua >> "$L" 2>&1; done

echo "== LuaJIT JIT (bit32=bit, signed: what CCLuaJIT does)" >&2
L="$W/logs/luajit_jit_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN "$LJ" -e "ITERS=11 $PARAMS" $b.lua >> "$L" 2>&1; done

echo "== LuaJIT JIT, unsigned bit32 shim (sha256 only)" >&2
L="$W/logs/luajit_jit_u32_$SUF.log"; : > "$L"
$PIN "$LJ" -e "ITERS=11 BIT32_MODE='unsigned' $PARAMS" b06_sha256.lua >> "$L" 2>&1

echo "== LuaJIT -joff (interpreter only)" >&2
L="$W/logs/luajit_joff_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN "$LJ" -joff -e "ITERS=5 $PARAMS" $b.lua >> "$L" 2>&1; done

echo "== LuaJIT -joff + count hook every 1000 instructions" >&2
L="$W/logs/luajit_joff_hook_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN "$LJ" -joff -e "ITERS=3 $PARAMS debug.sethook(function() end, '', 1000)" $b.lua >> "$L" 2>&1; done

echo "== LuaJIT JIT + count hook every 1000 instructions" >&2
L="$W/logs/luajit_jit_hook_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN "$LJ" -e "ITERS=5 $PARAMS debug.sethook(function() end, '', 1000)" $b.lua >> "$L" 2>&1; done

echo "== Cobalt 0.9.9 via CC:T CobaltLuaMachine, fresh JVM per benchmark" >&2
L="$W/logs/cobalt_fresh_$SUF.log"; : > "$L"
for b in $BENCHES; do $PIN java -Xmx2g -cp "$W/cobalt/classes;$CP" CobaltBench --prefix "ITERS=10 $PARAMS" $b.lua >> "$L" 2>&1; done

echo "== Cobalt, one JVM, all benchmarks, 2 rounds" >&2
L="$W/logs/cobalt_shared_$SUF.log"; : > "$L"
$PIN java -Xmx2g -cp "$W/cobalt/classes;$CP" CobaltBench --rounds 2 --prefix "ITERS=5 $PARAMS" $(for b in $BENCHES; do echo $b.lua; done) >> "$L" 2>&1
echo "done" >&2
