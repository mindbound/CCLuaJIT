# verify-perf-bench: adversarial check of the perf-bench claims PB-3, PB-4, PB-5, PB-6

Work dir (written `$V` below): `<scratch>\work\verify-perf-bench`
Investigator dir (read-only for me, written `$P`): `...\scratchpad\work\perf-bench`

I wrote my own harnesses for everything I could. I did not reuse the investigator's binaries, except that I re-ran its compiled Cobalt host-call classes (`CobaltHost`) to check its Cobalt numbers.
* LuaJIT: a private copy of the shared prebuilt (`$V/lj/luajit.exe`, `lua51.dll`). The sources of `lj_profile.c`, `lj_record.c`, `lib_jit.c`, `lj_dispatch.c`, `lj_api.c` and `lj_debug.c` are byte-identical to the repo's `vendor/luajit` at 19878ec0 (checked with `cmp`).
* JDK: 21.0.13 Temurin. P-core pinning was done with the investigator's `pin.exe C03C03` for the timing runs only.

## Verdict summary

| id | verdict | severity (mine) | one-line |
|---|---|---|---|
| PB-3 | partially confirmed | high | All the individual observations reproduce. "Every simple Lua-level abort fails" is **refuted**: pre-arming a count hook makes a pure-Lua abort from the 'l' profiler callback work reliably with the JIT on. A LuaJIT patch is still needed for a *multi-computer* server, for different reasons (process-global profiler). |
| PB-4 | confirmed | critical | Reproduced exactly with an independent harness (+32/60/97/123/160 MB; 578-594 ms pauses). With Aikar's G1 server flags and background allocation, **a single young pause took 3.9 s**. Generational ZGC avoids the pauses but not the memory growth. |
| PB-5 | partially confirmed | medium | The ns/call figures reproduce within about 10%. The "3.5-10x" ratio is overstated at the low end: String-typed calls (term.write-like, string results) are 1.4-3.2x, nop is 3-4x. A lean bridge's string *results* are faster than Cobalt. Sub-microsecond, so not "high". |
| PB-6 | confirmed | high | Independent probe gives the same bytes: 256 -> 513 bytes, 0 equal positions, `C0 80` for NUL, `EF BF BD` for 0x80+, and byte[256] starting with 0 -> 0-byte string. |

Missed issues (details in section 6):
* (M1) **CCLuaJIT never enables the JIT at all.** It does not call `luaopen_jit` on any branch, so it runs at exactly `-joff` speed.
* (M2) **LuaJIT's C pattern matcher cannot be preempted.** A 300-character `find` ran 54 s and ignored a 0.2 s count-hook deadline. Cobalt checks interrupts inside `StringMatch`.
* (M3) `jit.profile` is process-global:
  * One VM at a time.
  * Starting or stopping it flushes all traces. On Windows, stopping it also blocks for about 12 ms.
  * On POSIX it uses SIGPROF/ITIMER_PROF for the whole process.
* (M4) An error in a `jit.profile` Lua callback calls `exit(EXIT_FAILURE)`, which would kill the server JVM.
* (M5) The local-ref leak also grows with table size inside a single call.

---

## 1. PB-3: preemption with the JIT on

### 1.1 Count hooks do not fire in traces: confirmed
`$V/preempt/e1_hookcount.lua` counts how often a count hook (every 1000 instructions) is invoked during a fixed 2e8-iteration loop:
```
JIT:   hook calls=0      (expected ~800000 if every 1000 instrs) t=0.249
-joff: hook calls=600000                                         t=0.773
```
Zero invocations with the JIT on is direct evidence, stronger than the investigator's timing argument. The JIT and JIT+hook(1e5) timings in my overhead run below also match (geomean 0.995).

### 1.2 Profiler modes: confirmed
`$V/preempt/prof.lua <mode> <variant> [prearm]`, 4 s timeout. Log: `$V/logs/prof_r1.log`.

| run | result |
|---|---|
| `li10`, callback calls `os.exit(3)` after 0.3 s | callback ran after 29 calls, **vmstate=N** (inside compiled code), exit=3 |
| `fi10` / `i10`, JIT on | exit=124 (timeout, callback never ran) |
| `i10`, `-joff` | callback ran (vmstate=I) |
| `li10` + `error()` in callback | `PANIC: unprotected error in call to Lua API (prof.lua:16: err-in-callback)`, exit=1 |
| `li10` + `debug.sethook(abort,"",1)` in callback | PANIC (TLWY-abort), exit=1 |
| `li10` + `debug.sethook(abort,"",20)` in callback, JIT and `-joff` | exit=124 (hook silently dropped) |
| `li10` + `prof.stop(); debug.sethook(abort,"",20)` | **Segmentation fault**, exit=139 |
| `li10` + `prof.stop(); debug.sethook(abort,"",1)` | PANIC |

Mechanism, from source (`vendor/luajit/src`):
* `lj_profile.c:126-146` `lj_profile_interpreter`:
  * Saves `mask = g->hookmask & ~HOOK_PROFILE` (line 132).
  * Sets `g->hookmask = HOOK_VMEVENT` during the callback (136) and calls `ps->cb` (139).
  * Then **restores** `g->hookmask = mask` (143).
  * So hook *mask bits* set inside the callback are discarded.
* `lj_dispatch.c:329-340` `lua_sethook`: sets `g->hookf` and `g->hookcount` and ORs the event bits into `hookmask`. Only the mask bits get reverted.
* Correction to the investigator's explanation of the PANIC: the callback **is** run under `lua_pcall`, in the profiler's own coroutine L2. On error, `lib_jit.c:546-550` deliberately calls `G(L2)->panic(L2)` and then **`exit(EXIT_FAILURE)`**. Inside a JVM that would terminate the whole server, which is worse than a mere "unprotected error". See M4.

### 1.3 Refutation: a simple Lua-level abort that works with the JIT on
Because only the mask bits are restored, pre-arming a count hook *before* the profiler runs keeps `LUA_MASKCOUNT` in the saved mask. Re-arming the hook inside the callback then changes `hookf`/`hookcount`, and those changes survive.

`$V/preempt/prof.lua li10 hook20 prearm` (pre-arm `debug.sethook(noop,"",1e9)`; the callback calls `debug.sethook(abort,"",20)`):
```
pcall ->  false  TLWY-abort  callbacks: 34  elapsed 0.37s      (JIT on)
pcall ->  false  TLWY-abort  callbacks: 29  elapsed 0.31s      (-joff)
```

A robust variant (`$V/preempt/prof2.lua`, log `$V/logs/prof2_r1.log`):
* A count hook at 100000 is pre-armed. It ignores the profiler's own coroutine and errors once a deadline has passed.
* The profile callback only re-arms it with count 1 after the deadline.
* The user code runs in a coroutine, as in CC.

Every workload was aborted cleanly. `resume` returned `false, "Too long without yielding"` and the coroutine was dead, within about 10 ms of the deadline:
```
spin  (while true do x=x+1 end)          abort after 0.313s
multi (2-line arithmetic loop)           abort after 0.313s
calls (Lua function call in loop)        abort after 0.311s
strings (rep/concat/table store)         abort after 0.315s
nested (for-loop inside while)           abort after 0.311s
-joff spin                               abort after 0.305s
li1 spin                                 abort after 0.301s
i10 spin                                 timeout (exit 124)  <- 'l' mode is required
```

**Cost** (`$V/profcost/run.sh`, all 10 benchmarks, the investigator's sizes, 3 rounds, QPC clock via FFI, median of iterations 2-7; `$V/logs/profcost.log`, summary in `$V/logs/profcost_summary.txt`):

| bench | jit | li10 | li10 + pre-armed hook | hook only |
|---|---|---|---|---|
| fib | 0.0376 | 0.0377 | 0.0376 | 0.0376 |
| mandel | 0.0173 | 0.0167 | 0.0165 | 0.0165 |
| tables | 0.2350 | 0.2659 (+13%) | 0.2674 | 0.2340 |
| oop | 0.0431 | 0.0458 | 0.0458 | 0.0436 |
| strings | 0.0716 | 0.0710 | 0.0707 | 0.0718 |
| sha256 | 0.0120 | 0.0123 | 0.0124 | 0.0121 |
| framebuf | 0.1886 | 0.1863 | 0.1858 | 0.1891 |
| serialize | 0.1845 | 0.1757 | 0.1737 | 0.1796 |
| lzw | 0.1671 | 0.1894 (+13%) | 0.1834 | 0.1648 |
| compile | 0.2149 | 0.2154 | 0.2174 | 0.2189 |
| **geomean vs jit** | 1.000 | **1.024** | **1.019** | 0.995 |

* "Essentially free" is confirmed: about 2% geomean, and at most +13% (tables, lzw).
* The investigator's "lzw dropped from 3.5x to 2.5x" (+43%) did not reproduce. It came from a single round, and I see +10-13%.
* The investigator's geomean 9.1x vs 8.9x (profiling *faster* than plain JIT) is also noise. The real overhead is small but positive.

### 1.4 But a production design still needs C/LuaJIT work (see M3)
The pure-Lua trick works for **one** VM per process. CC needs one VM per computer.

### PB-3 corrected claim
* Count hooks never fire inside traces.
* `jit.profile` mode `'l'` compiles IR_PROF guards and regains control from JIT-compiled loops at about 2% geomean cost (max +13%). `'f'` and `'i10'` do not regain control.
* `error()` in the callback, or a count-1 hook armed in the callback, leads to panic plus `exit()`. Arming a count hook from scratch in the callback is silently dropped (the mask is restored at `lj_profile.c:143`). `prof.stop()` + sethook in the callback segfaulted on Windows.
* **However**, if a count hook is pre-armed, re-arming it with a small count in the callback aborts the running code cleanly (verified on 5 workload shapes, JIT on and off).
* A multi-computer server still needs a LuaJIT patch, because the profiler is process-global and single-VM.

---

## 2. PB-4: JNI local-reference leak

### 2.1 Source re-read
`main.cpp` on 1.20.1_CCT:
* `to_java_values` (lines 365-371): `NewObjectArray` plus one `to_java_value` per argument. Each is a local ref and is never deleted.
* `to_java_value` (lines 335-351):
  * Number: `CallStaticObjectMethod(Double.valueOf)`.
  * String: `NewByteArray`, then the `decodeString` result. Two refs, neither deleted.
* `invoke_java_fn` (lines 384-466): `results` from `CallObjectMethod` is never deleted. Only `GetObjectArrayElement` elements are deleted (`to_lua_values`, line 332).
* `handleEvent` (lines 798-821) runs `lua_resume` inside the JNI native method. Every Lua -> Java call made before the coroutine yields or returns therefore shares that native frame's local-ref table.
* Bridge code is identical on `origin/1.14.4_CCT` (diffed; only the `IDynamicLuaObject`/`ILuaObject` branch differs).

### 2.2 Independent reproduction
`$V/vjni/src/vjni.cpp` + `VJ.java` + `leak.lua`:
* The `invoke_java_fn`/`to_java_value(s)`/`to_lua_value(s)` bodies are copied from main.cpp; only the table branches are dropped.
* The Java callee echoes its argument (`return new Object[]{args[0]}`).
* 5M calls without yielding; `-Xmx2g`, G1. Log: `$V/logs/leak_r1.log`.

```
leak 1M calls: private +32 MB   | GC(0) Pause Young 102M->99M  593.6 ms
leak 2M calls: private +60 MB   | GC(1) Pause Young 188M->198M 578.1 ms
leak 3M calls: private +97 MB   | GC(2) Pause Young 287M->295M 588.1 ms
leak 4M calls: private +123 MB
leak 5M calls: private +160 MB  (3.34 s total)
fixed (Push/PopLocalFrame): +0/+1 MB; pauses 102M->0M 2.4 ms, 116M->0M 1.5 ms; 1.32 s total
```
The private-memory growth matches the investigator's numbers MB for MB. That is 3 handles per call (args array, `Double`, results array), at about 10 bytes per JNIHandleBlock slot.

### 2.3 Server-like GC settings (my addition)
Log: `$V/logs/leak_server_r1.log`, `$V/logs/leak_server_r2.log`.
* **Aikar's G1 flags, 4 GB, no other allocation, 10M calls:** no GC happened during the run, because the 1.2 GB young gen did not fill. The memory still grew to +294 MB.
* **Aikar's G1 flags, 4 GB, plus a background thread allocating 300 MB/s** (to stand in for Minecraft's own allocation):
  ```
  GC(0) Pause Young (Normal) (G1 Evacuation Pause) 1224M->680M(4096M) 3936.122ms
  ```
  That is a **3.9 s stop-the-world pause, about 79 ticks**. With `SurvivorRatio=32` and `MaxTenuringThreshold=1`, the roughly 600 MB of JNI-pinned objects are evacuated into old gen, which also pollutes old gen afterwards. The fixed variant under the same flags paused 2.4 ms (1224M->0M).
* **Generational ZGC, 4 GB, 10M calls:** all safepoints were below 0.65 ms, but memory still grew to +409 MB.

### PB-4 corrected claim
* Confirmed.
* The worst case is worse than reported: with common server G1 flags and normal background allocation, a single pause reached 3.9 s.
* The pauses depend on the collector (ZGC avoids them); the native and heap growth does not.
* The bug is reachable only once Java APIs are registered again. On 1.20.1_CCT `registerAPI` is commented out, so this is a must-fix for the port's bridge.

---

## 3. PB-5: host-call cost

### 3.1 Independent JNI measurement
`$V/vjni/src/perf.lua`: 10M calls, median of 3, pinned to P-cores. Log: `$V/logs/perf_r1.log`.

| path | mine, JIT / -joff | investigator, JIT / -joff |
|---|---|---|
| C nop (lua_CFunction) | 10.6 / 4.5 | 10.4 / 3.9 |
| lean JNI nop | 29.4 / 22.2 | 30.5 / 22.6 |
| lean JNI num | 52.9 / 49.0 | 53.2 / 45.6 |
| boxed nop (faithful + frame) | 105.4 / 101.9 | 102.5 / 116.1 |
| boxed num | 279.3 / 262.9 | 261.9 / 275.1 |
| boxed str out (100 chars) | 316.7 / 297.2 | 310.5 / 304.0 |
| boxed echo(100 B string), in **and** out | 482 / 430 | (not measured) |

### 3.2 Cobalt numbers, re-run of the investigator's `CobaltHost`
This runs the real `CobaltLuaMachine` with an `ILuaAPI`, 2 runs. Log: `$V/logs/cobalt_host_r1.log`.

| path | ns/call |
|---|---|
| raw LibFunction nop / num / strin / strout | 23.7-25.1 / 20.7-22.4 / 23.5-23.8 / 24.3-26.2 |
| Lua -> Lua call | 47.6-55.5 |
| CC:T nop | 27.3-29.6 |
| CC:T num | 34.3-44.2 |
| CC:T `Coerced<String>` in (= term.write) | 70.8-80.6 |
| CC:T `ByteBuffer` in | 31.2-41.8 |
| CC:T String out | 138.6-145.3 |
| CC:T blit3 (3x ByteBuffer = term.blit) | 46.0-49.8 |

These are slightly lower than the investigator's ranges, so the Cobalt path is if anything cheaper.

I checked that the shapes are right. In CC:T 1.121.0 `TermMethods.java`, `write(Coerced<String>)` is at line 39 and `blit(ByteBuffer, ByteBuffer, ByteBuffer)` at line 254.

### 3.3 Ratios (boxed bridge vs Cobalt, like for like)

| call | boxed vs Cobalt |
|---|---|
| nop | 2.8-3.9x |
| number | 4.8-8.2x |
| String in (term.write shape), boxed string-in 227.5 vs 71-153 | 1.5-3.2x |
| String out, 310-317 vs 139-220 | 1.4-2.3x |
| term.blit, 489 vs 46-64 | 7.6-10.6x |

So the true range is **1.4-10.6x**, not 3.5-10x. The investigator's own TL;DR said "3x to 10x".

Lean bridge (investigator's lean string numbers; my lean nop/num reproduce):

| call | lean vs Cobalt |
|---|---|
| nop / num | parity (29-53 vs 27-54) |
| String in via `NewString` (187.6) vs term.write | 1.2-2.6x |
| byte[] in (112.7) vs ByteBuffer in | 1.9-3.6x. The lean byte[] callee only reads `b.length`, so this is a lower bound. |
| blit3 (222) | 3.5-4.8x |
| String out (123) / byte[] out (90) vs Cobalt String out 139-220 | **faster than Cobalt** |

On "most of the boxed cost is per-value upcalls": for nop (zero values), boxed costs about 75 ns more than lean. That is fixed overhead (GetEnv, empty `Object[]`, Push/PopLocalFrame, virtual call, field read), not per-value upcalls. The per-value upcalls dominate only for the number and string calls (+160-175 ns per boxed number).

### PB-5 corrected claim
* The magnitudes are right: boxed bridge about 100-320 ns per call; lean bridge 30-53 ns for nop/number.
* The ratios vs Cobalt are 1.4-10.6x, depending on argument type. It is lowest for String-typed calls, about 3-4x for nop, and highest for ByteBuffer-typed calls such as term.blit.
* A lean bridge is at parity for nop/number, 1.2-4.8x for string arguments, and faster than Cobalt for string results.
* Severity is **medium**, not high. All of these are below 1 µs, so they matter only for call-bound loops, and the port has to rewrite the bridge anyway for `IArguments`/`MethodResult`.

---

## 4. PB-6: strings are not 8-bit clean (confirmed)
`$V/vjni/src/probe.lua` goes through my faithful copy, with Java `decodeString` = `new String(bytes, US_ASCII)`. That is the same on 1.20.1_CCT (`LuaJITMachine.java:13-14`) and on 1.14.4. Log: `$V/logs/probe_r1.log`.
```
echo bytes 0..255: sent 256 bytes, got 513 bytes, positions equal: 0
  first 8 bytes back: C0 80 01 02 03 04 05 06 | bytes 0x80.. come back as: EF BF BD EF BF BD
Java byte[256] {0,1,..,255} -> Lua string of 0 bytes
echo 'abc\0def' (7 bytes) -> 8 bytes: 61 62 63 C0 80 64 65 66
echo '\128' (1 byte) -> 3 bytes: EF BF BD
```

Direction detail:
* Lua -> Java maps each byte >= 0x80 to U+FFFD. That is lossy, but it keeps the length in chars, so `term.blit` length checks still pass and the wrong glyphs are drawn.
* Java String -> Lua is modified UTF-8 via `GetStringUTFChars` + `lua_pushstring` (`main.cpp:270-274`).
* Java `byte[]` -> Lua is cut off at the first NUL by `lua_pushstring` (`main.cpp:275-285`).

Real impact in the 1.21.1 ROM:
* `cc/internal/error_printer.lua:161,166,169` itself calls `term.blit("\x95"...)` and `("\x83"):rep(...)`.
* `io.lua:401`, `wget.lua:86`, `cc/internal/import.lua:57` and `cc/audio/dfpwm.lua:43` use binary `"wb"` handles.

Mentioning `string.pack` is a stretch: LuaJIT 5.1 has no `string.pack`, and packed data is affected only when it is passed to an API. The rest of the impact list stands.

---

## 5. Things I could not verify independently
* The PB-5 "port" variant (`LuaMethod` + `ObjectArguments`, +15-20 ns). I did not rebuild it.
* The FFI 0.4 ns figure.
* The lean string-in/out numbers. My lean nop/num reproduce, so I take them as plausible.

---

## 6. Missed issues in this lane

### M1. CCLuaJIT never turns the JIT compiler on (high; decisive for "does it deliver the perf benefit")
* In LuaJIT 2.1, `JIT_F_ON` and the default JIT parameters are set only by `jit_init()`, which `luaopen_jit` calls (`lib_jit.c:712-718`). `luaL_newstate` leaves the JIT off. `JIT_F_ON` is set in no other place apart from `luaJIT_setmode` (`lj_dispatch.c:262`), and even that does not initialise `J->param`.
* `initMachine` (`main.cpp:704-714` on 1.20.1_CCT; same on 1.14.4 at 701-705, and on 1.12.2/1.7.10 at 692-696) opens only base, math, string, table and bit. It never calls `luaopen_jit`, `luaL_openlibs` or `luaJIT_setmode`.

Empirical check: `$V/jitflag/jitflag.c` creates the state exactly as `initMachine` does. Log: `$V/logs/jitflag_r1.log`, seconds per iteration.

| bench | CCLuaJIT-style state | + `luaopen_jit` via `lua_call` | luajit.exe `-joff` | luajit.exe JIT |
|---|---|---|---|---|
| fib(32) | 0.064 | 0.015 | 0.064 | 0.014 |
| mandel (300 it) | 0.053 | 0.0087 | 0.053 | 0.008 |
| oop (4000) | 0.10-0.107 | 0.022 | 0.11 | 0.022 |

So CCLuaJIT as written delivers only interpreter speed, about 3.7x over Cobalt (investigator's `-joff` geomean), not 8.9x.

The port must open the jit library through `lua_call` and then remove the `jit` global from the sandbox. Doing so is also exactly what introduces the preemption problem: with the JIT off, CCLuaJIT's count-hook `abortMachine` does fire.

Side observation: in that state, `sha256` failed with "attempt to call upvalue 'rrotate' (a nil value)", because `bit32 = bit` has no `rrotate`.

### M2. Long C library calls cannot be preempted (critical: computer-thread / server hang)
* `$V/preempt/cfunc.lua` uses `-joff` with a count hook every 100 instructions that errors after 0.2 s. It then calls `string.find(("a"):rep(n), ".-.-.-.-b")`.
  ```
  n=200: returned after 7.80 s (deadline 0.2 s)
  n=300: returned after 54.20 s
  n=400: still running at 60 s (timeout)
  ```
  Log: `$V/logs/cfunc_r1.log`.
* LuaJIT's matcher (`lib_string.c`) runs in C with no hook checks. Neither count hooks nor IR_PROF guards can reach it.
* Cobalt checks `state.isInterrupted()` inside `StringMatch` (`cobalt-src/org/squiddev/cobalt/lib/StringMatch.java:513,528`) and in the compiler (`LuaC.java:205`), so CC:T's soft and hard aborts work there.
* With CC:T's default `computer_threads = 1`, one player's one-liner stalls every computer on the server, past the 7 s + 1.5 s abort.
* The port needs an interruptible matcher, for example a patched `lib_string.c` match loop with a step budget or an abort-flag check. Other long C paths need checking too (`load` of huge sources, `string.rep`, `table.concat`, gsub).

### M3. `jit.profile` cannot serve several computers (high, design input for PB-3)
* `static ProfileState profile_state` (`lj_profile.c:90`): one profiler per process. `luaJIT_profile_start` returns silently with "Profiler in use by another VM" (`lj_profile.c:326-329`).
  * Verified: `$V/multivm/multivm.c` creates two `lua_State`s. VM1 got **0** callbacks during 0.3 s of a JIT-compiled loop while VM0 held the profiler, and 28 after VM0 released it. Log: `$V/logs/multivm_r1.log`.
* Starting and stopping the profiler per time slice flushes all traces (`lj_profile.c:316-320`, `:345-352`).
  * On Windows, `profile_timer_stop` also waits for the timer thread. With 500 slices: 6.97 s with start/stop per slice, vs 0.64 s baseline and 0.69 s with only `jit.flush()` per slice. That is about 12 ms per slice, which is not workable.
* On Linux and macOS the profiler is `SIGPROF` + `setitimer(ITIMER_PROF)` (`lj_profile.c:170-205`, `lj_arch.h:597-599`):
  * The timer is process-wide and counts the CPU time of all JVM threads.
  * The signal can arrive on any thread, so `profile_trigger` races with the Lua thread.
  * It clashes with other SIGPROF users in the JVM, such as async-profiler-based server profilers.
* A practical port needs a small LuaJIT patch. For example: let each VM keep `prof_mode='l'` guards, and add an exported per-`global_State` "request interrupt" function that sets `HOOK_PROFILE` and calls a per-VM C callback. CC:T's `TimeoutState` thread would call that function.

### M4. A Lua error in a `jit.profile` callback calls `exit()` (medium)
`lib_jit.c:546-550`: `if (status) { if (G(L2)->panic) G(L2)->panic(L2); exit(EXIT_FAILURE); }`.
* Inside Minecraft this terminates the server process.
* CCLuaJIT does not expose `jit` to user code, so this matters only if the port uses a Lua-level profile callback or exposes `jit`. Any such callback must never throw; a C callback or a patch is safer.

### M5. The leak also scales with table size (low/medium, extends PB-4)
* `map_to_table` (`main.cpp:~196-215`) creates `entrySet`, `iterator`, and per entry `next`/`key`/`value`, plus `Integer.valueOf`.
* `table_to_map` creates boxed key/value refs per entry.
* None of these are deleted. A single call that passes or returns a large table (fs.list, inventory list, http headers) therefore adds 3-5 refs per entry, on top of the per-call refs.
* A per-call Push/PopLocalFrame bounds this per call. Per-entry deletion (or a frame per entry) is still advisable for big tables.

---

## 7. Files
* Preemption:
  * `$V/preempt/e1_hookcount.lua`, `prof.lua`, `prof2.lua`, `cfunc.lua`
  * `$V/multivm/multivm.c`, `slices.lua`, `slices2.lua`
  * Overhead: `$V/profcost/run.sh`, `clock.lua`, `summ.py`
* JIT flag: `$V/jitflag/jitflag.c`, `status.lua`
* JNI harness (`$V/vjni`):
  * Sources: `src/vjni.cpp`, `src/VJ.java`, `src/leak.lua`, `src/perf.lua`, `src/probe.lua`
  * Build: `g++ -O2 -shared -o bin/vjni.dll -I<luajit/src> -I"$JDK/include" -I"$JDK/include/win32" src/vjni.cpp bin/lua51.dll -lpsapi -Wl,--kill-at -static-libgcc -static-libstdc++` and `javac -d classes src/VJ.java`
  * Run: `java [-Xlog:gc] [-Dalloc.mbps=300] -cp classes VJ <bin-dir> <script.lua> "<lua prelude>"`
* Logs: `$V/logs/` (`prof_r1`, `prof2_r1`, `profcost`, `profcost_summary.txt`, `jitflag_r1`, `leak_r1`, `leak_server_r1`, `leak_server_r2`, `perf_r1`, `cobalt_host_r1`, `probe_r1`, `multivm_r1`, `cfunc_r1`)
