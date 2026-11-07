# CCLuaJIT → Minecraft 1.21.1: findings and port plan (v2)

*Investigation date: 2026-11-07. Target: CC:Tweaked 1.121.0 (the latest for MC 1.21.1), Java 21.*
*Method: 8 investigations and 8 adversarial re-checks, then 3 independent reviews of plan v1 (accuracy, completeness, sequencing) and 2 prototype lanes (pause protocol; patches 07/08). All of these used real builds, JNI harnesses and benchmarks on this machine. Reports are in [`reports/`](reports), reusable code is in [`artifacts/`](artifacts), and the technical design is in [DESIGN.md](DESIGN.md). The changes from v1 are listed at the end.*

---

## TL;DR

| Question | Answer |
|---|---|
| **Does the mod work?** | **No, on any branch, in any useful sense.** The 1.20.1 branch is a stub: the Java machine returns `null`, and the natives are `System.load`ed from absolute paths on the original author's machine (`/run/media/vereena/ldata/...`). Its C++ does not compile, and the jar it builds contains no native code. The last "working" branch (1.14.4, Forge) ran LuaJIT **with the JIT compiler switched off**. From reading its code, it deadlocks on every main-thread peripheral task. Its native code also has at least 7 reproduced JVM-killing triggers, several of them reachable from ordinary player code (for example, a cyclic or ~5,000-deep table passed to any API). |
| **Can LuaJIT deliver real performance?** | **Yes, for CPU-bound Lua: about 8x faster than CC:T's Cobalt.** That is the geometric mean over 10 CC-style benchmarks (range 2.3x–29x), measured against Cobalt running inside CC:T 1.121.0's real machine, with correct unsigned `bit32`. With the JIT off it is 3.7x. **This needs `bit32` written in C:** with the current pure-Lua shim, SHA-256 is only ~1.5x Cobalt. Players notice the speed-up only in CPU-heavy programs (crypto, compression, rendering, emulators, path-finding, big sorts and serialisation). Event-driven, turtle and peripheral programs are bound by the 50 ms game tick and won't feel a difference. |
| **How much work is the 1.21.1 port?** | The **Minecraft/loader side is small**. CC:T's machine API is unchanged from 1.20.1 to 1.21.1, and 1.21.1 build scaffolds already compile. The **real work is a new Lua machine and JNI bridge** that is safe enough to run untrusted player code inside a server, plus the tooling to prove that. Effort, including 20% contingency: **≈ 45–70 person-days for an experienced C/JNI/LuaJIT developer** and **≈ 50–90 for you** (strong C; new to JNI and modern Java). A **personal-use MVP** (NeoForge, Windows and Linux, single-player or private server) takes ≈ 21–37 person-days for you, without contingency. **At your 6–8 hours a week:** working alone, the MVP takes ≈ 5–11 months and a public-server-grade release ≈ 1–2.3 years. With Claude doing most of the implementation while you review and decide, the MVP takes ≈ 2–7 months (midpoint ~3.5) and the release ≈ 4–16 months (midpoint ~9). That second set assumes your hours are 35–60% of the person-days; calibrate it at gate G1 (§5.5). |
| **Is it feasible at all?** | **Yes. Every VM-level blocker now has a fix that was prototyped and tested here:**<br>• JIT loops that ignore the timeout: patch 01, 25/25 loop shapes stopped in under a millisecond.<br>• Uninterruptible pattern matching: patch 07.<br>• C-stack overflows that kill the JVM: patch 08, 31/31 shapes survive.<br>• Bytecode sandbox escape: patch 03.<br>• Transparent pause/time-slicing: C/Lua coroutine wrapper, thousands of pauses with byte-identical results.<br>• Yielding Java callbacks: Lua trampoline.<br>**Still design-only:** the JNI machine itself, the cross-thread `close()`/lock protocol, the curated `debug` library, Java-object lifetime, the memory caps (only the allocator is prototyped), and patches 09–11. **Nothing has run end-to-end inside CC:T yet.** A headless CC:T runner that can host it now exists (WP 0). |

---

## 1. What is actually in the repo

### 1.1 Branch status

| Branch | Loader / CC version | State |
|---|---|---|
| `1.20.1_CCT` (current) | Fabric, CC:T 1.115.0 | **Unfinished port, non-functional.**<br>• `gradlew build` succeeds, but the jar is 5.5 KB with 2 classes and no natives.<br>• `LuaJITMachine.handleEvent` returns `null`.<br>• `System.load` uses `/run/media/vereena/...` absolute paths.<br>• Native `registerAPI` is commented out.<br>• `main.cpp` does not compile: `assert` is used without `<cassert>`, a regression from the last WIP commit `505b1f9`.<br>The commit message itself says *"we're probably in for a fair bit of pain here"*. |
| `1.14.4_CCT` | Forge coremod, CC:T 1.86.2 | Last plausibly-running version. It was designed for CC:T's pre-1.87 API (blocking `ILuaContext.pullEvent`), which no longer exists. Serious bugs (§1.2). |
| `1.12.2_CC`, `1.7.10_CC` | Original ComputerCraft | Historical, ASM transformers. |
| `main` | n/a | README, logo and licence only. |

### 1.2 The old design's problems
Most of these were verified with harnesses built from the unmodified code. The 1.14.4 deadlock comes from reading the code. Details: `reports/code-audit.md` and `reports/verify-code-audit.md`.

- **The JIT was never enabled on any branch.** `initMachine` opens base/math/string/table/bit but never calls `luaopen_jit`, and in LuaJIT only `luaopen_jit` switches the JIT on. The mod therefore always ran at LuaJIT *interpreter* speed. That is also why its hook-based abort ever worked.
- **JVM-killing triggers (reproduced in harnesses on the unmodified native code):**
  - an unknown Java type in an event;
  - a Java map with a `null` or NaN key;
  - a cyclic Lua table passed once to any API (e.g. `os.queueEvent`) or yielded;
  - a ~5,000-deep nested table;
  - a function yielded to the top level (because of an off-by-one, two yielded values are needed);
  - `close()` while running: a use-after-free. CC:T 1.21.1 calls `close()` from its monitor thread, whereas 1.14.4's coremod rerouted that call to `abort()`.
  - abort after close.

  There are three causes:
  - Lua errors raised outside any protected call: a fatal SEH `0xE24C4A02` on Windows, and `PANIC` + `exit(1)` on Linux, which skips Java shutdown hooks;
  - unbounded native recursion in the converters: a C-stack overflow at ~5,000 levels, which cyclic tables also hit;
  - the cross-thread use-after-free.

  A cyclic *Java* map only throws a `StackOverflowError`. The JVM survives, but the following JNI calls are made with the exception still pending.
- **Sandbox escape:** `load`/`loadstring` accept LuaJIT bytecode, and `string.dump` is available. LuaJIT has no bytecode verifier. In fuzzing, 9–25% of mutated chunks crashed the process, and crafted bytecode is a known route to native code execution.
- **Strings are not 8-bit clean.** Bytes 0x80–0xFF become U+FFFD, Java→Lua uses modified UTF-8, and `byte[]` is cut at the first NUL. CC:T maps bytes 1:1 to chars 0–255 (Latin-1), and CC's teletext/graphics characters and binary files depend on that.
- **JNI local references leak until the program yields:** +32 MB of native memory per 1M calls, and GC pauses of 0.24–3.9 s.
- **Aborting is one-shot and catchable by `pcall`**, so `while true do pcall(f) end` cannot be stopped.
- **1.14.4 deadlock (from reading the code, not executed):** `isSpecialEvent` matches `"task_completed"`, but completed tasks are queued as `"task_complete"`. Every `executeMainThreadTask` call (main-thread peripheral methods) therefore blocks the worker forever. Turtle commands wait for `turtle_response`, which is routed, but user programs that wait for `turtle_response` themselves never receive it.
- **`bit32 = bit` has the wrong semantics:** signed results, rounding instead of truncation, and no `extract`/`replace`/`btest`/`lrotate`/`rrotate`. The CC:T ROM uses `bit32.extract`, and a `bit32`-based CRC32 gives different answers.
- **Build pipeline:** a Fedora 30 Docker image (EOL), macOS never built, a Makefile that drops `libluajit` under `--as-needed`, and temp-directory native extraction that leaks on Windows.

**Salvageable:** less than 10% of `main.cpp` and less than 5% of the Java side. Keep the ideas: `ServerContext.luaMachine` injection, cached JNI IDs, the restricted library set, identity-based cycle tracking. **Plan for a rewrite, using the old code as a sketch.** Do not start coding from the old `main.cpp`.

---

## 2. Performance: what LuaJIT can deliver
Sources: `reports/perf-bench.md`, `reports/verify-perf-bench.md`, `reports/review-accuracy.md`, and `artifacts/bench`. Cobalt was driven through CC:T 1.121.0's real `CobaltLuaMachine`.

| Runtime | vs. warm Cobalt 0.9.9 (geomean, 10 benchmarks) |
|---|---|
| LuaJIT, JIT on, correct unsigned `bit32` | **8.1x** (fib 26x, mandelbrot 29x, OOP 18x, SHA-256 22x; tables/strings/serialise/LZW 3.5–6x; compiling Lua 2.3x). The v1 figure of 8.9x (SHA-256 55x) used LuaJIT's signed `bit` ops, which give wrong results for CC programs. SHA-256's 22x was measured with a fast, correct 2/3-argument Lua `bit32` wrapper. A C `bit32` is expected to be at least as fast, but has not been measured. |
| … with the pure-Lua `bit32` shim | SHA-256 only ~1.5x: its loops abort JIT traces. **`bit32` must be in C.** |
| LuaJIT, JIT on, with the patch stack | Patches 01–06: +1% geomean on these 10 benchmarks, except `strings` at +22–25%, because patch 05 makes `tostring(float)` ~2.6x slower (Cobalt-compatible round-trip formatting). Patches 07 and 08 were measured only on microbenchmarks: 07 adds +1.8% on pattern microbenchmarks (worst +8.7%), and 08 adds +0.5%. The full 01–08 stack has not yet been run on the 10 benchmarks (WP H). |
| LuaJIT, interpreter only (`-joff`) | 3.7x (3.2x with a count hook every 1,000 instructions) |
| PUC Lua 5.4 (reference) | 2.2x |

Other costs to know about:
- **The count hook must stay installed** for the interrupt protocol. It costs nothing in JIT-compiled code, but adds ~+23% geomean (+1% to +65%) to interpreted code: cold code, paths the JIT does not compile, and `coroutine.*` plumbing.
- **The coroutine wrapper** (pause and nesting limit) adds ~13–15 ns per `resume`. That is 1.3–1.7x on coroutine ping-pong microbenchmarks and ~0% on real programs. Each pause costs ~1–2.5 µs.

**Host (Lua→Java) call cost**, per call:

| Call path | Cost per call |
|---|---|
| Cobalt via CC:T | 29–36 ns (no-op), 41–54 ns (number) |
| Old CCLuaJIT boxed bridge | 1.4–10.6x slower than Cobalt |
| Lean JNI bridge (planned) | parity for numbers; 1.2–4.8x Cobalt for string/`term.blit`-style args |

All of these are under 1 µs. A full 51×19 screen redraw costs ~36 µs on Cobalt and ~13–18 µs on LuaJIT, against a 50,000 µs tick.

**Fairness (time-slicing),** measured with the headless runner (`artifacts/headless-runner/src/FairnessProbe.java`). Computer A burns 3 s of CPU without yielding, while computer B waits on 50 ms timers. The table shows B's worst lateness:

| Machine | Worst lateness of B's timer |
|---|---|
| Cobalt (pauses at 5–50 ms) | 76 ms |
| A machine that never pauses, CC:T default 1 worker thread | **3,717 ms** |
| Never pauses, `computer_threads = 2` | 14 ms (helps only while busy computers < threads) |

**Where players notice it:**
- Programs that compute. SHA-256 goes from ~0.9 MB/s on Cobalt to ~20 MB/s with a fast, correct `bit32`. That figure was measured with a 2/3-argument Lua wrapper; the planned C `bit32` has not been benchmarked yet.
- Jobs that currently die with *"Too long without yielding"*.
- Not in programs that mostly wait for events, timers, rednet, turtles (~8 ticks per move) or main-thread peripheral calls (≥ 1 tick each).

For comparison: CC:T's maintainer measured native PUC Lua at ~2.5x Cobalt, and we measured PUC 5.4 at 2.2x. CraftOS-PC Accelerated claims 8–16x ("over 30x in some scenarios") over *standard CraftOS-PC*, which is itself a native PUC-Lua emulator, not Cobalt. Neither claim comes with a published method.

---

## 3. The hard problems, and their status

The "When" column gives the milestone (§5.2). Design details are in [DESIGN.md](DESIGN.md).

| # | Problem | Severity | Status | When |
|---|---|---|---|---|
| P1 | **JIT-compiled loops ignore debug hooks**, so CC:T's timeout can't stop `while true do end`. CC:T then abandons the worker thread, which spins forever, and the computer is wedged until it is reloaded. | critical | **Solved and tested.** Patch `01` adds a thread-safe `luaJIT_interrupt()` flag that traces check. 25/25 loop shapes stopped:<br>• Windows x64: 0.6–49 µs across builds, including an assert build. Logs: `reports/preempt-patch.md` and `artifacts/luajit-patches/results/`.<br>• Linux x64 under WSL: 0.9–564 µs (`artifacts/harnesses/preempt/preempt-linux-x64.txt`).<br>• arm64: the preemption harness passes on linux-aarch64 and macOS arm64 (first CI run, §6; harness only, no JVM).<br> Upstream's `LUAJIT_ENABLE_CHECKHOOK` cannot coexist with a count hook that stays installed: every trace exits at every check, making it 3.5–77x slower than the JIT. It would therefore need cross-thread `lua_sethook`, which was shown to segfault and to lose interrupts. Don't use it. | M0–M1 |
| P2 | **Long C library calls can't be interrupted.** `string.find` backtracking ran for more than 300 s, and plain `find` is O(n·m): about 90–105 s on a 6 MB input, depending on the build. Cobalt stops these in about 1 s. | critical | **Solved and tested.** Patch `07` polls the flag in the pattern matcher and in `table.sort`: 17/17 pathological cases stop in 2–50 µs. Plain `find` gets a linear-time Two-Way fallback that also works on JIT traces (~90 s → 10–21 ms). A 2.18M-result differential fuzz gave identical results. Remaining: patch 11 (lexer poll). An unpolled `load` of a 20 MB chunk took 1.0 s, and of a 38 MB chunk 1.5 s. CC:T abandons a worker ~3 s after the hard abort. We *expect* the 64 MiB cap to keep a single parse below that window, but this is unproven, so patch 11 closes it. A WP B inventory lists every C function reachable from Lua with its complexity bound; the rule is that anything worse than O(bytes touched) must poll. | M1 (07), M2 (11) |
| P3 | **Hook-disabled contexts:** `__gc` finalizers, user `debug.sethook` functions and `xpcall` handlers run with hooks off, so they can't be interrupted. | critical | **Solved.** `newproxy` is removed and native `debug.sethook` is not exposed (the ROM never uses it). Patch `02` makes errors raised from hooks skip `xpcall` handlers. Tested. | M1 |
| P4 | **Cross-thread lifecycle.** CC:T calls `close()` (repeatedly) and `printExecutionState()` from the Monitor thread while Lua runs. A naive lock order deadlocks the Monitor thread. | critical | Design (DESIGN §3.1): lock order `TimeoutState` → `machine.lock`; `close()` removes the listener first, then sets flags and interrupts; the owning worker frees the state. The failure itself is reproduced. **Needed in M1**, because the first hard abort triggers it. | M1 |
| P5 | **Unprotected Lua errors kill the JVM** (converters, OOM, stack growth). | critical | Design rule (DESIGN §3.4, §4.3): every Lua API call runs under protection; a JNI phase rule; conversions are iterative, raw-only, with depth limits, cycle tracking and size budgets. | M1 |
| P6 | **Bytecode loading** is a sandbox escape, and a Lua-level guard is bypassable (`debug.getupvalue(load, n)` returns the real `load`). | critical | **Solved and tested.** Patch `03` makes every loader refuse binary chunks; `string.dump` is removed. | M1 |
| P7 | **`debug` library, registry and environments expose host internals.** The ROM needs `debug.getregistry` / `getupvalue` / `getinfo` / `traceback`, but the real ones reach `_PRELOAD` (`jit.util` leaks native addresses), host userdata metatables (`getmetatable(ud).__gc(ud)` is a use-after-free), and private native functions. `debug.getlocal` on a suspended coroutine read the trampoline's native resume function, which a probe then called with a forged handle. `getfenv`/`setfenv` on built-ins is the Luanti CVE-2026-41196 class. | critical | Design (DESIGN §3.5): host-owned functions and metatables are opaque to `getinfo('f')`, `getlocal`, `getupvalue`, `getmetatable` and `getfenv`/`setfenv`; a fake registry; one sandbox `_G`; native entry points validate their arguments. | M1 (remove dangerous libraries), M2 (complete) |
| P8 | **Native C-stack overflow kills the JVM** (deep `gsub` callback, sort comparator, `__tostring`, coroutine nesting). LuaJIT has no C-call depth limit, and CC:T threads have ~1 MB stacks. | critical | **Solved and tested.** Patch `08` raises a catchable "C stack overflow" at every C→VM re-entry inside a red zone. 31 recursion shapes ran with 0 crashes on Windows and Linux native threads; 8 cases, including JNI upcalls at every level, ran in a JDK 21 JVM and it survived. The coroutine wrapper adds a nesting limit (count 200 + stack floor; prototyped). The host must set the limit per worker thread before each resume. | M1 |
| P9 | **No memory limit.** LuaJIT memory is native, outside `-Xmx`; one computer allocated 2 GiB. | high | The allocator wrapper is prototyped (+2–5%). The design adds a **global** cap across all computers and a `maxmcode` cap for JIT code (DESIGN §3.6). Otherwise many computers, each under its own cap, can still get a container OOM-killed. | M1 (per computer), M2 (global) |
| P10 | **Pause / time-slicing.** A hook can only yield the *innermost* coroutine, and CC:T programs always run inside `parallel`/`multishell` coroutines. Without pause, one busy computer delays every other computer on the default single worker thread. The measured delay was 3.7 s for a 3 s busy loop, and the bound is CC:T's 7 s soft-abort window. | high | **Prototyped and tested** (`artifacts/pause-prototype`): the hook yields only when the whole resume chain can yield, and each wrapper yields upward until the top returns PAUSE. ROM `parallel` ran through 2,719 pauses (61k at 20–60 µs) with byte-identical results, and 7 nesting levels all paused. Pauses inside non-yieldable C callbacks are deferred, up to ~0.5 s (fairness only); checkpoint wrappers cut that to 6–17 ms. Re-run on the final stack (LuaJIT `3c4f9fe2` + 01–08, final flags): all 7 suites (46 sub-tests) pass, including 2,548 pauses in the ROM `parallel` test and hard-abort latency of 0.1–27 µs (`artifacts/pause-prototype/logs/run_tests-3c4f9fe2-01-08.out`). | M2 (gate G4) |
| P11 | **The ROM needs Lua 5.2 features LuaJIT lacks:** `_ENV` as a value (14 files), `table.pack`/`unpack`, unsigned `bit32`, `utf8`, `string.pack`; NUL in patterns; number formatting; `goto` as an identifier. | high | **Mostly solved and tested.** Build with `LUA52COMPAT` and patches 04–06, plus the `_ENV` emulation and shims in `artifacts/lua/cclj_prebios.lua`. The real 1.121.0 BIOS boots to the shell in a mock harness and runs `ls`, `lua`, `require`, `cd`, `programs`, etc. **`bit32`, `utf8` and `string.pack` must move to C.** The Lua `bit32` is slower than Cobalt on full 32-bit values, and the Lua `string.pack` is ~5x slower. Start from CraftOS-PC's MIT C ports. | M1 |
| P12 | **Yielding Java methods** (`MethodResult` with `ILuaCallback`: turtle commands, main-thread peripheral methods, `peripheral.call`). LuaJIT can't resume a C function after a yield. | high | **Prototyped** (`reports/verify-cct-contract.md` CT-4, `vtramp.c`): a Lua trampoline completed a callback that yielded 3 times. Still to do: error-level adjustment for the extra frame, and `getErrorAdjust()`. | M1 |
| P13 | **Remaining differences from Cobalt** that players can see. See the list below. | low–medium | Accept and document. Patch 09 narrows the `unpack` limit. CraftOS-PC's `CCT-Tests.patch` (the CC:T specs it had to drop on LuaJIT) is a ready checklist. | M2 (`expected-failures.txt`), M3 (docs, WP I) |
| P14 | **Java object lifetime.** CC:T relies on Java GC to close files and websockets that a program forgot to close (`WeakReference` against `maximum_open_files` = 128, and `GlobalCleaner`). LuaJIT tables have no `__gc`, and pinning handles until `close()` breaks this. | high | Design (DESIGN §4.4): one host userdata per Java object; its `__gc` pushes the handle ID onto a native ring that Java drains after each event; generation-tagged handles. | M2 (MVP: known leak until reboot) |
| P15 | **Identity leaks.** LuaJIT seeds `math.random` with a fixed value, so every computer and every boot gets the same sequence; rednet then **silently drops messages** between computers that boot together, because message IDs collide. `tostring(t)` and `%p` print raw heap addresses, which defeat ASLR. | high | Design (DESIGN §3.5): seed from Java `SecureRandom` (trivial); patch 10 for a hashed `tostring`. | M1 (seed), M2 (patch 10) |
| P16 | **Server-wide resources and operations.** Native memory and JIT machine code are not bounded across computers. A player can flood the log with Java stack traces. There are no metrics, no live kill-switch and no per-computer quarantine. | high (public servers) | Design (DESIGN §3.6, §5): global cap, rate-limited and sanitised logging with CC:T's markers, CC:T `Metric`s, a reloadable config and a `/ccluajit` admin command. | M2 |

**P13: differences players will see** (these go into the README):
- No yield inside C-library callbacks: `xpcall` message handlers, `string.format` (`__tostring`), and `load` reader functions. Also the C `sort` comparator and `gsub` callback, unless the planned pure-Lua `sort`/`gsub` replace them. Cobalt allows yields in all of these. `coroutine.isyieldable()` is false there, while Cobalt returns true.
- `local _ENV` is unsupported. The emulation covers `_ENV` as a value, which is what the ROM uses.
- "Did you mean" error hints degrade (CC:T `error_hints_spec`).
- `table.unpack` / `string.byte(s, 1, -1)` stop at 8,000 values; with patch 09 the limit is ~30k–64k. Cobalt allows at least 2^19 (CC:T `vm_spec`). Code like `string.char(table.unpack(bytes))` on large strings needs chunking.
- `pairs()` iteration order differs from Cobalt and can differ between computers, so code or tests that depend on `textutils.serialise` key order break.
- There is no `debug.sethook` (CC:T `coroutine_spec` "within debug hooks" fails; Lua profilers don't work).
- `coroutine.wrap` re-raises child errors unchanged, as Cobalt does. Native LuaJIT would add a position.
- **Lexer and parser differences**, found by differential parsing against Cobalt 0.9.9 (`reports/pin-head-gate.md`, `reports/version-survey.md`):
  - LuaJIT rejects unknown escapes such as `"\)"` and `"\q"`, which Cobalt accepts; at least two published CC programs use `\)`.
  - LuaJIT allows at most 60 upvalues per function; Cobalt allows more.
  - LuaJIT accepts `0b101` literals, a UTF-8 BOM and identifiers containing bytes above 0x7F. Cobalt rejects all three, so code written on CCLuaJIT would not port back.
  - LuaJIT with patch 06 rejects `goto goto`, which Cobalt accepts.
  - Planned patch 12 (lexer parity) closes the escape, `0b`, BOM and identifier gaps. The 60-upvalue limit is structural in LuaJIT and stays as a documented limit.
- **Still to check against Cobalt:**
  - LuaJIT skips a first line starting with `#` in every `load()`, not only in files;
  - LuaJIT treats `for i = 1, 3, 0` as ascending and loops forever (stoppable by the timeout).

### 3.1 Acceptance tests per problem
Most of these come from CC:T's own test suite, which the headless runner already executes (WP 0).

| Problem | Acceptance test |
|---|---|
| P1 | `timeout_spec` "in loops" (message `<chunk>:<line>: Too long without yielding`); preemption harness 25/25 on every shipped platform |
| P2 | `timeout_spec` `string.find`/`string.match` raise exactly `Too long without yielding` **with no position prefix** (the hook runs inside the C function); `natives/tests/run-harnesses.sh` pattern harness 74/74 (Windows) |
| P3 | the preemption harness `X_*` cases are unreachable from the sandbox (no `newproxy`, no `debug.sethook`) |
| P4 | headless: `while true do end` → hard abort → `close()` from `ComputerCraft-Computer-Monitor-0` while `handleEvent` is running; the JVM survives, the worker count is unchanged, and `ctx.close(2, SECONDS)` returns true. Plus the 10-minute lock stress test (DESIGN §3.1). |
| P5 | the 7 old JVM-killers (§1.2) ported as headless Lua specs, with test-API hooks for the Java-side cases (`null`/NaN map keys, unknown Java type). Also a cyclic Java map: it was not fatal, but it must become a clean Lua error instead of a pending `StackOverflowError`. |
| P6/P7 | sandbox specs: bytecode refused; `getupvalue`/`getlocal`/`getinfo('f')`/`getfenv` cannot reach host functions; `debug.getmetatable(handle)` is nil; no registry leak; calling a stolen native function with a forged handle errors |
| P8 | `natives/tests/run-harnesses.sh` C-stack harness 31/31 on x64 (27/4 on Linux arm64 without the fast-path port, §6); `coroutine_spec` "allow yielding within …" (gsub, sort, metamethods, pcall, xpcall); a nesting-depth spec errors without crashing |
| P9/P16 | OOM spec (catchable, and the computer keeps working); 200-computer soak inside a memory-limited container |
| P10 | FairnessProbe worst lateness ≤ 100 ms (Cobalt: 76 ms); `parallel`/`multishell` specs green with pause on |
| P11/P13 | the full CC:T suite (textutils 370 tests, `cc.internal.syntax` 355): every remaining failure is listed in `expected-failures.txt` with its P13 reason |
| P12 | headless `smoke.lua` `headless.mainAdd` (`@LuaFunction(mainThread = true)`) and a `MethodResult.pullEvent` callback |
| P14 | 1,000 unclosed files never hit "Too many files open"; the handle table stays bounded under 10,000 websocket/http handles |
| P15 | 50 machines booted in one JVM get distinct first `math.random` values; two computers booted together exchange rednet messages |

---

## 4. Architecture in one page
Full details are in [DESIGN.md](DESIGN.md).
- **Mod layer:** a multiloader Gradle project with `common`, `neoforge` and `fabric` modules (scaffold in `artifacts/scaffold-multiloader`). It is server-side only. A factory wrapper sets `ServerContext.luaMachine` and decides per computer boot (config, quarantine, Cobalt fallback). A reflective CC:T contract probe at startup falls back to Cobalt on mismatch, instead of pinning a CC:T version range.
- **Native layer:** one static `libcclj` per platform, written in **plain C**, linking LuaJIT `cclj/v2.1` (`14d8a7a2` + 17 upstream fixes) + patches 01–08 (09–11 to come). It is built with `-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT`. Linux builds are made in `manylinux_2_28`. LuaJIT becomes a maintained fork with a backport list.
- **Host core:**
  - the interrupt protocol (a count hook that stays installed + `luaJIT_interrupt`, no cross-thread `lua_sethook`);
  - the coroutine wrapper (nesting limit + pause token protocol);
  - the per-thread C-stack limit;
  - the protected boundary;
  - the curated environment with opaque host functions;
  - PRNG seeding;
  - per-computer and global memory caps.
- **Java machine:** the lean JNI bridge (cached IDs, `Push/PopLocalFrame`, Latin-1 `byte[]`); lazy `IArguments`; iterative raw conversions with budgets; generation-tagged handles with GC-driven release; the `MethodResult` trampoline; a lock-ordered `close()`; CC:T metrics and sanitised, rate-limited logging.

---

## 5. Work plan and estimates

### 5.1 Work packages
Estimates are in **person-days of 8 focused hours**, for one person working alone.
- **"Experienced"** means someone fluent in C, JNI and LuaJIT internals.
- **"You"** comes from decision 0: about 10 years of systems C (DPDK-class work), little JNI, Java at the Java 8 level, and Minecraft modding experience from 1.7.10. The C-heavy packages (B, C, F, G) sit close to the experienced column, plus time to learn the Lua C API and LuaJIT internals. The JNI and modern Java/Gradle/NeoForge packages (0, A, D, J, K) sit closer to a learner's pace.
- §5.5 converts these figures to calendar time at 6–8 hours a week, both alone and with Claude doing most of the implementation.
- The generic "learning" column in v2 assumed no C background; it totalled 70–120 days.

| WP | Content | Experienced | You |
|---|---|---|---|
| 0 | **Headless CC:T runner and conformance oracle.** Copy `artifacts/headless-runner` into `common/src/test`, vendor CC:T's `test-rom` (tag `v1.21.1-1.121.0`, MPL-2.0 headers kept), and add Gradle `headlessSuite` tasks. Reproduce the Cobalt baseline: 1407 tests, of which 1390 pass, 12 are pending, and 5 HTTP errors appear when HTTP is disabled. Then run once with HTTP enabled (NettyHttp comes in through the core POM) and expect 1395. Add a `--factory` switch to the benchmark runs for end-to-end comparisons that include host calls. | 0.5–1 | 0.5–1.5 |
| A | **Mod project.** Start from the scaffold. NeoForge first; Fabric is +0.5–1 / +1–2 and included here. Native loader (musl detection, antivirus/noexec hints), factory wrapper with Cobalt fallback, CC:T contract probe, factory-replaced and mixin-conflict warnings, server-side-only metadata. | 2–3 | 2–4 |
| B | **LuaJIT stack.** Patches 01–08 are done. Remaining:<br>• host `cclj/v2.1` (`14d8a7a2` + the 17 picks in `luajit-patches/backports/`) in your LuaJIT fork, and set up the monthly triage (`BACKPORTS.md`);<br>• patch 12, lexer parity with Cobalt (see P13);<br>• patches 09–11;<br>• an inventory of every C function reachable from Lua with its complexity bound (anything worse than O(bytes touched) must poll);<br>• harness cases `X_huge_parse` and `X_gsub_many_matches`;<br>• assert and ASan builds;<br>• optionally, port 08's coroutine fast-path check to arm64. | 2–3.5 | 2–4 |
| C | **Host core (C).** Counting allocator (two caps, `maxmcode`); curated stdlib, `debug`, registry and environments (DESIGN §3.5); C `bit32`/`utf8`/`string.pack` (adapting CraftOS-PC's ports); interrupt state machine and lock order; protected boundary; `close()` deferral; per-thread C-stack limit; PRNG seeding; dead-handle ring. | 8–12 | 9–14 |
| D | **JNI bridge + `LuaJITMachine`.** Lazy `IArguments`, iterative raw conversions with budgets, `ByteBuffer` rules, method tables, handle table with GC-driven release, callback trampoline, error levels, `TimeoutState` listener, `MachineResult` mapping. | 7.5–11 | 9–15 |
| E | **Lua prelude.** `_ENV` emulation and the rest of `cclj_prebios.lua`, Lua `sort`/`gsub`, kept free of privileged upvalues. | 1–2 | 1–3 |
| F1 | **Coroutine wrapper with nesting limit**, productionised from `artifacts/pause-prototype`. Needed even without pause. | 0.5–1 | 0.5–1 |
| F2 | **Pause token protocol**, from the prototype: ticker, checkpoint wrappers, CC:T `coroutine_spec`. | 1–1.5 | 1–2 |
| G | **CI.** A week-1 harness-only job on Windows, Linux x64/arm64 and macOS arm64 (no JNI; macOS x64 joins at M4); then the full 5-platform native matrix with JVM and headless smoke tests; reproducible builds with published SHA-256; VirusTotal scan; split debug symbols. | 3–5 | 3–6 |
| H | **Testing.**<br>• `expected-failures.txt` mechanism (CI fails on new failures *and* unexpected passes).<br>• Runs with `computer_threads` = 1 and 4.<br>• The old JVM-killers as specs, plus a port of CC:T's `ComputerTest.testTimeout` (exact message).<br>• **24 h soak** on Linux x64 and arm64: 200 chaos computers in a memory-limited container (random reboot/shutdown, chunk unload, OOM, aborts, unclosed fs/http/websocket handles, deep coroutines, cyclic `queueEvent`). **Pass** = no JVM crash; live native states == running computers; global native bytes back to baseline after shutdown; handle tables bounded; RSS and `/proc/self/maps` count stable.<br>• ASan/UBSan build of `libcclj`.<br>• Differential fuzzing (JIT on / off / Cobalt).<br>• `-Xcheck:jni` everywhere.<br>• The 10-benchmark perf suite on the final stack.<br>• In-game checks on both loaders: client and Linux dedicated server.<br>*(The reviewer estimated 8–12 / 15–24. That included building the runner, which is now WP 0 and already prototyped.)* | 7–10 | 8–13 |
| I | **Docs.**<br>• README / mod page: speed-up expectations, P13 list, config, platform matrix, how to disable, the native-cache path for antivirus exclusions, and a "secure defaults" statement (what is removed: bytecode, `debug.sethook`, `jit`/`ffi`/`io`/`os`, the real registry; plus the caps).<br>• Developer docs: natives, patch rebases, headless suite.<br>• `SECURITY.md`: private vulnerability reporting, a 90-day disclosure policy, and a supported-versions table. | 1–2 | 1–2 |
| J | **Release and licensing.**<br>• REUSE/SPDX headers and `reuse lint`.<br>• `META-INF/licenses`: LuaJIT (MIT, including Lua and dlmalloc notices); CraftOS-PC (MIT) for the ported C libraries; Cobalt/LuaJ (MIT) if `bit32` or `StringMatch` logic is ported; the MPL-2.0 text if test code is published.<br>• Metadata `license`: `MIT` only while the shipped jar contains no MPL/CCPL code, otherwise `MIT AND MPL-2.0`.<br>• Version scheme `x.y.z+mc1.21.1`.<br>• Modrinth and CurseForge projects (allow for moderation time).<br>• Changelog. | 1–2 | 1.5–3 |
| K | **Config, admin command, observability** (DESIGN §5). | 2–3 | 2.5–4 |
| L | **Add-on compatibility pass in-game:** generic inventory/fluid methods, Advanced Peripherals, UnlimitedPeripheralWorks, Create, Tom's Peripherals; plus CCGraphics/cride to check the incompatibility warning. | 1 | 1–2 |
| | **Total** | **≈ 38–58** | **≈ 42–75** |
| | **With 20% contingency** | **≈ 45–70** | **≈ 50–90** |

Why the experienced figure is higher than in v1 (26–43):
- The reviews found ~20 things v1 left out: Java object lifetime, global memory, more sandbox routes, lock order, PRNG, licensing, compatibility policy, config/observability, docs/release/add-on work, soak testing.
- The contingency is now explicit.
- The prototypes removed about 2–4 days (experienced) from WP B and F.

### 5.2 Milestones and stop/go gates

The last two columns are cumulative person-days. §5.5 turns them into calendar time.

| Milestone | Content | Gate (measurable) | Cumulative, experienced | Cumulative, you |
|---|---|---|---|---|
| **M0 – Toolchain and oracle** | WP 0; scaffold builds; the mod loads in a NeoForge dev client and falls back to Cobalt; a client without CCLuaJIT can join a server that has it; patched LuaJIT and `libcclj` build on Windows and Linux; week-1 CI harness job. | **G0:** the Cobalt baseline reproduces headless (1390/1407). On **Windows x64 and Linux x64** (required, decision 4):<br>• the preemption harness passes 25/25;<br>• the C-stack harness completes 31/31;<br>• `NativeProbe` loads the library in a JDK 21 JVM.<br>*Optional, informational only:* the same harness-only jobs on GitHub's linux-arm64 and macOS arm64 runners. They are free and need no hardware, but nobody is committed to fixing what they find yet. macOS needs a `pthread_get_stackaddr_np` port of the stack-bounds code. These jobs run no JVM, so they cannot measure the C-stack margin on 16 KiB / 64 KiB pages; that needs a JVM on such hardware (M4). | 2.5–4 d | 3–6 d |
| **M1 – Personal MVP** | Built as vertical slices, each headless and testable:<br>**S1** a 3-line bios calls `term.write`, and an error comes back as `MachineResult.error`;<br>**S2** events, filters, `terminate`, `GENERIC_ERROR`;<br>**S3** conversions (Latin-1, `byte[]`/`ByteBuffer`, cycles, depth limits, `IArguments`, `LuaException` levels);<br>**S4** the real ROM boots, and `smoke.lua` passes;<br>**S5** `MethodResult` trampoline, `mainAdd`, `pullEvent` callbacks;<br>**S6** timeouts: hook + interrupt, soft and hard abort, **`close()` deferral and lock order (P4)**, coroutine wrapper (F1), C-stack limit;<br>**S7** suite triage;<br>**S8** in-game: computer, turtle (move, dig, inventory), monitor, disk drive, modem, one generic-inventory peripheral. | **G1** (≈ ⅓ into M1): S1 is clean under `-Xcheck:jni`. This is also the calibration point for §5.5: compare the hours actually spent on M0 + S1 with the estimate. *If S1 has taken more than ~2x its estimate, re-scope before going on; what follows (concurrency, sandbox) is harder.* **G2** (exit): CC:T suite ≥ 1250/1407 under LuaJIT, every other failure tagged; `timeout_spec` "in loops" passes; the P4 test passes. | 18–28 d | 21–37 d |
| **M2 – Server-safe** | Complete P7 sandbox; P14 handle lifetime; global memory cap; patches 09–11 (including the address fix, patch 10); pause (F2); config, admin command, observability; WP H soak/fuzz/ASan; full CI matrix; licensing done. | **G3, the gate for sharing any build:** zero JVM crashes over N hours of fuzz and crash harnesses; the old JVM-killers pass as specs; `timeout_spec` pattern tests pass; suite clean under `-Xcheck:jni`; all P6/P7/P9/P14/P15 acceptance tests pass; the 24 h soak passes. **G4, pause timebox** (4 person-days): FairnessProbe ≤ 100 ms with `parallel`/`multishell` specs green. *If G4 fails:* ship without pause and document `computer_threads ≥ 2` along with the measured stall table. | 34–51 d | 38–67 d |
| **M3 – Release** | NeoForge on Windows and Linux x64; add-on pass; docs; release plumbing; perf regression suite. Release as "experimental", with an off-switch. Other platforms fall back to Cobalt. | The G3 conditions still hold on the release build; the add-on pass is clean. | 37–57 d (+20%: 44–68) | 41–73 d (+20%: 49–87) |
| **M4 – Later: more platforms and Fabric** (decisions 1 and 4) | Fabric module; linux-arm64; macOS x64/arm64; optionally, port 08's coroutine fast-path check to arm64 (WP B). | **G5, macOS:** the headless suite is green on `macos-15` and `macos-15-intel` with the JIT on, and one manual run with the official launcher shows the JIT speed-up. *If it fails:* macOS defaults to JIT-off or Cobalt. **arm64 Linux:** the preemption and C-stack harnesses plus the headless suite are green on `ubuntu-24.04-arm`. For the C-stack harness, green means 31/31 with the fast-path port, or without it the 27/4 result that `run-harnesses.sh` accepts (§6). **C-stack margin, macOS and arm64:** patch 08's margin is measured in a JDK 21 JVM with JNI upcalls on macOS arm64 (16 KiB pages) and on an aarch64 kernel with 64 KiB pages; a page size that has not been measured is not supported. | +0.5–1 d for Fabric (total 38–58, +20%: 45–70). The arm64/macOS CI is already budgeted in WP G; fixing real platform bugs is not budgeted. | +1–2 d for Fabric (total 42–75, +20%: 50–90). Same caveat. |

### 5.3 MVP path: a personal build first
**Scope:**
- NeoForge only; Windows x64 and Linux x64.
- JIT on; no pause (recommend `computer_threads = 2` in the README).
- Single-player and private servers only, labelled "do not run on public servers".

**Must still be included.** Each item below prevents a JVM crash, a wedged computer, or lost or corrupted data, even when you only run your own code:
- patches 01–08 (all done);
- soft and hard abort via the count hook + `luaJIT_interrupt`;
- `close()` deferral and lock order (P4);
- the protected boundary and iterative, depth-limited conversions (P5);
- the per-thread C-stack limit and the coroutine nesting limit (P8);
- Latin-1 8-bit strings;
- the `MethodResult` trampoline (turtles, inventories);
- the per-computer allocator cap;
- PRNG seeding (or rednet loses messages);
- C `bit32` (or SHA-style programs are slower than Cobalt);
- `debug.sethook`, `string.dump`, `jit`, `ffi`, `io`, `os` and `package` removed;
- the Cobalt fallback and a config off-switch.

**Deferred, documented as known issues:**
- complete P7 debug curation;
- P14 (files a program forgets to close stay open until reboot);
- the global memory cap and observability;
- pause;
- patches 09–11;
- Fabric, macOS and arm64.

A public release still requires G3.

### 5.4 Start here (for your profile)
You can skip C fundamentals. What is new to you is JNI, Java 9–21, the modern modding toolchain, and Lua's C API and LuaJIT internals. Your systems background is a direct asset for the riskiest parts of the design: the memory ordering of patch 01 on arm64 (relaxed stores against loads in a trace loop), the lock and `close()` protocol, the cache-line layout of the interrupt request fields, the allocator caps, and ASan/fuzzing.

1. **WP 0 (an evening or two).** Run `HeadlessRunner` and `CctSuiteRunner` with Cobalt. Then read the reference machine: CC:T's `CobaltLuaMachine.java` (329 lines), `ResultInterpreterFunction.java` (103), `VarargArguments.java` (246), and `reports/cct-contract.md` §3–§6 and §11. `CobaltLuaMachine` is CCPL-licensed: read it to understand the contract, but **don't copy from it** (§6).
2. **Java 8 → 21, only what this code uses (~1–2 days, mostly while reading CC:T and the scaffold):**
   - records (`MachineEnvironment` is one);
   - `var`, switch expressions, pattern matching for `instanceof`, text blocks, sealed interfaces;
   - `java.lang.ref.Cleaner`, which replaces `finalize()` and is how CC:T's `GlobalCleaner` closes leaked websockets (P14);
   - `VarHandle` / `java.util.concurrent.atomic` for the machine state;
   - JSpecify `@Nullable`.

   Gradle is the Groovy DSL with NeoForge's ModDevGradle: `runClient`, `runServer`, `neoforge.mods.toml`, `ModConfigSpec`. Your 1.7.10 experience covers the concepts; the tooling takes half a day to a day.
3. **JNI, from a C programmer's angle.** The rules that differ from ordinary C:
   - Local refs pile up until the native method returns, so use `PushLocalFrame`/`PopLocalFrame` per call. Class refs cached in `JNI_OnLoad` must be global refs. Method and field IDs are safe to cache.
   - Every call into Java can leave a pending exception. Check `ExceptionCheck` before the next JNI call.
   - `JNIEnv` is per thread. Cache the `JavaVM`, not the env. CC:T moves computers between worker threads.
   - `Get*ArrayCritical` holds off the GC: no JNI calls and no Lua calls while you hold it.
   - Never `longjmp` across a JVM frame. This is the JNI version of the async-signal-safety rule you already know.
   - Always run with `-Xcheck:jni`. Model code: the lean paths in `artifacts/bench/hostcall/jnibench.cpp`. **Do not start from** the 1.20.1 or 1.14.4 `main.cpp`.
   - Java's FFM API (Panama) would avoid JNI, but it is a *preview* feature in Java 21 (final only in Java 22). Minecraft 1.21.1 runs on Java 21, so it is not an option.
4. **The Lua 5.1 C API** (manual §3–§4): stack discipline, `lua_pcall`/`lua_cpcall`, `lua_resume`/`lua_yield`, registry refs. Lua errors are `longjmp`s. Treat every API call that can raise like a function that may not return: never hold a lock, a JNI resource or a half-built structure across it. Exercise: change `artifacts/harnesses/preempt/harness.c` (or `harness_posix.c` with the `win32_on_posix.h` shim) to run a coroutine and report errors.
5. **Native debugging in a JVM:** read `hs_err_pid*.log`. Make debug builds with `-g -DLUA_USE_APICHECK -DLUA_USE_ASSERT`, use gdb under WSL, and add an ASan build of `libcclj`. A JVM cannot unload a native library, so every change needs a JVM restart; the headless boot takes ~4 s.
6. **Later, as needed:** LuaJIT internals: `lj_record.c`/`lj_trace.c` for patch rebases, `lib_string.c` for 07, `vm_*.dasc` for 08's fast path.

**Code to start from:**
- `scaffold-multiloader`;
- `luajit-patches/build-patched-luajit.sh` and `verify-patch-stack.sh` (since M0: `natives/luajit-patches/` and `natives/tests/run-harnesses.sh`);
- `lua/cclj_prebios.lua`;
- `harnesses/runtime/allocwrap.c` (memory cap);
- `pause-prototype/cclj_co.c` (coroutine wrapper, interrupt hook);
- `headless-runner/`.

### 5.5 Calendar time at 6–8 hours a week
One person-day is 8 focused hours. At 6–8 hours a week, one person-day of work is about 1–1.3 calendar weeks.

**Working alone:**

| Target | Person-days | Calendar at 6–8 h/week |
|---|---|---|
| M1, personal MVP | 21–37 | ≈ 5–11 months |
| M2, server-safe | 38–67 | ≈ 9–20 months |
| M3 + M4 (every work package), +20% contingency | 50–90 | ≈ 1–2.3 years |

**With Claude doing most of the implementation.** The working split would be:

- **Claude drafts:**
  - Gradle/NeoForge setup and the Java machine;
  - JNI glue, C host code, harnesses and specs;
  - CI YAML, docs, and upstream-backport triage.

  It also runs the builds, the headless suite, the harnesses and the benchmarks on this machine (Windows, plus Linux under WSL). Long build-and-test sessions can run while you are away.
- **Your hours go to:**
  - design decisions;
  - **line-by-line review of every safety-critical C and JNI change** (protected boundary, sandbox, lock protocol, conversions, allocator);
  - in-game testing;
  - debugging that needs your judgement;
  - whatever needs hardware we don't have here (arm64, macOS).

How much of the person-day figure is still your time is an **assumption, not a measurement**. Careful review of safety-critical native code is real work, and AI-drafted C/JNI that is wrong crashes the server just like hand-written code. A reasonable guess is that your hours come to **35–60%** of the person-day figures.

| Target | Your hours (35–60% of person-days × 8 h) | Calendar at 6–8 h/week | Midpoint |
|---|---|---|---|
| M1, personal MVP | ≈ 57–175 h | ≈ 2–7 months | ≈ 108 h, ~3.5 months |
| M3 + M4 (every work package), +20% contingency | ≈ 141–429 h | ≈ 4–16 months | ≈ 265 h, ~9 months |

**Calibrate at gate G1.** Track your actual hours for M0 and slice S1, then rescale these figures before committing to M2.

---

## 6. Risks
- **Every native bug is a server crash,** and a sandbox escape is remote code execution on every server that runs the mod. CC:T's maintainer rejected native Lua for exactly this reason. Mitigations: the protected boundary, fuzzing, ASan, `-Xcheck:jni` in CI, the Cobalt fallback, the live kill-switch and quarantine, `SECURITY.md` with a 90-day disclosure policy, and an incident playbook (`/ccluajit disable` → patch release → advisory).
- **CC:T internals are not public API.** `ILuaMachine` lives in core, and `ServerContext.luaMachine` is `@VisibleForTesting`. 1.115 → 1.121 already changed `IArguments.escapes()` in a binary-incompatible way. Mitigations: the contract probe with Cobalt fallback, a ROM parse check, a weekly CI canary against new CC:T releases, and warnings for other mods that replace the factory or mixin into `CobaltLuaMachine` (DESIGN §1).
- **Licensing.** The repo and mod are MIT, but CC:T's `CobaltLuaMachine`, `ILuaMachine`, `bios.lua` and 80 of the 105 ROM files are **CCPL** (non-commercial, share-alike for copied code). They cannot be copied into an MIT codebase. The machine must be a clean reimplementation from the behavioural contract; say so in `CONTRIBUTING`. Code adapted from CC:T's MPL-2.0 files (`VarargArguments`, `ResultInterpreterFunction`, the test-rom, `ComputerTestDelegate`) stays in separate MPL-licensed files with their headers. *(Done in this folder: the benchmark that embedded the ROM is now generated from the CC:T jar by `artifacts/bench/gen_b10_compile.py` instead of being stored.)*
- **LuaJIT becomes a maintained fork.** Staying before the v3.0-syntax merge means cherry-picking upstream fixes by hand: budget 0.5–1 d/month and keep a backport list (DESIGN §2). Patch 06 already conflicts on HEAD.
- **arm64 is only partly tested.** The first CI run ([run 42422693045](https://github.com/mindbound/CCLuaJIT/actions/runs/42422693045), 2026-11-07 UTC) ran the harnesses on linux-aarch64 and macOS arm64 in the informational jobs, without a JVM.
  - Preemption (patch 01), pattern interrupts, compat, syntax and fixcheck give the x86_64 results on both.
  - The C-stack harness runs on Linux only (macOS skips it). It shows the expected gap of patch 08's x64-only fast-path check: the 4 cases that recurse only through the stock `coroutine.resume`/`wrap` overflow the native stack (SIGSEGV), and the other 27 complete. `natives/tests/run-harnesses.sh` accepts exactly that result on Linux arm64 without the fast-path port, and nowhere else.
  - Sandboxed code does not depend on that fast path once the coroutine wrapper (DESIGN §3.3, F1) replaces `resume`/`wrap`, so porting the check to `vm_arm64.dasc` stays optional (WP B).
  - Still open: patch 01 inside a JVM on ARM, and the C-stack margin on 16 KiB / 64 KiB pages. The margin depends on HotSpot's guard zones, so it needs a JVM on such hardware (M4 gate). The harness-only CI jobs cannot measure it, and a 64 KiB-page aarch64 kernel needs a machine of its own.
- **macOS:** the JIT under the hardened runtime (`MAP_JIT`) is unverified on Apple Silicon with Mojang's bundled Java. CraftOS-PC disables the JIT there. Upstream's `LUAJIT_ENABLE_OSX_HRT` and the macOS fix `716f74a6` are the starting points. The fallback is JIT-off or Cobalt on macOS (G5).
- **Antivirus and moderation friction.** An unsigned, statically linked DLL containing LuaJIT, extracted into a game folder, closely resembles current LuaJIT-based malware loaders. Mitigations: reproducible builds, published hashes, a VirusTotal scan in CI, a PE version resource, optional OSS code signing (e.g. SignPath Foundation), a clear "contains native code" note on the mod page, and a quarantine hint in the log.
- **Fairness without pause.** If pause slips, one busy computer delays all others by seconds on CC:T's default single thread. Even with pause, pauses inside non-yieldable C callbacks are deferred (measured up to ~0.5 s without checkpoint wrappers).
- **Programs diverge slightly from stock CC:T** (P13). Anything a player writes on CCLuaJIT should stay portable, which is why the pin is before the v3.0 syntax.

---

## 7. Decisions
The answers recorded on 2026-11-07 are marked **[answered]**. Items still open say what they are waiting for.

0. **[answered] Your profile and time.** About 10 years of systems C (DPDK-class), little JNI, Java at the Java 8 level, and 6–8 hours a week. That gives the "You" column and §5.5. You are the C reviewer yourself. JNI correctness is covered by `-Xcheck:jni`, the ASan build, the crash harnesses and review. Before G3, still get an independent security review of the sandbox (P6/P7, DESIGN §3.5): a fresh pair of eyes, for example a separate review session, plus a public call for testers.
1. **[answered] Loader: NeoForge first** (your modpack is NeoForge-based); Fabric comes after M2, if at all.
2. **[answered] First audience: personal.** The MVP after M1 is the first target, and nothing goes public before G3.
3. **[answered] Pause: include it in M2** (default accepted). It is now prototyped and cheap (F2: 1–1.5 person-days experienced, 1–2 for you). If gate G4 fails, ship without it and recommend `computer_threads ≥ 2` (measured worst lateness: 3.7 s with 1 thread, 14 ms with 2, against 76 ms on Cobalt).
4. **[answered] Platforms: Windows x64 and Linux x64 only for now.** You have no Mac. arm64 Linux and macOS come later, when you can support them. Until then, G0 and G5 are reduced as described in §5.2, and those platforms fall back to Cobalt (the native loader finds no library for them).
    Test machines:
    - Windows 11 (development, in-game client).
    - WSL **AlmaLinux 8.10**: glibc 2.28, the glibc floor and the same base as `manylinux_2_28`. GCC 8.5.
    - WSL **Ubuntu 26.04**: glibc 2.43, GCC 15.2. A NeoForge dedicated server can run here headless.
    - **CachyOS** on dual boot: native Linux, for an in-game Linux client check and benchmarks without WSL noise. Minecraft is not installed yet; copy the installation over when needed.
    - JDK 21 is installed in both WSL distros (2026-11-07): Ubuntu 21.0.12 is the default `java`; Alma 21.0.13 (`/usr/lib/jvm/java-21-openjdk`) is the default `java` and `javac`. Scripts still set `JAVA_HOME` explicitly.
5. **[decided by Claude, delegated] LuaJIT base: `cclj/v2.1` = upstream `14d8a7a2` + 17 cherry-picked upstream fixes** (DESIGN §2, `artifacts/luajit-patches/BACKPORTS.md`).
    - `14d8a7a2` is the newest v2.1 commit without the v3.0 syntax.
    - The picks fix 5 JIT/VM bugs that sandboxed code can reach, confirmed on Windows and Linux x64.
    - All harnesses pass, and the new syntax stays rejected.
    - Tracking upstream HEAD with a syntax gate also works (0 parse differences over 113,872 inputs). It is kept as a ready kit, not adopted, because upstream is actively rewriting its parser (`reports/pin-head-gate.md`).
    - You will need a LuaJIT fork on your GitHub account to host the branch.
6. **[answered] `_VERSION` and self-identification: accepted as recommended.** This was your instinct, backed by a survey of CC:T, CraftOS-PC, 897 CC source files and the libraries CC users vendor (`reports/version-survey.md`):
    - **`_VERSION = "Lua 5.2"` stays exactly as is.** No CC program looks for LuaJIT there. Every "compatible but distinct" string changes at least one real check: `== "Lua 5.2"` in sha2.lua and Phoenix, `:sub(-3)` in compat53, `tonumber(_VERSION:sub(5))`.
    - **A minimal `jit` table with only `version` and `version_num`**, also available through `require "jit"`. This is how CC programs already detect LuaJIT (`if jit`). AUKit and sanjuuni's video player then pick chunk sizes that fit LuaJIT's 8,000-value result limit; without the table they fail on CCLuaJIT. No functions are exposed, and the real `jit` library stays removed.
    - **A trailing `_HOST` tag** for humans and logs, for example `ComputerCraft 1.121.0 (Minecraft 1.21.1) (CCLuaJIT 0.1.0; LuaJIT 2.1.…)`. It has the same shape as UnBIOS's existing " (UnBIOS)" suffix, which the 20 real `_HOST` parsers checked already cope with.
    - **`require "bit"` returns LuaJIT's BitOp**; the global `bit` stays CC's stub. The widely vendored `sha2.lua` needs this, because it detects LuaJIT by a VM quirk and then requires `bit`.
7. **[answered] `debug.sethook`: removed** (default accepted). The ROM never uses it; CC:T's "within debug hooks" test and Lua profilers break without it. A virtual `sethook` needs the user's hook to run outside hook context (otherwise it can't be interrupted): 1–2 d plus design risk.
8. **[answered] Per-computer memory cap: 64 MiB** (default accepted; one reviewer proposed 128 MiB). The lower default keeps every single linear C operation and full GC well under 0.5 s; a 368 MB heap took 0.19–0.25 s to collect. It also keeps many computers within a container's memory budget. Exceeding the cap raises a Lua "not enough memory" error, never a crash. The global cap defaults to min(1 GiB, 25% of `-Xmx`).
9. **[answered] Branch: `1.21.1_CCT`, created from `main` (`72bc0d9`) by you on GitHub.** Almost nothing from `1.20.1_CCT` is reused. Commits follow your review workflow: Claude drafts the message, and you review and commit.
10. **[answered] Identity and credit.** The new branch is your work and you will maintain it, so it carries your identity:
    - Java package `io.github.mindbound.cclj` and Maven group `io.github.mindbound`;
    - `authors = "mindbound"`.

    Original authorship stays visible, both for provenance and as plain courtesy:
    - Keep the existing `Copyright (c) 2024-2025 Vereena Inara` line in `LICENSE`, and add your own line above or below it. MIT requires the original notice to stay with copies of the work, and the branch descends from that repository.
    - Add a "Credits" section to the README that links to the original repository and names its author.
    - Fill in `credits = "Original CCLuaJIT by Vereena Inara (vereena0x13)"` in `neoforge.mods.toml` (and `contributors` in `fabric.mod.json` later).

    *Recommended:* keep the mod ID `ccluajit` and the name CCLuaJIT, for continuity. The original never shipped for 1.21.1, so nothing clashes. If the original has a project page on Modrinth or CurseForge, publish under a new project that links back to it; you cannot take over someone else's page.
11. **[answered] CC:T dependency range: `>= 1.121.0` with no upper bound** (default accepted), plus the runtime contract probe and Cobalt fallback. The alternative is `[1.121.0,1.122)`: it blocks server start after every CC:T update until CCLuaJIT ships a new release.

---

## 8. What's in this folder
- [DESIGN.md](DESIGN.md): the technical design (mod layer, native layer, host core, JNI bridge, observability and config).
- `reports/`: investigation and review reports, with methodology, commands, outputs and `file:line` evidence. Paths inside them point to the original scratch workspace, which is gone after this session; the reusable parts were copied into `artifacts/`.
  - `cct-contract.md`: the CC:T machine contract; §11 is the implementation checklist.
  - `rom-compat.md`: Lua dialect gaps, the shim, and the patches.
  - `code-audit.md`: old-code defects and the salvage assessment.
  - `luajit-runtime.md`: experiments on LuaJIT C-API behaviour.
  - `preempt-patch.md`: preemption mechanism, patch, and host protocol (§7).
  - `perf-bench.md`: benchmarks and host-call costs.
  - `port-mechanics.md`: build/loader port, native packaging, CI.
  - `prior-art.md`: CraftOS-PC Accelerated, LuaJIT security/CVEs, CC:T maintainer views, loader statistics.
  - `verify-*.md`: the adversarial re-checks of each of the above.
  - `review-accuracy.md`, `review-completeness.md`, `review-sequencing.md`: the three reviews of plan v1. Their section numbers refer to v1.
  - `proto-pause.md`, `proto-patterns-cstack.md`: the prototype reports.
  - `pin-backports.md`, `pin-head-gate.md`: the evaluation of the two LuaJIT-base options (decision 5).
  - `version-survey.md`: how CC programs detect `_VERSION`, `_HOST` and LuaJIT (decision 6).
- `artifacts/luajit-patches/`:
  - patches 01–08, README, `build-patched-luajit.sh`, and `verify-patch-stack.sh` (builds and runs every harness). This is the evaluation record; since M0 the canonical copy is `natives/luajit-patches/`, and `natives/tests/` holds the harnesses CI runs;
  - `BACKPORTS.md` and `backports/`: the `cclj/v2.1` base, 17 upstream fixes as `git am` patches; `backports-full/` adds the 9 deferred performance commits;
  - `tools/`: `syntax_probe.lua`, `fixcheck.lua`, the Linux sanity script, the outcome normaliser;
  - `results/`: verification logs per base;
  - `head-kit/`: the alternative HEAD + syntax-gate setup and the differential parse test.
- `artifacts/pause-prototype/`: the coroutine wrapper and pause token protocol (`cclj_co.c`), a CC:T-like host, 7 test suites, and results.
- `artifacts/headless-runner/`: `HeadlessRunner`, `CctSuiteRunner` and `FairnessProbe`, with Cobalt reference results.
- `artifacts/lua/`: `cclj_prebios.lua` (the compat shim, fuzz-checked against Cobalt) and `compat_check.lua`.
- `artifacts/scaffold-multiloader/`: a compiling common/fabric/neoforge project, `NativeLoader`, the native build script, and a CI YAML draft.
- `artifacts/harnesses/`:
  - `preempt/`: 25 loop shapes + 3 known-hang cases; the POSIX shim `win32_on_posix.h` and `harness_posix.c`;
  - `patterns-cstack/`: patch 07/08 harnesses, fuzzers and results;
  - `runtime/`: LuaJIT runtime probes and `allocwrap.c`;
  - `jni-crash/`: the old-code JVM-killer harness;
  - `review-probes/`: the completeness review's probes (unpack limits, quadratic `find`, `debug.getlocal` escape, PRNG).
  - `version-survey/`: the `_VERSION`/`_HOST`/`jit` probes, result-limit probes and the Cobalt runner.
- `artifacts/bench/`: the CC-style benchmark suite (`b10_compile.lua` is generated by `gen_b10_compile.py`), the Cobalt-via-CC:T runner, the JNI host-call benchmark, and the result summary.

---

## Changes from v1
- **Corrected:**
  - the speed-up: 8.1x with correct `bit32`, not 8.9x; C `bit32` is required, not optional;
  - the JVM-killer list and its causes;
  - the LuaJIT pin distance: 127 commits, not 180;
  - the patch overhead claims: hook interpreter cost, patch 05 `tostring` cost;
  - the native-loader path;
  - the 1.14.4 deadlock: code reading, not a harness; turtles not affected;
  - the CraftOS-PC comparison baseline;
  - the sort figures;
  - the CHECKHOOK rationale.
- **New results:**
  - patches 07 and 08 written and tested;
  - the pause token protocol prototyped and tested;
  - the headless CC:T runner built, reproducing CC:T's suite (1390/1407 on Cobalt);
  - fairness measured;
  - the full 01–08 stack re-verified on both `19878ec0` and `3c4f9fe2`.
  - LuaJIT base evaluated: `cclj/v2.1` chosen; the HEAD + syntax-gate kit is kept.
- **New problems:** P14 (Java object lifetime), P15 (fixed PRNG seed, address disclosure), P16 (server-wide resources and operations). P2, P4, P7 and P9 were widened (plain `find`, lock order, `getlocal`/`getinfo`/`getfenv`/`getmetatable`, global cap).
- **Plan structure:**
  - WP 0 (headless runner) first;
  - P4 moved into M1;
  - WP F split into F1 (nesting limit) and F2 (pause);
  - new WPs I–L (docs, release/licensing, config/observability, add-on pass);
  - stop/go gates G0–G5;
  - an MVP path, a learning path, and per-problem acceptance tests;
  - decisions now have defaults.
- **Licensing fix in this folder:** the ROM-embedding benchmark was replaced by a generator.

### v2.1 (decision 0 answered)
- The generic "learning" estimate column was replaced by one for your profile.
- §5.2 now gives cumulative person-days.
- New §5.5 gives calendar time at 6–8 h/week, alone and with Claude implementing.
- §5.4 is rewritten for a strong C programmer who is new to JNI and Java 9–21.
