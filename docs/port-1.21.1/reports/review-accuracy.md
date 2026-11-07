# Plan review: factual accuracy (PLAN.md v1)

*Written on 2026-11-07 by an independent review/prototype agent. Section references (§, P-numbers, WP letters) point to PLAN.md **v1**; PLAN.md v2 has incorporated or answered every item (see its change log). Paths under `scratchpad/work/...` refer to the session scratch workspace; the reusable files were copied into `../artifacts/`.*

## Verdict

Most of PLAN.md matches the reports and the corrections from the verification reports. Several places already use the corrected figures: 14 `_ENV` files, the 5,000-deep crash threshold, the 1.4–10.6x boxed-bridge range, the +2–5% allocator wrapper, the 9–25% fuzz crash rate, the 0.24–3.9 s GC pauses, and the "assert came in 505b1f9" history. The arithmetic in the estimate table also checks out (26–43 and 61–103 person-days). I re-ran the preemption harness on the combined build and got 25/25 at 0.6–16.9 µs. I also applied patches 01–06 to `3c4f9fe2`, built it, and got 25/25 at 0.7–19 µs with `compat_check.lua` passing. Six factual problems remain, one of which matters for the decision. (1) The headline speedup (8.9x, SHA-256 55x) was measured with LuaJIT's signed `bit` standing in for `bit32`, which §1.2 itself calls wrong. With correct semantics the geomean is about 8.1x. With the pure-Lua `bit32` that actually ships in `cclj_prebios.lua`, SHA-256 is only about 1.5x Cobalt (I measured this). So C `bit32` is a prerequisite, not an optimisation. (2) The JVM-killer list includes a cyclic Java map, which both lanes show is not fatal. It also blames all the crashes on unprotected Lua errors. (3) "180 commits newer" is the distance from the vendored copy to HEAD, not to the pin; the pin is 127 commits newer. (4) "~0% overhead" holds only for patch 01 in JIT code. It leaves out the +23% interpreter cost of the count hook that the protocol requires to stay installed, and the cost of patch 05: `tostring(float)` is 2.6x slower and the CC strings benchmark is +22–25% (measured). (5) The TL;DR says two items are unprototyped, but the §3 table lists five or more design-only items. (6) The native-loader description contradicts the tested NativeLoader in the scaffold (different path; it does delete stale versions). The remaining items are small misattributions.

## 1. [must-fix] TL;DR row 2; §2 table row 'LuaJIT, JIT on'; §2 'Where players notice it'; §3 P11

**Problem.** The headline 8.9x geomean and 'SHA-256 55x' were measured with bit32 = LuaJIT's signed `bit`. That is the configuration §1.2 calls semantically wrong, and the plan replaces it. With a correct unsigned bit32, SHA-256 is 22x and the geomean is about 8.1x. The '~20 MB/s with correct bit32' figure came from a fast 2/3-argument wrapper written for the benchmark. It does not come from the vararg pure-Lua bit32 in artifacts/lua/cclj_prebios.lua, which the plan ships, and which reaches only about 1.3 MB/s (about 1.5x Cobalt). P11 presents C bit32 as an optional speed-up, but the measured numbers depend on it.

**Evidence.** artifacts/bench/lua/compat.lua: BIT32_MODE default 'signed' ('mirrors what CCLuaJIT 1.20.1 does: bit32 = bit'). artifacts/bench/results-summary.md: 'sha256 ... correct unsigned bit32 shim: 0.033 s vs 0.013 s with bit32=bit'. Recomputed geomean with sha256 = 0.737/0.033 = 22.3x gives 8.10x (python over the summary's per-benchmark times). verify-rom-compat RC-9: the shim's bit32 is '2.2-2.4x SLOWER' than Cobalt for full 32-bit values (trace aborts: 'loop unroll limit reached'). My run on the combined build (scratchpad/work/main-combined/src/luajit.exe, b06_sha256, 160x4 KiB): signed bit 0.012 s, bench unsigned wrapper 0.029 s, dofile(cclj_prebios.lua) then bench 0.50 s per run (about 1.3 MB/s, against Cobalt warm 0.737 s). The JIT was on (-joff gives 1.57 s). Files: <scratch>\work\review-accuracy\b\ (sha.lua, pre.lua).

**Suggested text.**

TL;DR row 2: "**Yes, for CPU-bound Lua: about 8x faster than CC:T's Cobalt** (geometric mean over 10 CC-style benchmarks, range 2.3x–29x, measured against Cobalt running inside CC:T 1.121.0's real machine, with correct unsigned `bit32`. The 8.9x / 'SHA-256 55x' figures used LuaJIT's signed `bit` ops, which give wrong results for CC programs.) This assumes `bit32` is implemented in C: with the current pure-Lua shim, SHA-256 is only ~1.5x Cobalt. With the JIT off it is 3.7x. ..."

§2 table row: "| LuaJIT, JIT on | **8.1x** with correct unsigned `bit32` (fib 26x, mandelbrot 29x, OOP 18x, SHA-256 22x; tables/strings/serialize/LZW 3.5–6x; compiling Lua 2.3x). 8.9x if SHA-256 uses the signed `bit32 = bit` alias (55x), which is not CC-compatible. |"

§2 bullet: "- Programs that compute: SHA-256 goes from ~0.9 MB/s on Cobalt to ~20 MB/s with a fast, correct `bit32` (C, or 2-argument Lua fast paths). With the vararg pure-Lua `bit32` in `artifacts/lua/cclj_prebios.lua` it reaches only ~1.3 MB/s (~1.5x Cobalt), because its loops abort JIT traces."

§3 P11, last sentence: "Move `bit32` to C (required for the performance case: the Lua shim is slower than Cobalt on full 32-bit values) and `string.pack` to C (the Lua shim is ~5x slower than Cobalt)."

## 2. [should-fix] §1.2 'JVM-killing triggers' bullet and TL;DR row 1

**Problem.** The list of '7 reproduced' JVM-killing triggers includes 'a cyclic Java map'. Both the investigator's own results table and the verifier show that it does not kill the JVM. The list leaves out two triggers the verifier showed are fatal (a HashMap with a null key, and one with a NaN key). The sentence 'The cause is Lua errors raised outside any protected call' misattributes three of the triggers. The deep-table and cyclic-table crashes are C-stack overflows in the recursive converters, and close-while-running is a cross-thread use-after-free. The TL;DR also credits all 7 to 1.14.4 'for any player'. But close-while-running is a hazard for a naive 1.21.1 port: 1.14.4's coremod rerouted the monitor's close() to abort(). Abort-after-close was only shown by calling the native functions directly in a harness.

**Evidence.** code-audit.md §2.1 table: cyclic-map → 'StackOverflowError thrown out of the native code' (no fatal error). verify-code-audit CA-03: 'A cyclic Java Map (Java→Lua) gave only a StackOverflowError in my runs'. CA-01: 'New triggers I verified: a HashMap null key and a NaN key' (fatal 0xe24c4a02 / PANIC). CA-02: 5,000 levels gives 'An unrecoverable stack overflow has occurred'. CA-04: close-while-running gives 0xC00000FD/ACCESS_VIOLATION, and 'Nuance: 1.14.4's coremod rewired the monitor's close() to abort(), so 1.14.4 mostly avoided cross-thread close'. CA-01: a single-value yield(print) does not crash the unmodified code; (print, print) does.

**Suggested text.**

§1.2 bullet: "- **JVM-killing triggers (reproduced in harnesses on the unmodified native code):**
  - an unknown Java type in an event;
  - a Java map with a `null` or NaN key;
  - a cyclic Lua table passed once to any API (e.g. `os.queueEvent`) or yielded;
  - a ~5,000-deep nested table;
  - a function yielded to the top level (because of the old off-by-one, two yielded values are needed);
  - `close()` while running (use-after-free; CC:T 1.21.1 calls `close()` from its monitor thread, whereas 1.14.4's coremod rerouted that call to `abort()`);
  - abort after close.

  There are three causes: Lua errors raised outside any protected call (on Windows a fatal SEH `0xE24C4A02`; on Linux `PANIC` and `exit(1)`, which skips Java shutdown hooks); unbounded native recursion in the converters (a C-stack overflow at ~5,000 levels, also hit by cyclic tables); and the cross-thread use-after-free. A cyclic *Java* map only throws a `StackOverflowError`: the JVM survives, but JNI calls are then made with the exception pending."

TL;DR row 1, last sentence: "... deadlocked on every main-thread peripheral task, and its native code has at least 7 reproduced JVM-killing triggers, several of them reachable from ordinary player code (e.g. a cyclic or ~5,000-deep table passed to any API)."

## 3. [should-fix] §4.4 'Native layer' bullet 'Pin LuaJIT to 3c4f9fe2'

**Problem.** 'It is 180 commits newer than the vendored copy' is wrong. 180 is the number of commits from the vendored 19878ec0 to the current HEAD ea9eb7ed. The pin 3c4f9fe2 is 127 commits (100 non-merge) newer than the vendored copy and 53 commits behind HEAD. The plan does not say this, but the patch README calls 3c4f9fe2 'the last v2.1 commit before' the syntax backport; 14d8a7a2 (2026-07-18) actually sits between them.

**Evidence.** git -C vendor/luajit rev-list --count 19878ec0..3c4f9fe2 = 127 (--no-merges 100); 19878ec0..ea9eb7ed = 180 (146 non-merge); 3c4f9fe2..ea9eb7ed = 53. git log --first-parent ea9eb7ed: ... a2ce8114 2026-07-20 'Backport some v3.0 syntax extensions.', 14d8a7a2 2026-07-18 'x64/LJ_GC64: Avoid store-to-load forwarding stall after stack restore.', 3c4f9fe2 2026-07-11 'Merge branch master into v2.1'.

**Suggested text.**

"- **Pin LuaJIT to `3c4f9fe2`** (2026-07-11). It is 127 commits (100 non-merge) newer than the vendored `19878ec0` and 53 commits behind the current HEAD `ea9eb7ed` (2026-10-30), which is 180 commits past the vendored copy. It predates upstream's v3.0-syntax backport (`a2ce8114`, 2026-07-20), which would let CCLuaJIT programs use syntax that normal CC:T rejects. The last v2.1 commit before the backport is actually `14d8a7a2` (2026-07-18, an x64 GC64 performance fix), which could serve as the pin instead."

## 4. [should-fix] TL;DR row 4; §2 table row 'JIT on + preemption patch'; §3 P1

**Problem.** '~0% overhead' / '−0.1% geomean, worst +1.5%' are placed in the 10-CC-benchmark table, but they were measured for patch 01 alone, against stock LuaJIT, on the preempt lane's own 8 benchmarks, using pass 2 only (pass 1 gave +2.3% / +1.9%). Three costs are left out. (a) The interrupt protocol requires a count hook that stays installed, and that hook costs +22–24% geomean (+1% to +65%) on interpreted code. CC:T's coroutine plumbing is such code. (b) The full recommended stack has never been measured on the CC benchmarks. I measured it: +1.2% geomean overall, but 'strings' is +22–25% because patch 05 makes tostring(float) ~2.6x slower. (c) The JIT-code figure holds; the interpreter figure needs stating.

**Evidence.** preempt-patch.md §5: pass 2 (9 runs) −0.1% / −0.05%, pass 1 +2.3% / +1.9%; the benchmarks were fib, mandelbrot, nbody, spectralnorm, tableops, stringops, sha256, methods; interpreter + count hook 1e4 = '+22.4% / +23.6% / +23.1%' geomean; 'CC:T's event loop and coroutine plumbing are such paths, since coroutine.* is not compiled'. My run (2 rounds x 6 iterations, medians of iterations 2+, unpinned) of stock scratchpad/luajit-win against main-combined on the 10 CC benchmarks: geomean comb/stock 1.012; strings 0.072 → 0.088 s (re-run 3x: 0.070 → 0.088–0.090; with hook 1e4 0.087–0.095). Microbenchmark: tostring(i/7) x1e6 0.075 → 0.195 s; integer tostring and patterns unchanged. Files: ...\work\review-accuracy\b10\out.txt, micro.lua.

**Suggested text.**

§2 row: "| LuaJIT, JIT on + full patch stack 01–06 | ≈ same in JIT code: +1% geomean on these 10 benchmarks, but `strings` is +22–25% because patch 05 makes `tostring(float)` ~2.6x slower (round-trip `%.17g` check, like Cobalt). Patch 01 alone, on its own 8-benchmark suite: −0.1% / +2.3% geomean in two passes, worst +1.5%. |"
Add under the table: "The interrupt protocol needs a count hook that stays installed. That hook costs nothing in JIT-compiled code but +22–24% geomean (+1% to +65%) in interpreted code (cold code, NYI paths, `coroutine.*`); this is the '−joff + hook' penalty."
TL;DR row 4 / P1: "... 25/25 loop shapes stopped within microseconds, with no measurable cost in JIT-compiled code; interpreted code pays ~+23% for the count hook that must stay installed."

## 5. [should-fix] TL;DR row 4 ('Two items remain designed but unprototyped'); §3 P12

**Problem.** The TL;DR says only pause and interruptible pattern matching remain unprototyped. The plan's own §3 table marks P4 (cross-thread close protocol), P5 (protected boundary), P7 (curated debug library and fake registry) and P8 (nesting limit, Lua gsub, patch 08, which is unwritten) as 'Design'. This understates the untested surface, in exactly the WP C/D area §5 calls the biggest uncertainty. In the other direction, P12 is listed as 'Design', but the trampoline was prototyped in verify-cct-contract.

**Evidence.** PLAN §3: P4 'Design: close() only sets flags...', P5 'Design rule', P7 'Design: curated debug library', P8 'Design: coroutine-nesting limit ...; patch 08 ... as a backstop' (patches README: '08 C-stack guard ... Still to write'), P12 'Design'. verify-cct-contract CT-4: 'A Lua trampoline (native returns a marker + handle; Lua yields and calls native_resume) handled a callback that yielded 3 times and finished'.

**Suggested text.**

TL;DR row 4, last sentence: "Several pieces are still designs only: transparent pause (P10), interruptible C pattern matching (patch 07), the C-stack guard and coroutine-nesting limit (P8, patch 08), the curated `debug` library and fake registry (P7), and the cross-thread `close()` protocol (P4). For P4 only the failure has been reproduced, not the fix. The `MethodResult` trampoline (P12) has a working C/Lua prototype."
§3 P12 status: "Prototyped (verify-cct-contract `vtramp.c`: a callback that yielded 3 times completed). Still to do: error-level adjustment for the extra Lua frame and `getErrorAdjust()`."

## 6. [should-fix] §4.1 'Native loading' bullet

**Problem.** The described design contradicts the tested artifact it cites. The plan says '<gamedir>/.cclj-natives/<sha256>/ ... and never delete'. The tested NativeLoader in artifacts/scaffold-multiloader uses <gameDir>/.ccluajit/natives/<first 16 hex of sha256>/<lib>, re-hashes before reuse, and deletes stale hash directories best-effort. The '12 concurrent JVMs / locked DLL' test applies to that implementation.

**Evidence.** artifacts/scaffold-multiloader/common/src/main/java/gay/vereena/cclj/CCLuaJITCommon.java:28 `gameDir.resolve(".ccluajit").resolve("natives")`; NativeLoader.java:63 `cacheRoot.resolve(hash.substring(0, 16))`, :72 ATOMIC_MOVE, :82/86-92 `cleanupStale(...)` with Files.deleteIfExists; Javadoc :26 'Stale hash directories from older versions are deleted best-effort'. port-mechanics.md D: 'Stale versions: other hash dirs are deleted best-effort after loading'.

**Suggested text.**

"- **Native loading:** extract to `<gamedir>/.ccluajit/natives/<first 16 hex of sha256>/<lib>`. Write to a private temp file, publish it with `ATOMIC_MOVE`, and re-hash an existing file before reusing it. Delete other hash directories best-effort, ignoring failures (e.g. a DLL that another process still has loaded on Windows). This is what `artifacts/scaffold-multiloader/.../NativeLoader.java` does. It was tested with 12 concurrent JVMs and with a locked DLL on Windows."

## 7. [nice-to-have] §2 last paragraph ('For comparison ...')

**Problem.** The CraftOS-PC Accelerated figure is presented next to the vs-Cobalt numbers, but it is measured against a different baseline. Its 8–16x and 'over 30x in some scenarios' are relative to standard CraftOS-PC, which is a native PUC-Lua emulator, not Cobalt, and neither figure has a published method. Read as vs-Cobalt numbers, they would wrongly make the 8–9x measured here look weak.

**Evidence.** prior-art.md §1: 'Docs claim "over 30x faster than standard CraftOS-PC in some scenarios" ...; the v2.4.1-luajit release notes claim "8-16x performance improvements" ... Neither number comes with a published methodology.' verify-prior-art PA-5: 'Neither has a method.'

**Suggested text.**

"For comparison, CC:T's maintainer measured native PUC Lua at ~2.5x Cobalt (we measured PUC 5.4 at 2.2x). CraftOS-PC Accelerated claims 8–16x ('over 30x in some scenarios') over *standard CraftOS-PC*, which is itself a native PUC-Lua emulator, not Cobalt. Neither claim comes with a published method."

## 8. [nice-to-have] §1.2 intro sentence and '1.14.4 deadlock' bullet

**Problem.** 'All of these are verified with harnesses' is not true for the 1.14.4 typo deadlock. That finding comes from reading the code; the verifier rates it low confidence because old CC:T was not run. The bullet also wrongly includes turtle calls. In 1.14.4, turtle commands wait for 'turtle_response', which isSpecialEvent does route. The typo breaks only executeMainThreadTask ('task_complete'). The separate turtle problem is that user programs waiting for turtle_response themselves never get it.

**Evidence.** git show origin/1.14.4_CCT:src/main/java/gay/vereena/cclj/computer/LuaJITMachine.java: isSpecialEvent returns `evt.equals("turtle_response") || evt.equals("task_completed")`; executeMainThreadTask calls `pullEvent("task_complete")`; taskCompleted queues "task_complete". code-audit.md severity table CA-19: 'analysis'. verify-cct-contract CT-4: 'This was not executed ... low confidence'. code-audit CA-19: 'Programs that wait for turtle_response themselves never receive it, because the event is swallowed.'

**Suggested text.**

Intro: "Most of these were verified with harnesses built from the unmodified code. The 1.14.4 blocking-yield bugs come from reading the code (details: `reports/code-audit.md`, `reports/verify-code-audit.md`):"
Bullet: "- **1.14.4 deadlock (from reading the code, not executed):** `isSpecialEvent` matches `\"task_completed\"`, but completed tasks are queued as `\"task_complete\"`. Every `executeMainThreadTask` call (main-thread peripheral methods) therefore blocks the worker forever. Turtle commands wait for `turtle_response`, which is routed, but user programs that wait for `turtle_response` themselves never receive it."

## 9. [nice-to-have] §5 WP B; §4.2 pin bullet

**Problem.** WP B describes rebasing 01–06 onto 3c4f9fe2 as open work. In fact the patch README already says the stack applies cleanly to 3c4f9fe2, and I confirmed it builds and passes the checks there. The remaining WP B work is patches 07/08 and the per-platform builds.

**Evidence.** artifacts/luajit-patches/README.md: 'The stack applies cleanly to the vendored 19878ec0 and to 3c4f9fe2'. My run: git archive 3c4f9fe2 → git apply 01–06 (all OK) → mingw32-make BUILDMODE=static with the recommended XCFLAGS (OK) → preemption harness 25/25 'RESULT OK', latency 0.7–19.0 µs → compat_check.lua: bytecode refused, 0.30000000000000004, goto as field and as statement, table.pack, no ffi, JIT on. Files: ...\work\review-accuracy\lj3c\, ...\h\res3c.txt, build log ...\build-3c.log.

**Suggested text.**

"| B | LuaJIT patch stack: pin `3c4f9fe2` (01–06 already apply, build and pass the preemption harness and `compat_check.lua` there), write 07 (interruptible patterns) and 08 (C-stack guard), build script, assert builds | 2–4 d | 5–10 d |"

## 10. [nice-to-have] §3 P8

**Problem.** 'Lua sort is 3.2x faster under the JIT' comes from a separate quicksort benchmark (1e6 doubles). The pure-Lua sort in the shim the plan ships was measured at 1.5–1.8x faster than LuaJIT's C sort.

**Evidence.** luajit-runtime.md §c: 'C table.sort 419 ms vs pure-Lua quicksort 131 ms'. rom-compat.md §9: table.sort 200k: native 39 ms vs shim 22 ms; with comparator 80 vs 54 ms.

**Suggested text.**

"... pure-Lua `table.sort` and function-replacement `gsub` (this also fixes yieldability; under the JIT the shim's Lua sort measured 1.5–1.8x faster than LuaJIT's C sort, and a quicksort 3.2x faster on 1e6 doubles) ..."

## 11. [nice-to-have] §3 P1 parenthetical on CHECKHOOK

**Problem.** The reason given for rejecting CHECKHOOK is incomplete. The decisive measured problem is that CHECKHOOK cannot coexist with a count hook that stays installed: every trace then exits at every check. That forces on-demand arming from another thread. Cross-thread arming is the race-prone part; signals or SuspendThread are untested alternatives.

**Evidence.** preempt-patch.md §5: CHECKHOOK + installed hook 25–73x slower than JIT; verify-preempt-patch PP-5: '3.5x-77x vs JIT and 2.4x-19x vs the plain interpreter'; upstream comment: 'only useful if hooks are *not* set most of the time' (lj_record.c:2888). verify-prior-art PA-2: segfaults and lost interrupts from cross-thread lua_sethook.

**Suggested text.**

"(Upstream's `LUAJIT_ENABLE_CHECKHOOK` also stops loops, but it cannot coexist with a count hook that stays installed: every trace then exits at every check, 3.5–77x slower than the JIT. Hooks would have to be armed on demand from another thread, and cross-thread `lua_sethook` was shown to segfault and to lose interrupts permanently. Don't use it.)"

## 12. [nice-to-have] §6 Risks, macOS bullet

**Problem.** The plan leaves out a relevant upstream fact from prior-art. LuaJIT added a build option for MAP_JIT under Apple's hardened runtime. It is in the recommended 3c4f9fe2 pin but not in the vendored copy, and it is missing from the recommended build flags.

**Evidence.** prior-art.md §4: '`LUAJIT_ENABLE_OSX_HRT` build option (commit 4f2bb199, 2025-03-10, issue #1334) for MAP_JIT under Apple hardened runtime'. git merge-base: 4f2bb199 is an ancestor of 3c4f9fe2 and not of 19878ec0.

**Suggested text.**

Append to the macOS bullet: "Upstream added `-DLUAJIT_ENABLE_OSX_HRT` (4f2bb199, 2025-03-10) for MAP_JIT under the hardened runtime. It is in the `3c4f9fe2` pin but not in the vendored `19878ec0`, and it is not in our recommended flags. Add it to the macOS CI builds and check JIT status in the smoke test."

## 13. [nice-to-have] TL;DR row 1

**Problem.** 'natives load from the original author's home directory' is inaccurate. The paths are under /run/media/vereena/ldata/..., which is a mounted data drive, not a home directory. §1.1 states this correctly.

**Evidence.** git show 505b1f9:src/main/java/gay/vereena/cclj/computer/LuaJITMachine.java: System.load("/run/media/vereena/ldata/Projects/ccluajit/vendor/luajit/src/libluajit.so") and .../src/main/cpp/cclj.so.

**Suggested text.**

"The 1.20.1 branch is a stub: the Java machine returns `null`, and the natives are `System.load`ed from absolute paths on the original author's machine (`/run/media/vereena/ldata/...`)."
