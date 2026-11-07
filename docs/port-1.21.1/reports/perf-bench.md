# perf-bench: LuaJIT vs Cobalt 0.9.9 for CC:Tweaked workloads, and the cost of Lua -> Java calls

Lane: how much faster LuaJIT really is than Cobalt 0.9.9 (the Lua VM in CC:T 1.121.0 for MC 1.21.1) on CC-style workloads, and how much a Lua -> Java call costs through JNI.
All work dir paths below are relative to
`<scratch>\work\perf-bench` (written `$W`).

## TL;DR

* **Pure-Lua CPU work: LuaJIT with the JIT on is 8.9x faster than warm Cobalt (geometric mean over 10 benchmarks; range 2.3x to 55x).** Against Cobalt's best single iteration it is 8.3x. Against Cobalt in a JVM that has run many kinds of Lua (closer to a long-running server) it is 10.5x. Number crunching, bit operations and function/method calls gain the most (fib 26x, mandelbrot 29x, OOP 18x, SHA-256 55x). Work that mostly calls library functions on strings and tables gains 3.5x to 6x (tables, strings, framebuffer, serialize, LZW). `load()`/compiling gains 2.3x.
* **LuaJIT's interpreter alone (`-joff`) is 3.7x faster than warm Cobalt (range 2.2x to 8x).** With a count hook every 1000 instructions (a preemption method that works in the interpreter) it is still **3.2x**. This is the floor for a "safe" build that turns the JIT off.
* **Count hooks are ignored inside JIT-compiled code.** JIT + count-hook timings are the same as plain JIT, so that combination gives no preemption. LuaJIT's profiler in `'l'` mode compiles guard checks into traces and costs about nothing (9.1x vs 8.9x geomean). Its callback does run inside a JIT-compiled `while true do end`, so guard-based preemption looks practical. However, every simple Lua-level abort I tried either PANICked or segfaulted. This needs C-level work (details in section 5).
* **A Lua -> Java call through JNI costs more than the same call in Cobalt.** With CCLuaJIT's marshalling style (Object[] in and out, boxed numbers, strings decoded through a Java upcall), each call is **3x to 10x slower than Cobalt's own CC:T call path**: nop 102 vs 29-36 ns, number 262 vs 41-54 ns, `term.blit`-shaped call 489 vs 47-64 ns. A lean JNI bridge (primitive signatures, byte[]) roughly matches Cobalt for numbers (30-53 ns) and costs 2x to 4x more for strings. All of these are below one microsecond, so they matter only in loops that make hundreds of thousands of calls.
* **Two serious bugs in the JNI bridge code, both reproduced in my faithful copy:**
  * (a) JNI local references are never freed until the Lua code yields. That is +32 MB of native memory per 1M calls, and every boxed argument and result stays alive. This caused **240-510 ms stop-the-world GC pauses**, against 2 ms with a one-line fix.
  * (b) Strings are not 8-bit clean. Bytes 0x80-0xFF become U+FFFD (3 bytes each), NUL becomes `C0 80`, and a byte[] result is cut off at its first NUL.
* **Where players would notice:** CPU-bound Lua such as crypto (SHA-256 at 0.9 MB/s on Cobalt vs 20-50 MB/s), compression, image decoding, 3D/ray rendering, emulators, path-finding, and big sorts or serialization. Programs that now hit "Too long without yielding" after 7 s would finish. **Where they would not:** anything waiting on events, timers, rednet, turtle moves (8 ticks), main-thread peripheral calls (at least 1 tick = 50 ms each, e.g. inventory `pushItems`/`list`), or screen redraws (at most 20 Hz, and only tens of microseconds of VM time per frame on either VM).

## 1. Setup and methodology

**Machine.** Intel Core Ultra 9 285HX (8 P-cores + 16 E-cores), Windows 11, about 30% background CPU load from other processes (Firefox and other agents) during the runs. To reduce noise from the hybrid cores, every measured process was pinned to the 8 P-cores (affinity mask `0xC03C03`, i.e. logical CPUs 0,1,10-13,22,23, found with `$W/tools/cpusets.exe` / `GetSystemCpuSetInformation`) using a small launcher `$W/tools/pin.exe` (source `$W/tools/pin.c`). Child processes inherit the affinity.

**Runtimes**
| Runtime | Details |
|---|---|
| Cobalt 0.9.9 | Run inside the **real `dan200.computercraft.core.lua.CobaltLuaMachine` from `cc-tweaked-1.21.1-core-1.121.0.jar`** on JDK 21.0.13 (Temurin, G1, `-Xmx2g`). The benchmark source is passed as the "bios" and run with `handleEvent(null, null)`. The `LuaState` is therefore built exactly as CC:T builds it (CobaltLuaMachine.java:59-89: interrupt handler, `CoreLibraries.debugGlobals`, `Bit32Lib`, `ErrorInfoLib`, `_HOST`, `_CC_DEFAULT_SETTINGS`), and host functions go through CC:T's generated `LuaMethod` -> `ResultInterpreterFunction` -> `VarargArguments` path. Clock and print come from an `ILuaAPI` named `bench` (`$W/cobalt/CobaltBench.java`). Extra jars: guava 32.1.2, failureaccess, slf4j 2.0.9 (+nop), jspecify, fastutil (`$W/deps`). `TimeoutState` is a no-op subclass, so the run never pauses or aborts. That matches a single busy computer on an otherwise idle computer thread. |
| LuaJIT 2.1 ROLLING | x64, GC64, MinGW build (copy of the shared prebuilt in `$W/luajit`), driven by `luajit.exe`. Variants: JIT on; `-joff`; `-joff` + `debug.sethook(f,'',1000)`; JIT + the same hook; JIT + `jit.profile.start('li10'/'fi10', noop)`. |
| PUC Lua 5.4.6 | Reference only. |

**Benchmarks** (`$W/lua/b01..b10*.lua`; built as `compat.lua + bNN.lua + driver.lua` into `$W/build/`)
* There is one Lua-5.1-compatible source per benchmark. The compat prologue (`$W/lua/compat.lua`) picks the clock and output functions and provides `bit32`:
  * Cobalt: its own `bit32`.
  * LuaJIT: `bit32 = bit` by default (signed results, as CCLuaJIT does), with `rrotate`/`lrotate` added as aliases of `ror`/`rol`. With `BIT32_MODE='unsigned'` it uses a correct wrapper instead.
  * Lua 5.4: built from native operators inside a `load()`ed string, so 5.1 parsers never see them.
* The sources use no `//` or bitwise operators.
* **Every run's checksum is identical on all runtimes** (see the summary output). SHA-256 output was also checked against Python `hashlib`.

| name | what it does | size parameter |
|---|---|---|
| fib | naive recursive fib(34), about 18.5M calls | FIB_N=34 |
| mandel | 320x240 Mandelbrot, max 600 iterations, float loops | MANDEL_I=600 |
| tables | array fill/scan, `table.sort` (numbers and records with a comparator), `table.insert`/`remove` (stack and front-insert queue), word-count hash map | TABLES_K=15 |
| oop | metatable classes with 2-level inheritance, method calls, `__add`, closures (100 entities x 8000 steps) | OOP_STEPS=8000 |
| strings | `table.concat` building, `byte`/`char`/`sub` loops (ROT13), `string.format`, `gmatch`/`gsub`/`find` | STRINGS_K=20 |
| sha256 | pure-Lua SHA-256 with `bit32.band`/`bxor`/`bnot`/`rshift`/`rrotate`, 160 chained hashes of 4 KiB | SHA_ROUNDS=160 |
| framebuf | 51x19 text/fg/bg line buffers (like CC's `window` API): boxes and text spliced in with `sub`/`rep`/`..`, then per-line "blit" reads, 25,000 frames | FB_FRAMES=25000 |
| serialize | textutils.serialize-style recursive serializer (`%q`, identifier keys, cycle tracking) plus `load('return '..s)` deserializer on 300 item records, 80 times | SER_REPS=80 |
| lzw | LZW compress and decompress of a 42.7 KB word stream, round-trip checked, 80 times | LZW_REPS=80 |
| compile | `loadstring`/`load` of **all 105 CC:T 1.121.0 ROM .lua files (718 KB)**, 40 times (generated by `$W/tools/gen_compile_bench.py`). All 105 files also parse in LuaJIT. | COMPILE_REPS=40 |

**Protocol** (`$W/run_all.sh <tag>`, three full rounds r1-r3 plus an extra shared-JVM pass r4; `$W/run_profmode.sh r2` for the profiler variants)
* **Cobalt, fresh JVM per benchmark:** 10 iterations. "Cold" is iteration 1, "warm" is the median of iterations 4-10, and "best" is the minimum.
* **Cobalt, shared JVM:** all 10 benchmarks run twice in one JVM, 5 iterations each, and I report the median of the second pass. This models a long-running server where HotSpot has compiled Cobalt's interpreter loop for many kinds of Lua code.
* **LuaJIT JIT:** 11 iterations per process. I report iteration 1 and the median of iterations 2-11. `-joff` and Lua 5.4: median of 5 iterations. Hook variants: 3 iterations (`-joff`) or 5 (JIT).
* Every cell is the **median across rounds** of the per-round statistic. Summaries were produced with `python -I $W/summarize.py $W/logs r1 r2 r3 r4` and saved to `$W/logs/summary_r1-r4.md`.
* Timer resolution: `os.clock` on Windows/MSVCRT has 1 ms resolution. For the shortest LuaJIT runs (12-20 ms: sha256, mandel) the ratio is therefore only accurate to about +/-8%. Cobalt uses `System.nanoTime`.

## 2. Results: CPU benchmarks (seconds per run; lower is better)

| benchmark | Cobalt cold | Cobalt warm | Cobalt best | Cobalt shared-JVM | LuaJIT JIT 1st | **LuaJIT JIT** | **LuaJIT -joff** | -joff + hook/1000 | JIT + hook/1000 | JIT + profile 'l' | JIT + profile 'f' | PUC 5.4 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.880 | 1.068 | 0.836 | 1.165 | 0.041 | **0.042** | **0.179** | 0.200 | 0.038 | 0.037 | 0.043 | 0.208 |
| mandel | 0.734 | 0.551 | 0.531 | 0.948 | 0.018 | **0.019** | **0.109** | 0.178 | 0.017 | 0.017 | 0.018 | 0.179 |
| tables | 1.939 | 1.525 | 1.339 | 1.473 | 0.285 | **0.267** | **0.359** | 0.454 | 0.248 | 0.272 | 0.248 | 0.547 |
| oop | 0.963 | 0.828 | 0.814 | 1.040 | 0.046 | **0.045** | **0.224** | 0.246 | 0.044 | 0.043 | 0.047 | 0.366 |
| strings | 0.794 | 0.380 | 0.369 | 0.426 | 0.078 | **0.079** | **0.134** | 0.146 | 0.074 | 0.075 | 0.081 | 0.325 |
| sha256 | 0.962 | 0.737 | 0.697 | 0.833 | 0.014 | **0.013** | **0.092** | 0.119 | 0.012 | 0.013 | 0.013 | 0.348 |
| framebuf | 1.190 | 0.875 | 0.867 | 1.158 | 0.203 | **0.222** | **0.404** | 0.438 | 0.190 | 0.184 | 0.209 | 0.649 |
| serialize | 1.249 | 0.835 | 0.797 | 0.888 | 0.188 | **0.182** | **0.272** | 0.277 | 0.183 | 0.173 | 0.189 | 0.386 |
| lzw | 0.847 | 0.611 | 0.593 | 0.753 | 0.176 | **0.174** | **0.240** | 0.269 | 0.165 | 0.249 | 0.186 | 0.367 |
| compile | 0.913 | 0.639 | 0.628 | 0.669 | 0.304 | **0.277** | **0.218** | 0.221 | 0.217 | 0.286 | 0.239 | 0.260 |

### Speedups (Cobalt time divided by the other runtime's time)

| benchmark | **JIT vs Cobalt warm** | JIT vs Cobalt best | JIT vs Cobalt shared-JVM | JIT vs Cobalt cold | **-joff vs Cobalt warm** | -joff vs Cobalt best | -joff+hook vs Cobalt warm | JIT+profile 'l' vs Cobalt warm | PUC 5.4 vs Cobalt warm |
|---|---|---|---|---|---|---|---|---|---|
| fib | **25.7x** | 20.1x | 28.1x | 21.2x | **6.0x** | 4.7x | 5.3x | 28.5x | 5.1x |
| mandel | **29.0x** | 27.9x | 49.9x | 38.6x | **5.1x** | 4.9x | 3.1x | 32.4x | 3.1x |
| tables | **5.7x** | 5.0x | 5.5x | 7.3x | **4.2x** | 3.7x | 3.4x | 5.6x | 2.8x |
| oop | **18.2x** | 17.9x | 22.9x | 21.2x | **3.7x** | 3.6x | 3.4x | 19.0x | 2.3x |
| strings | **4.8x** | 4.7x | 5.4x | 10.1x | **2.8x** | 2.8x | 2.6x | 5.1x | 1.2x |
| sha256 | **54.6x** | 51.7x | 61.7x | 71.3x | **8.0x** | 7.6x | 6.2x | 59.0x | 2.1x |
| framebuf | **4.0x** | 3.9x | 5.2x | 5.4x | **2.2x** | 2.1x | 2.0x | 4.7x | 1.3x |
| serialize | **4.6x** | 4.4x | 4.9x | 6.8x | **3.1x** | 2.9x | 3.0x | 4.8x | 2.2x |
| lzw | **3.5x** | 3.4x | 4.3x | 4.9x | **2.5x** | 2.5x | 2.3x | 2.5x | 1.7x |
| compile | **2.3x** | 2.3x | 2.4x | 3.3x | **2.9x** | 2.9x | 2.9x | 2.2x | 2.5x |
| **geomean** | **8.9x** | **8.3x** | **10.5x** | **11.8x** | **3.7x** | **3.5x** | **3.2x** | **9.1x** | **2.2x** |
| range | 2.3-54.6x | 2.3-51.7x | 2.4-61.7x | 3.3-71.3x | 2.2-8.0x | 2.1-7.6x | 2.0-6.2x | 2.2-59.0x | 1.2-5.1x |

### SHA-256 and `bit32` semantics
* With `bit32 = bit` (what CCLuaJIT does) sha256 takes 0.013 s.
* With a correct unsigned `bit32` wrapper it takes **0.033 s**. That is 2.5x slower than the signed version but still **22x faster than Cobalt**.
* Throughput (4 KiB blocks):

| Runtime | MB/s |
|---|---|
| Cobalt warm | ~0.9 |
| LuaJIT JIT, correct shim | ~20 |
| LuaJIT JIT, signed `bit` | ~50 |
| LuaJIT `-joff` | ~7 |

### Observations
1. **The JIT gain depends on the workload.**
   * Very large (18-55x) for numeric work, bit operations and Lua-to-Lua calls or metatable dispatch. LuaJIT inlines and specialises these; Cobalt pays roughly 45-55 ns for each Lua call.
   * Modest (3.5-6x) when time is spent in library C functions on strings and tables (`sub`, `..`, `rep`, `gsub`, `table.sort` with a Lua comparator, `string.format`, `pairs`).
   * Smallest (2.3-3x) for `load()`/compiling. That is C code in LuaJIT, so the JIT changes nothing: `-joff` is the same speed.
2. **The interpreter-only floor is 2-8x (3.7x geomean).** LuaJIT's hand-written assembly interpreter beats Cobalt, and Cobalt is itself about 2.2x slower than PUC Lua 5.4.
3. **A count hook every 1000 instructions costs about 13% in the interpreter** (3.7x down to 3.2x geomean; up to -40% on mandelbrot). The hook makes the interpreter dispatch through `lj_dispatch_ins` for every instruction.
4. **JIT + count hook runs as fast as plain JIT** (fib 0.038 vs 0.042 s, etc.). That is not a good sign: it is consistent with the hook simply not firing inside traces (already established), so the combination gives no preemption.
5. **Cobalt's speed varies a lot:**
   * Iteration 1 in a fresh JVM is up to 2x slower (strings 0.79 vs 0.38 s). Warm-up takes several seconds of Lua execution.
   * After warm-up some benchmarks get *slower* (fib: 0.84 s on iteration 1, about 1.0-1.4 s afterwards; HotSpot recompiles the large interpreter method with a different profile).
   * In a shared JVM (polluted profile) mandel is 0.95 s vs 0.55 s and oop 1.04 vs 0.83 s.
   * One shared-JVM pass (r3) had a pathological episode where `compile` took 4.8-5.5 s per iteration instead of 0.65 s. It did not reproduce in r2 or r4 (`$W/logs/cobalt_shared_r3.log`).
   * Cobalt allocates all its values on the Java heap. A GC log of a shared-JVM pass (`$W/logs/cobalt_shared_r4_gc.log`) shows 77 young-GC pauses totalling 2.2 s in 104 s (2%), median 6 ms, p90 69 ms, max 328 ms. In Minecraft those pauses stop the whole server, not just the computer thread.
   * LuaJIT times are steady by comparison: the first iteration matches the warm runs.

## 3. Results: host-call overhead (ns per call; 10M calls, median of 3 repetitions after 1M warm-up calls)

Harnesses:
* LuaJIT side: `$W/hostcall/jnibench.cpp` (C++, built with MinGW g++ 16.2 against JDK 21 `include` and `include/win32`, linked to `lua51.dll`), `$W/hostcall/JniBench.java` and `$W/hostcall/hostcall.lua`. The JVM `System.load()`s `lua51.dll` and `jnibench.dll`, then a native method creates a LuaJIT state and runs the Lua loops.
* The "boxed" variants copy CCLuaJIT's `invoke_java_fn`, `to_java_values`, `to_java_value`, `to_lua_value` and `to_lua_values` logic from `src/main/cpp/main.cpp` on the 1.20.1_CCT branch (lines 259-466) line for line. That includes `GetEnv` on every call, `Double.valueOf` through `CallStaticObjectMethod`, strings via `NewByteArray` plus the `decodeString` upcall (US_ASCII, as in the 1.14.4 `LuaJITMachine.decodeString`), the `IsInstanceOf` chain, `GetStringUTFChars` + `lua_pushstring`, and the `yieldRequested` field read. The only change is that a `PushLocalFrame`/`PopLocalFrame` pair was added; the leak without it is measured separately below.
* "port" = the same boxed bridge, but the Java side dispatches the way a 1.21.1 port would have to: `LuaMethod.apply(target, ctx, new ObjectArguments(args)).getResult()`, using CC:T's own generated `LuaMethod` for an `@LuaFunction` API.
* Cobalt side: `$W/cobalt/CobaltHost.java` + `$W/hostcall/hostcall_cobalt.lua`, (1) raw Cobalt `LibFunction`s in a CC:T-like `LuaState` and (2) the real `CobaltLuaMachine` with an `ILuaAPI` (`CobaltBench.BenchAPI`).
* Logs: `$W/logs/hostcall_jni_r2.log`, `hostcall_jni_blit_r1.log`, `hostcall_cobalt_r1.log`, `hostcall_cobalt_r2.log`.

| path | nop | num(double) -> double | 100-byte string in | 100-byte string out | term.blit shape (3 x 100 B in) |
|---|---|---|---|---|---|
| Lua -> Lua function (baseline) | LJ JIT 0.39 (inlined); LJ -joff 5.9; Cobalt 49-57 | | | | |
| **(a) LuaJIT -> lua_CFunction**, JIT on / -joff | 10.4 / 3.9 | 12.7 / 7.3 | 11.7 / 5.5 | 13.6 / 10.2 | - |
| LuaJIT FFI call (JIT / -joff), reference only, unsafe in a sandbox | - | 0.42 / 28.2 | - | - | - |
| **(c) lean JNI** (primitive signature, cached env), JIT / -joff | 30.5 / 22.6 | 53.2 / 45.6 | byte[] 112.7 / 79.2; direct ByteBuffer 133.2 / 109.8; `NewString` 187.6 / 185.1 | byte[] 90.5 / 80.1; String via `GetStringRegion` 123.2 / 116.4 | 3 x byte[] 222 / 160 |
| **(b) CCLuaJIT-style boxed JNI** (+ local frame), JIT / -joff | 102.5 / 116.1 | 261.9 / 275.1 | 227.5 / 291.4 | 310.5 / 304.0 | 489 / 658 |
| (b') boxed + CC:T `LuaMethod`/`ObjectArguments` (realistic port), JIT / -joff | 120.5 / 140.9 | 279.8 / 356.6 | 291.9 / 241.5 | 334.4 / 321.8 | 696 / 655 |
| Cobalt raw `LibFunction` (2 runs) | 24.3-25.0 | 20.9-22.7 | 23.8-25.8 | 25.6-25.7 | - |
| **(d) Cobalt via CC:T's real path** (`ResultInterpreterFunction`, `VarargArguments`), 2 runs | 29.4-36.2 | 41.4-54.3 | `Coerced<String>` (= `term.write`) 94-153; `ByteBuffer` 37-58 | String 151-220 | 3 x `ByteBuffer` (= `term.blit`) 47-64 |

Interpretation:
* **Calling Java from JIT-compiled LuaJIT code is not free.** A `lua_CFunction` call costs about 10 ns under the JIT (the trace exits and the next trace is "stitched" on), more than the about 4 ns in the interpreter. The JNI transition adds at least 20 ns. Each boxed argument or result adds about 60-80 ns, because every `Double.valueOf`/`doubleValue` is itself a JNI upcall.
* **Cobalt's CC:T call path is cheap** (30-55 ns, roughly the cost of a Lua -> Lua call in Cobalt). Its `ByteBuffer` arguments wrap the Lua string's bytes without copying (47-64 ns for a 3-buffer `blit`). **The CCLuaJIT-style bridge makes host calls 3.5-10x more expensive than Cobalt's.** A lean bridge gets nop and number calls to parity (30-53 ns) and string calls to 2-4x (byte[] copies; JNI cannot hand Java a heap view of LuaJIT memory).
* For a realistic port, the Java-side dispatch through `LuaMethod` + `ObjectArguments` adds only 15-20 ns over a hand-written switch. The JNI marshalling is the cost, not CC:T's dispatch.
* What this means for a frame. Composing measured numbers for a UI frame like `framebuf` that also does 19 `term.blit` calls:

  | Configuration | Per frame | vs Cobalt |
  |---|---|---|
  | Cobalt | about 35.0 µs of Lua + 19 x 0.055 µs = **about 36 µs** | 1x |
  | LuaJIT + CCLuaJIT-style bridge | 8.9 + 19 x 0.49 = **about 18 µs** | 2.0x |
  | LuaJIT + lean bridge | 8.9 + 19 x 0.22 = **about 13 µs** | 2.7x |

  All of these are far below one 50 ms tick, so players would not notice.
* A loop that does nothing but host calls (e.g. 100k `term.setCursorPos`/`term.write` between yields) **would be slower on CCLuaJIT than on Cobalt**: about 0.3 µs vs 0.05-0.15 µs per call.

### 3.1 Leak in CCLuaJIT's call path (faithful copy, no local frame; `$W/logs/hostcall_leak_r1.log`, `hostcall_leak_gc.log`, `hostcall_leakfix_gc.log`)
`invoke_java_fn` creates several local references on every call: the argument `Object[]`, one boxed `Double`/`Boolean`/`String` per argument, a `byte[]` per string argument, and the result array. It never deletes them. JNI local refs live until the outermost native frame (`handleEvent`) returns, which happens only when the Lua program yields. 5M calls of a 1-argument, 1-result function without yielding gave:

| 1M calls done | private bytes, leaky | elapsed, leaky | private bytes, with PushLocalFrame/PopLocalFrame | elapsed, with frame |
|---|---|---|---|---|
| 1 | +31 MB | 0.27 s | +0 MB | 0.28 s |
| 2 | +60 MB | 1.42 s | +1 MB | 0.54 s |
| 3 | +97 MB | 2.12 s | +1 MB | 0.81 s |
| 4 | +123 MB | 2.88 s | +1 MB | 1.06 s |
| 5 | +160 MB | 3.41 s | +1 MB | 1.33 s |

* GC, leaky version: `Pause Young 104M->97M 423ms`, `186M->191M 468ms`, `280M->286M 427ms`, `375M->379M 510ms`, `468M->473M 240ms`. Nothing can be reclaimed, and each pause has to scan millions of JNI handles.
* GC, fixed version: `104M->3M 2.6ms`, `117M->3M 1.9ms`.
* In a server these are stop-the-world pauses of 5-10 ticks each, and heap growth heads toward an OOM, all caused by one computer running a tight call loop within its 7-second allowance.

### 3.2 String marshalling is not 8-bit clean (probes in `hostcall.lua`, output in `$W/logs/hostcall_jni_r2.log`)

| Probe | Sent | Received | Cause |
|---|---|---|---|
| Echo through the boxed path | bytes 0..255 (256 bytes) | **513 bytes**, all 256 positions different | `decodeString` (US_ASCII) turns 0x80-0xFF into U+FFFD; `GetStringUTFChars` emits 3 bytes for each U+FFFD and `C0 80` for NUL |
| Java returns a 256-byte `byte[]` | 256 bytes starting with 0 | **0-byte** Lua string | `lua_pushstring` stops at the first NUL |
| Echo of `"abc\0def"` | 7 bytes | 8 bytes | NUL becomes `C0 80` |

CC relies on 8-bit strings: `term.blit` and the drawing characters 128-159, `fs` binary mode, `http` bodies, `string.pack`, rednet payloads, and `textutils`/`cc.audio.dfpwm`.

## 4. `bit32` compatibility, measured from my lane
CCLuaJIT sets `bit32 = bit` (`main.cpp`:711-714 on 1.20.1_CCT). Verified with `luajit.exe`:
* LuaJIT's `bit` has no `extract`, `replace`, `btest`, `lrotate` or `rrotate`.
* It returns **signed** results: `bit.band(0xFFFFFFFF, 0xFFFFFFFF) = -1`, where `bit32` gives 4294967295.
* It takes shift counts mod 32: `bit.lshift(1, 32) = 1`, where `bit32` gives 0.

CC:T's own ROM uses `bit32.extract` in `rom/apis/textutils.lua:436` (JSON `\u` surrogates) and `bit32.arshift` in `bios.lua:27`, `textutils.lua:763` and `cc/audio/dfpwm.lua:64`. Common CC SHA-256/ChaCha libraries use `bit32.rrotate`.

A correct unsigned shim, applied to sha256, costs 2.5x compared with raw `bit`, but is still about 22x faster than Cobalt (section 2).

## 5. Preemption and the JIT (performance side only; `$W/preempt/*.lua`, `$W/logs/preempt_r1.log`)
* `debug.sethook(count)`: no effect inside traces (JIT + hook timings equal plain JIT; the infinite-loop case was already established).
* `jit.profile.start('li10', cb)`:
  * The profiler compiles `IR_PROF` guards into traces at every line change (`lj_record.c`:725-760).
  * The callback **does** run while a JIT-compiled `while true do x = x + 1 end` is executing.
  * Throughput cost of running all benchmarks with the 'l' profiler active: **geomean 9.1x vs 8.9x without it** (roughly free; lzw dropped from 3.5x to 2.5x, everything else was within noise).
  * Mode `'f'` (guards on function entry only) and the default `'i10'` mode **do not** regain control from a call-free loop (`exit=124`, timeout).
* **Aborting from that callback does not work as written:**
  * `error()` in the callback gives `PANIC: unprotected error` (the callback is called from C with no protected frame).
  * A count hook armed with count 1 fires inside the callback and also PANICs.
  * A count hook armed with count 20 is **silently dropped**, even with `-joff`. `lj_profile_interpreter` saves `g->hookmask` before the callback and restores it afterwards (`lj_profile.c`:132-144).
  * `prof.stop()` + `debug.sethook` inside the callback **segfaulted** (exit 139).
* Conclusion: keeping the JIT on with guard-based preemption looks cheap in throughput (about 0-30%), but needs a C-level mechanism, e.g. a small LuaJIT patch or a native profile callback that sets the hook after `lj_profile_interpreter` restores the mask. It also needs careful testing. The fallback with the JIT off and count hooks still gives about 3.2x.

## 6. What it means for real CC:T usage
How CC:T 1.121.0 schedules work:
* All computers share `computer_threads` = **1** thread by default (`ConfigSpec.java`:161-167).
* `ComputerThread` gives each busy computer time slices sized from a 50 ms latency target, with a 5 ms minimum (`ComputerThread.java`:78-91). It pauses a computer only when others are waiting.
* A computer that runs 7 s without yielding is soft-aborted ("Too long without yielding"), and hard-aborted 1.5 s later (`TimeoutState.java`:46-61).
* Main-thread work is budgeted per tick (`max_main_global_time`, `max_main_computer_time`).
* `@LuaFunction(mainThread = true)` methods run on the next server tick, so each one waits at least 1 tick (50 ms) no matter which VM is used. These include the generic inventory, fluid and energy methods, the command API, the disk drive, the printer, and pocket/turtle storage (`AbstractInventoryMethods`, `TurtleAPI`, ...). Turtle commands go through `executeCommand` and wait for `turtle_response`; a move takes about 8 ticks.

**Players would notice it in:**
* **Crypto and hashing** (SHA-256 / HMAC / PBKDF2, ECC signing for secure rednet or Krist wallets, ChaCha20): 20-55x faster with the JIT. PBKDF2 or key generation that takes seconds on Cobalt becomes instant, and Cobalt jobs that end with "Too long without yielding" would complete.
* **Numeric and simulation work** (3D renderers such as raycasters, mandelbrot/plotters, physics, neural-net toys, emulators of 6502/CHIP-8/RISC-V CPUs, A* path-finding for turtles on large maps): 18-30x faster.
* **Compression, image/video decoding, data processing** (LZW/deflate/QOI/BIMG, big `table.sort`s, serialisation of large inventories or databases): 3.5-6x faster with the JIT, about 2.5-4x with the interpreter only.
* **Server-wide headroom:** with one shared computer thread, less CPU per unit of work means more busy computers before they start delaying each other. There is also less Java-heap allocation (Cobalt's GC pauses reach about 0.3 s under heavy load). This only holds if the bridge does not leak local refs (section 3.1).

**Players would not notice it in:**
* **Event- and IO-bound programs**, which is most CC programs: rednet servers, door locks, reactor and farm controllers, chat bots, shells and editors spend almost all their time yielded in `os.pullEvent`.
* **Storage systems and turtles:** dominated by main-thread peripheral calls (at least 50 ms each) and turtle actions (about 400 ms per move). VM time is microseconds per call on either VM.
* **GUI and redraw:** one 51x19 frame costs about 36 µs on Cobalt vs 13-18 µs on LuaJIT. Terminal changes reach clients at most once per tick, so this is invisible either way.
* **Boot and program start:** compiling the whole 718 KB ROM takes about 16 ms on Cobalt (warm) vs about 5.5-7 ms on LuaJIT. A normal boot compiles only a fraction of that.
* **Host-call-bound loops:** as CCLuaJIT is written they get **slower**, not faster (section 3).

**Net:** for typical players the visible gain is limited to CPU-heavy programs. There it is large (3.5-55x with the JIT; 2-8x without it). The port's performance case depends on (1) keeping the JIT on, which needs a working preemption design, and (2) a lean, non-leaking, 8-bit-clean JNI bridge.

## 7. Reproduction
```bash
W=<scratch>/work/perf-bench
# CPU suite (about 8 min per round), all runtimes, raw logs in $W/logs/<runtime>_<tag>.log
bash $W/run_all.sh r1
bash $W/run_profmode.sh r1                       # JIT + jit.profile 'l'/'f'
python -I $W/summarize.py $W/logs r1 [r2 ...]
# JNI host-call benchmark (classpath from $W/cobalt/classpath.txt)
CP="$(cat $W/cobalt/classpath.txt)"; cd $W/hostcall
$W/tools/pin.exe C03C03 java -Xmx2g -cp "$W/hostcall/classes;$CP" JniBench $W/hostcall/bin $W/hostcall/hostcall.lua jit joff
$W/tools/pin.exe C03C03 java -Xmx2g -cp "$W/hostcall/classes;$CP" JniBench $W/hostcall/bin $W/hostcall/hostcall.lua blit blitjoff
$W/tools/pin.exe C03C03 java -Xmx2g -Xlog:gc -cp "$W/hostcall/classes;$CP" JniBench $W/hostcall/bin $W/hostcall/hostcall.lua leak      # or leakfix
# Cobalt host-call benchmark
$W/tools/pin.exe C03C03 java -Xmx2g -cp "$W/cobalt/classes;$CP" CobaltHost $W/hostcall/hostcall_cobalt.lua
# Build JNI DLL
JDK="C:/Program Files/Eclipse Adoptium/jdk-21.0.13.9-hotspot"
g++ -O2 -shared -o $W/hostcall/bin/jnibench.dll -I$W/luajit/src -I"$JDK/include" -I"$JDK/include/win32" \
    $W/hostcall/jnibench.cpp $W/hostcall/bin/lua51.dll -lpsapi -Wl,--kill-at -static-libgcc -static-libstdc++
```

## 8. Files
* Benchmarks: `$W/lua/compat.lua`, `$W/lua/driver.lua`, `$W/lua/b01_fib.lua` ... `$W/lua/b10_compile.lua`; combined runnable files in `$W/build/`.
* Cobalt harnesses: `$W/cobalt/CobaltBench.java` (real `CobaltLuaMachine`), `$W/cobalt/CobaltHost.java`, classes in `$W/cobalt/classes`, classpath in `$W/cobalt/classpath.txt`.
* JNI harness: `$W/hostcall/jnibench.cpp`, `$W/hostcall/JniBench.java`, `$W/hostcall/hostcall.lua`, `$W/hostcall/hostcall_cobalt.lua`, binaries in `$W/hostcall/bin`.
* Runners and tools: `$W/run_all.sh`, `$W/run_profmode.sh`, `$W/summarize.py`, `$W/tools/pin.c`, `$W/tools/cpusets.c`, `$W/tools/gen_compile_bench.py`.
* Preemption experiments: `$W/preempt/profile_interrupt.lua`, `$W/preempt/profile_interrupt2.lua`.
* Logs: `$W/logs/*.log`, summaries in `$W/logs/summary_r1-r4.md`.

## 9. Caveats
* Windows/MinGW LuaJIT build. A Linux server build would behave similarly but was not measured.
* About 30% background load came from other processes; I pinned to P-cores and repeated 3-4 times, and run-to-run spread was about +/-10-30% for Cobalt and below 5% for LuaJIT.
* The benchmarks are synthetic CC-style kernels, not recorded real programs.
* The host-call "boxed" path copies CCLuaJIT's marshalling code but not its full `handleEvent`/yield machinery. The Java callees are trivial.
* Cobalt was measured in a JVM that runs nothing else. In a real server, Minecraft's own allocation and JIT activity would add noise and probably slow Cobalt further (profile pollution).
