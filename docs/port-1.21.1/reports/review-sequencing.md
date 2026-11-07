# Plan review: sequencing, estimates and newcomer path (PLAN.md v1)

*Written on 2026-11-07 by an independent review/prototype agent. Section references (§, P-numbers, WP letters) point to PLAN.md **v1**; PLAN.md v2 has incorporated or answered every item (see its change log). Paths under `scratchpad/work/...` refer to the session scratch workspace; the reusable files were copied into `../artifacts/`.*

## Verdict

The technical content of PLAN.md is sound and its WP sums add up (26–43 / 61–103 days match the table). The execution order is the weak part. It fits an experienced C/JNI developer, not this user, who knows MC modding well but is new to 1.21.1, CC:T internals, and probably JNI and LuaJIT internals. The biggest problem is that the cheapest, highest-leverage tool, a headless CC:T runner, sits inside the last work package (WP H). I built and ran one to check feasibility: about 250 lines of plain Java against the CC:T 1.121.0 core jar, with no Minecraft, Loom or EULA. It boots the real bios and ROM from the core jar in 1.6 s. It exercises timers, char/key events, a mainThread=true method (MethodResult, MainThread and task_complete), 8-bit file I/O and shell programs. It plugs in the scaffold's LuaJITMachine through `--factory`. A second ~200-line runner drives CC:T's own mcfly suite (tag v1.21.1-1.121.0, 1,407 tests) in 23 s. Cobalt baseline with HTTP disabled: 1390 pass, 12 pending, 5 HTTP errors. That gives a numeric progress metric for every milestone, and it should be WP 0. Two dependency errors also need fixing. First, close() deferral (P4) is scheduled for M2, but every hard abort in M1 makes CC:T's Monitor thread call close() while handleEvent is still running. Second, the C-stack nesting limit (P8, M2) depends on the coroutine wrapper, which sits in WP F (pause, M3). The plan also lacks: measurable stop/go gates; a newcomer M1 estimate, a contingency line and a part-time calendar conversion; and a defined MVP. I measured the case for deferring pause: without PAUSE, another computer's 50 ms timer is up to 3,717 ms late on CC:T's default single thread, but only 14 ms late with computer_threads=2 (Cobalt with pause: 76 ms). Other gaps: no early arm64/macOS check, although patch 01 has never run on arm64 (I ported the preempt harness to POSIX and got 25/25 on Linux x64, so a CI job is about half a day). The 3c4f9fe2 pin misses a macOS fix from Oct 2026. There are no WPs for docs, release, config or addon compatibility. The decision list has no recommended defaults, and it is missing the user's own skill and time budget, Mac access, debug.sethook (CC:T's own suite tests it), the memory cap default, and test-rom sourcing. Each issue below has paste-ready text.

## 1. [must-fix] §5 Work plan (WP H) / Milestones M0–M1

**Problem.** The headless CC:T runner is buried in WP H (the last WP, 5–8 d bundled with fuzzing and in-game tests), but it is cheap, proven feasible, and the main iteration and acceptance tool for a newcomer. It should be WP 0 and part of M0. Without it, every machine change needs a Minecraft dev-client relaunch and manual clicking, and progress has no numeric measure.

**Evidence.** Built and run in <scratch>\work\review-sequencing\headless\src\HeadlessRunner.java (real Computer + ComputerThread(1) + MainThread + JarMount ROM + bios from the core jar). Output in ...\headless\out-cobalt-smoke.txt: boots to the shell in 1.60 s (3.9 s JVM total); version, sleep, mainAdd(2,3)=5 through @LuaFunction(mainThread=true), read() fed by queued char/key events, binary fs round-trip, bit32 and `ls rom` all OK; context closed cleanly. ...\headless\src\CctSuiteRunner.java runs CC:T's own test-rom (sparse checkout of tag v1.21.1-1.121.0 = 380c6fc6, projects/core/src/test/resources/test-rom, 413 files, 80 *_spec.lua) in 23.4 s: 1407 registered, 1390 pass, 12 pending, 5 error (all http_spec, from disableHttp) -> ...\headless\suite-cobalt.tsv. With `--factory gay.vereena.cclj.computer.LuaJITMachine` (the scaffold stub) it shows 'Error running computer / CCLuaJIT: LuaJIT machine not implemented yet' and exits with code 126. Wiring evidence: ComputerContext.java:93-95,121-124,147-152,161-166,189-191,211-220; GlobalEnvironment.java:15-45; ComputerEnvironment.java:57-88; Computer.java:62-71,101-103,117-119,148-165,184-186; ComputerExecutor.java:159-166 (default APIs), 334/342 (hdd + rom mounts), 370-375 (MachineEnvironment), 60/279 (event queue cap 256); Terminal.java:33-42,233; JarMount.java:30; WritableFileMount.java:35; MainThread.java:69,112; NoWorkMainThreadScheduler.java:24-25 (throws on main-thread work, so pass a real MainThread). ROM and bios ship inside cc-tweaked-1.21.1-core-1.121.0.jar (data/computercraft/lua/bios.lua, data/computercraft/lua/rom/...). Classpath = core + core-api + cobalt 0.9.9 + guava 32.1.2 + failureaccess + fastutil 8.5.12 + slf4j 2.0.9 (verify-cct-contract cp.txt). CC:T's own equivalent is ComputerTestDelegate.java:101-127,349-494 (JUnit-bound; mine is JUnit-free).

**Suggested text.**

### WP 0 – Headless CC:T runner and conformance oracle (do first; exp 0.5–1 d, newcomer 1–2 d)
Copy `scratchpad/work/review-sequencing/headless/src/{HeadlessRunner,CctSuiteRunner,FairnessProbe}.java` into `common/src/test/java/gay/vereena/cclj/headless/`, and vendor CC:T's `test-rom` (tag `v1.21.1-1.121.0`, `projects/core/src/test/resources/test-rom`, MPL-2.0 headers kept) into `common/src/test/resources/test-rom/`.

Wiring (no Minecraft classes, no Loom, no EULA):
```java
GlobalEnvironment g = new GlobalEnvironment() {                 // GlobalEnvironment.java:15-45
  public String getHostString() { return "ComputerCraft 1.121.0 (headless)"; }
  public String getUserAgent()  { return "computercraft/1.121.0"; }
  public Mount getRomMount()    { return new JarMount(coreJar, "data/computercraft/lua/rom"); } // JarMount.java:30
  public InputStream getLuaBios(){ return zipEntry(coreJar, "data/computercraft/lua/bios.lua"); }
};
ComputerEnvironment ce = /* getDay, getTimeOfDay, MetricsObserver.discard(),
   createRootMount() -> new WritableFileMount(tmpDir, 1_000_000) */;   // put startup.lua in tmpDir
var mainThread = new MainThread();                               // default NoWork scheduler throws on mainThread=true methods
var ctx = ComputerContext.builder(g).computerThreads(1)          // real ComputerThread, same as CC:T default config
    .mainThreadScheduler(mainThread).luaFactory(LuaJITMachine::new) // same type as ServerContext.luaMachine
    .disableHttp().build();
var term = new Terminal(51, 19, true);
var c = new Computer(ctx, ce, term, 0); c.addApi(new TestAPI());
c.turnOn();                                   // only sets a flag; the start happens inside tick()
every 50 ms: c.tick(); mainThread.tick();     // drives timers, APIs and main-thread tasks
input:  c.queueEvent("char", new Object[]{"a"}); c.queueEvent("key", new Object[]{257, false});
output: term.getLine(y).toString();  stop: c.shutdown(); ctx.close(2, SECONDS) must return true
```
Gradle (`common/build.gradle`): `testImplementation` cc-tweaked core + core-api (brings in cobalt, guava, fastutil, netty), `testRuntimeOnly 'org.slf4j:slf4j-simple:2.0.9'`, and a `headlessSuite` JavaExec task running `CctSuiteRunner --core-jar <core jar> --test-rom src/test/resources/test-rom --factory <machine class>` with `-Xcheck:jni`.

**Done when:** the Cobalt baseline reproduces (1407 tests, 1390 pass, 12 pending, 5 HTTP errors with HTTP disabled, ~25 s), and the scaffold stub shows its error on the dumped terminal. From then on, every milestone reports `CC:T suite: N/1407 pass under LuaJIT`, plus an expected-failures file that cites a P13 item for each entry.

## 2. [must-fix] §3 P4 / §5 Milestones M1 vs M2

**Problem.** Close safety (P4) is listed under M2 ('close safety'), but M1 requires 'soft and hard aborts work'. Every hard abort makes CC:T's Monitor thread call machine.close() while handleEvent is still running on the worker. A close() that frees the lua_State is a use-after-free JVM crash, so the first M1 timeout test would crash the JVM.

**Evidence.** ComputerThread.java:529-531 (checkRunners: `executor.timeout.hardAbort(); executor.worker.abortWithTimeout();`) -> ComputerExecutor.java:236-237 (`abortWithTimeout -> immediateFail(ABORT_WITH_TIMEOUT)`) -> :254-256 (`if (machine != null) machine.close();`) on the Monitor thread. verify-cct-contract.md CT-1/CT-7 measured it: '8.66s [Monitor-0] listener: hard=true ; close() call #1 (handleEvent running=true)', then about 30 more close() calls. CobaltLuaMachine.close() (CobaltLuaMachine.java:159-163) only sets isDisposed, calls state.interrupt() and removes the listener, which is the pattern to copy.

**Suggested text.**

Move P4 into M1. Add to the M1 exit criteria: "close() is idempotent and only sets flags, requests an interrupt under the close mutex and removes the TimeoutState listener. The owning worker thread frees the VM. Test: in the headless runner, `while true do end` reaches hard abort, close() runs from `ComputerCraft-Computer-Monitor-0` while handleEvent is still running, the JVM survives, `ComputerThread.workerCount` is unchanged, and `ctx.close(2, SECONDS)` returns true." Remove 'close safety' from M2.

## 3. [must-fix] §3 P8 / §4.3 Pause token protocol / WP F / Milestones M2–M3

**Problem.** The C-stack safety in M2 (P8) depends on 'a coroutine-nesting limit in the coroutine.resume wrapper'. In §4.3 that wrapper belongs to the pause token protocol (WP F, scheduled for M3, and possibly deferred by Decision 3). Deferring pause would therefore silently drop the nesting limit, a JVM-crash guard.

**Evidence.** PLAN.md §3 P8: 'Design: coroutine-nesting limit in the coroutine.resume wrapper ...'. §4.3: 'The sandbox replaces coroutine.resume/wrap (C implementation) ... The same wrapper enforces a coroutine-nesting limit.' §5: WP F = 'Pause token protocol (P10)'; M2 = '... C-stack safety ...'; M3 = 'Pause/time-slicing'. Decision 3 offers to defer pause.

**Suggested text.**

Split WP F:
- **F1 (part of WP C, M2, exp 0.5–1 d / newcomer 1–2 d):** C replacements for `coroutine.resume`, `coroutine.wrap` and `coroutine.status`, with a nesting-depth limit (error, never crash). Also the Lua `table.sort` and function-replacement `gsub` from WP E. Must pass CC:T `spec/lua/coroutine_spec.lua` (yield inside gsub, sort comparator, metamethods, pcall/xpcall).
- **F2 (M3 or deferred, exp 2–4 d / newcomer 6–10 d):** pause-token logic added to the F1 wrapper. Pause stays optional; the nesting limit does not.

## 4. [must-fix] §5 Milestones (new 'Stop/go gates' subsection)

**Problem.** The plan names risks such as pause, macOS and JNI correctness but has no decision points with measurable pass/fail criteria. A newcomer cannot tell when to push on, cut scope or ask for help. Several gates can be measured today against Cobalt baselines.

**Evidence.** Baselines measured here. CC:T suite under Cobalt: 1390/1407 pass (suite-cobalt.tsv). FairnessProbe (...\headless\src\FairnessProbe.java; computer A does 3 s of CPU without yielding, computer B measures lateness of 60 sleep(0.05) calls): Cobalt with PAUSE, worst 76 ms, mean 13 ms (fair-pause.txt). A machine that never returns PAUSE, 1 thread: worst 3,717 ms, mean 74 ms (fair-nopause.txt). Never-PAUSE with computer_threads=2: worst 14 ms (fair-nopause-2threads.txt). Preempt harness ported to POSIX (...\posix\win32_on_posix.h + harness_posix.c), combined patched LuaJIT rebuilt in WSL Ubuntu gcc 15.2: 25/25 OK plus the 3 expected X_ hangs (...\posix\preempt-linux-x64.txt). preempt-patch.md:540,543: arm64 never built or run; ARM memory ordering 'reasoned, not tested'.

**Suggested text.**

### Stop/go gates
- **G0 – toolchain and oracle (end of M0):** patched LuaJIT (01–06) and libcclj build on Windows and Linux x64. NativeProbe loads in a JDK 21 JVM. WP 0 reproduces the Cobalt baseline (1390/1407). The preempt harness passes 25/25 on Windows, Linux x64, linux-arm64 (CI `ubuntu-24.04-arm`) and macOS arm64 (CI `macos-15`), using the POSIX shim. *If arm64 or macOS fails:* ship those platforms as JIT-off (`luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF)`, ~3.7x Cobalt) or Cobalt fallback now, and stop spending time on them.
- **G1 – vertical slice (about 1/3 into M1):** headless, LuaJITMachine runs a tiny bios that calls one API method, receives one event, raises one Lua error that comes back as `MachineResult.error`, and is clean under `-Xcheck:jni`. *If a newcomer has not reached this after ~3 weeks:* re-scope or bring in a C/JNI reviewer, because what remains (concurrency, sandbox) is harder.
- **G2 – M1 exit:** the real ROM boots headless, `smoke.lua` passes (including the mainThread method), CC:T suite ≥ 1250/1407 with every other failure tagged in `expected-failures.txt`, `timeout_spec` 'in loops' passes, and the hard-abort and close test passes (see P4).
- **G3 – M2 exit, the gate for sharing any build:** zero JVM crashes over N hours of fuzz and crash harness runs; the 7 old JVM-killers pass as regression specs; `timeout_spec` pattern tests pass (patch 07); suite clean under `-Xcheck:jni`; sandbox tests pass (bytecode refused, `debug.getupvalue` cannot reach native `load`, no registry leak, `debug.sethook` cannot disable the timeout); memory-cap test passes.
- **G4 – pause timebox (exp 4 d / newcomer 2 weeks):** prototype in the C harness first. Pass = coroutine, parallel and multishell specs green with pause on, and FairnessProbe worst lateness ≤ 100 ms (Cobalt: 76 ms). *Fail:* ship never-PAUSE and document `computer_threads` ≥ 2 (measured: 3,717 ms with 1 thread, 14 ms with 2).
- **G5 – macOS:** the headless suite is green in CI on `macos-15` and `macos-15-intel` with JIT on, and one manual run with the official launcher (Mojang's bundled Java) confirms a JIT loop speed-up. *Fail:* macOS JIT-off or Cobalt by default.

## 5. [must-fix] TL;DR 'How much work' / §5 table and Milestones

**Problem.** The estimates are internally consistent, but they serve this user poorly. (1) There is a prototype time for experienced developers ('≈ 2–3 weeks') but none for the newcomer column, which is this user. (2) No milestone has newcomer timing. (3) There is no contingency, although 'any bug can crash a server'. (4) Everything assumes full-time work. (5) The user's actual profile is mixed: MC/Java modding experienced, C/JNI unknown, so neither column fits.

**Evidence.** PLAN.md:14 gives the prototype time only for experienced developers. §5 M1 is '≈ 2–3 weeks cumulative, experienced'; M2/M3 have no timing. Summing the M1 content from the WP table (experienced): A partial 1–2 + B build 0.5–1 + C partial 3–5 + D 6–9 + E 1–2 = 11.5–19 d (2.3–3.8 weeks), which is at or above the stated 2–3 weeks. The newcomer equivalent is ≈ 27–44 d. Lane estimates: code-audit.md:443 (17–27 / 38–66 excluding _ENV and preemption R&D), port-mechanics.md:206, cct-contract.md:501-505 ('likely 1-3 months for a newcomer' for the native hard parts). port-mechanics.md:205 'Release plumbing' (0.25–1 d) was dropped from the PLAN table.

**Suggested text.**

### Estimates for a mixed profile (MC/Java experienced, new to C/JNI/LuaJIT)
| WP | person-days |
|---|---|
| 0 headless runner | 1–2 |
| A NeoForge first, Fabric later | 2–4 (+1–2 Fabric) |
| B LuaJIT stack | 5–10 |
| C host core | 14–22 |
| D JNI bridge + machine (Java side familiar) | 10–18 |
| E prelude | 3–5 |
| F1 coroutine wrapper / F2 pause | 1–2 / 6–10 |
| G CI | 5–10 |
| H testing (runner already built) | 8–13 |
| I docs | 2–3 |
| J release | 2–4 |
| K addon compatibility | 1–2 |

**Total ≈ 60–107 person-days. Add 20 % contingency: ≈ 72–128 days ≈ 3.5–6 months full-time.**

Cumulative (full-time, newcomer):
- M0 ≈ 1.5–2.5 weeks
- M1 / personal MVP ≈ 7–11 weeks
- M2 ≈ 12–17 weeks
- M3 ≈ 17–26 weeks

Experienced developer: M0 ≈ 1 week, M1 ≈ 3–4 weeks, M2 ≈ 5–7 weeks, M3 ≈ 6–10 weeks.

**Calendar:** at 10 h/week, multiply by about 4 (the MVP becomes about 6–10 months).

## 6. [should-fix] §5 (new 'MVP path') and §7 Decisions 1–4

**Problem.** There is no defined smaller first deliverable, so the first useful build arrives only after M2 or M3. A personal-use MVP (NeoForge-only, Windows and Linux x64, JIT on, never-PAUSE) would deliver value months earlier. The plan should say which safety items are non-negotiable even for personal use.

**Evidence.** Loader share: NeoForge 1.72M vs Fabric 73k Modrinth downloads (PLAN §7). Never-PAUSE is legal under the contract (verify-cct-contract CT-3), and its cost is measured: 3,717 ms worst lateness with 1 thread, 14 ms with 2. Crash triggers that also hit honest code: hard abort calls close() cross-thread (ComputerExecutor.java:254-256); accidental `while true do end` under the JIT ignores hooks (P1); memory is native and outside -Xmx ('one computer allocated 2 GiB', P9); lossy strings corrupt binary files (PLAN §1.2).

**Suggested text.**

### MVP path: personal build (newcomer ≈ 30–50 person-days, ≈ 6–10 weeks full-time; experienced ≈ 15–23 d)
**Scope:**
- NeoForge only; Windows x64 and Linux x64.
- JIT on; machine never returns PAUSE (README recommends `computer_threads = 2+`).
- Singleplayer and private use only, labelled "do not run on public servers".

**Must still include (each one prevents a JVM crash, a wedged computer, or lost or corrupted data, even when you only run your own code):**
- patches 01, 02 and 03 (plus 04–06 for ROM compatibility);
- soft and hard abort through the count hook plus `luaJIT_interrupt`;
- close() deferral (P4);
- protected boundary and iterative, depth-limited conversion (P5);
- Latin-1 8-bit strings;
- MethodResult trampoline (turtles, inventories);
- allocator cap (P9);
- `debug.sethook`, `string.dump`, `jit`, `ffi`, `io`, `os` and `package` removed;
- Cobalt fallback plus a config off-switch.

**Deferred, with documented known issues:**
- patch 07: a pathological `string.find` can wedge a computer until restart;
- patch 08 backstop;
- full P7 debug curation;
- pause;
- Fabric, macOS and arm64.

Public release still requires G3.

## 7. [should-fix] §4.2 / WP B / WP G / §6 Risks (macOS)

**Problem.** Platform risk is checked late, in M3/WP G, although it can be checked in week 1 without any JNI. Patch 01 has never run on arm64 (Apple Silicon or Linux aarch64). The preempt harness is Windows-only. The recommended pin 3c4f9fe2 (2026-07-11) predates an Oct 2026 macOS JIT fix, and the plan's build flags do not mention LuaJIT's macOS hardened-runtime option.

**Evidence.** preempt-patch.md:540 ('arm64 ... were not built or run'), :543 (ARM memory ordering 'reasoned, not tested'). harness.c uses windows.h/CreateThread/QueryPerformanceCounter/-lwinmm. I wrote a ~30-line shim (...\review-sequencing\posix\win32_on_posix.h), and the harness then built unchanged on Linux x64: 25/25 OK plus 3 expected X_ hangs (...\posix\preempt-linux-x64.txt; combined patched LuaJIT rebuilt from main-combined sources under WSL, ...\posix\build-linux.log). prior-art.md:131: `LUAJIT_ENABLE_OSX_HRT` (4f2bb199, 2025-03-10) for MAP_JIT under the hardened runtime, and 'Recent macOS-specific fixes: 716f74a6 (2026-10, mcode vs xzone malloc collision, #1520)', which is after the 3c4f9fe2 pin. PLAN §4.2 flags omit OSX_HRT.

**Suggested text.**

Add to WP B: "Pin `3c4f9fe2` **plus cherry-picks** of later upstream bug fixes that do not touch syntax, starting with `716f74a6` (macOS mcode vs xzone malloc, #1520). Keep the list in `luajit-patches/BACKPORTS.md` and re-check it at every bump. For macOS, build with and without `-DLUAJIT_ENABLE_OSX_HRT` and keep the variant that passes G5."

Add to M0 / WP G: "Week-1 CI, harness only, no JNI. Build patched LuaJIT and `harness_posix.c` on `ubuntu-24.04` (manylinux_2_28), `ubuntu-24.04-arm`, `macos-15` and `macos-15-intel`, then run `harness intr error <every case>`. This answers whether patch 01 works on arm64 before any bridge work. The full 5-platform matrix with headless-suite smoke tests stays in WP G."

## 8. [should-fix] §3 P1/P2/P3/P8/P10/P12 (status column) and WP H

**Problem.** The hard problems have no explicit acceptance tests, although CC:T's own suite and the existing harnesses provide them. One test shows a spec detail the plan gets wrong. §4.3 says the soft abort is 'a catchable "Too long without yielding" error, with a proper chunk:line: prefix', but CC:T's pattern-timeout test requires the message *without* a prefix. Another test conflicts with the P3 decision to drop debug.sethook.

**Evidence.** test-rom/spec/lua/timeout_spec.lua: loops `:str_match("^.+:%d+: Too long without yielding$")`; string.find/match with `("a"):rep(1e4), ".-.-.-.-b$"` must `:eq("Too long without yielding")` (exact, no position). spec/lua/coroutine_spec.lua:35-59 'within debug hooks' calls debug.sethook and yields inside the hook; it also tests yields within string.gsub, table.sort comparator and __lt, table.foreach(i), pcall and xpcall handlers, and metamethods. All of these pass on Cobalt (suite-cobalt.tsv). rom-compat.md:380 estimates a virtualised debug.sethook at 1–2 days.

**Suggested text.**

### Acceptance tests per problem
| Problem | Acceptance test |
|---|---|
| P1 | `timeout_spec` 'in loops' (message `<chunk>:<line>: Too long without yielding`); preempt harness 25/25 on every shipped platform |
| P2 / patch 07 | `timeout_spec` `string.find` and `string.match` must raise exactly `Too long without yielding` **with no position prefix** (a level-0 error from inside the matcher); harness case `X_string_find_backtrack` stops |
| P3 | `coroutine_spec` 'within debug hooks' passes only with a virtualised `debug.sethook` (1–2 d). Otherwise list it in `expected-failures.txt` (Decision 8) |
| P8 / F1 | `coroutine_spec` 'allow yielding within …' (gsub, sort, metamethods, pcall, xpcall); a nesting-depth spec errors without crashing |
| P10 | FairnessProbe worst lateness ≤ 100 ms |
| P12 | headless `smoke.lua` `headless.mainAdd` (`@LuaFunction(mainThread = true)`) and a `MethodResult.pullEvent` callback |
| P11 / P13 | the full CC:T suite (textutils 370 tests, cc.internal.syntax 355); each remaining failure is listed with the P13 reason |
| P5 | the 7 old JVM-killers (code-audit.md) ported as headless Lua specs plus test-API hooks (cyclic Java map, unknown Java type) |

## 9. [should-fix] §7 Decisions needed

**Problem.** The decision list is incomplete for this user. It leaves out skill and time budget, Mac access, debug.sethook, the memory-cap default, mod identity, test-rom sourcing, and when each decision is needed. Most items give no recommended default or consequence, so a newcomer cannot answer them quickly.

**Evidence.** PLAN §7 has 7 items; only 5 (LuaJIT pin) and 7 (branch) carry a recommendation. §4.3 says '`_VERSION = "Lua 5.2"` (decide)'. The memory cap is 'configurable' with no default. debug.sethook is dropped in §3 P3, but CC:T's suite tests it (coroutine_spec.lua:35). The scaffold metadata uses mod id `ccluajit`, package `gay.vereena.cclj` and authors 'vereena0x13, mindbound' (neoforge.mods.toml), under an MIT LICENSE © Vereena Inara.

**Suggested text.**

## 7. Decisions needed
Each item has a default. Answer "default" if you have no preference. The bracket says when the answer is needed.
0. **[before M0] Your profile and time.** (a) Have you written C with pointers and manual memory management: a lot, some, or none? (b) Have you used JNI? (c) Hours per week? → Picks the estimate column. If (a) or (b) is "none", line up a C/JNI reviewer for WP C/D before G3.
1. **[before M0] Loader.** Default: NeoForge first (1.72M vs 73k Modrinth downloads), Fabric after M2 (+1–2 d; its Gradle needs JDK 25).
2. **[before M0] First audience.** Default: personal MVP after M1; nothing public before G3.
3. **[before M2] Pause.** Default: defer; ship never-PAUSE and recommend `computer_threads = 2+`. Measured: another computer's timers were up to 3.7 s late with 1 thread and 14 ms late with 2 (Cobalt with pause: 76 ms). Decide again at G4.
4. **[before WP G] Platforms.** Default: Windows and Linux x64 first; arm64 Linux and macOS via CI after G0/G5. Do you have an Apple Silicon Mac for one manual test? If not, macOS ships JIT-off or Cobalt until a volunteer confirms.
5. **[before WP B] LuaJIT pin.** Default: `3c4f9fe2` plus a backport list (starting with `716f74a6`).
6. **[before M1] `_VERSION`.** Default: "Lua 5.2", the same as stock CC:T, so programs that check it behave identically.
7. **[before M2] `debug.sethook`.** Default: removed in the MVP (the ROM never uses it; CC:T's 'within debug hooks' test and Lua profilers break). Virtualise later (1–2 d).
8. **[before M1] Per-computer memory cap.** Default: 128 MiB, server-configurable. Exceeding it raises a Lua 'not enough memory' error, never a crash.
9. **[before M0] Branch.** Default: a fresh `1.21.1` branch from `main`.
10. **[before release] Identity and credit.** Default: keep mod id `ccluajit` and package `gay.vereena.cclj`, credit the original author, keep MIT, publish under the name CCLuaJIT.
11. **[before M0] Test-rom.** Default: vendor CC:T's `test-rom` from tag `v1.21.1-1.121.0` into `common/src/test/resources` (MPL-2.0 headers kept).

## 10. [should-fix] §5 Work plan table (missing WPs)

**Problem.** There are no WPs for user and developer documentation, release and distribution (Modrinth/CurseForge setup, versioning, licences, metadata), a specified config, or compatibility checks with popular CC:T addons, although M3 says 'Release as experimental'. The port-mechanics lane's 'release plumbing' line was dropped from the PLAN table. The scaffold's dependency ranges are open-ended, although §6 says to pin.

**Evidence.** PLAN §5 WP A to H contain no docs, publishing or addon testing. port-mechanics.md:205 'Release plumbing (versioning, mod-page uploads, LICENSE/COPYRIGHT in jar) 0.25-0.5 d / 0.5-1 d' is not in the PLAN. scaffold neoforge.mods.toml: computercraft `versionRange = "[1.121.0,)"`; fabric.mod.json: `"computercraft": ">=1.121.0"`; PLAN §6: 'pin a CC:T version range and re-test on every CC:T update'. In-game, ServerContext adds `.genericMethods(GenericSources.getAllMethods())` (ServerContext.java:78), which the headless runner lacks, so addon and generic peripherals need in-game checks.

**Suggested text.**

| WP | Content | Exp | Newcomer |
|---|---|---|---|
| I | **Docs.** User README and mod page: speed-up expectations, P13 incompatibilities, config keys, platform matrix, 'experimental' warning, how to disable. Developer docs: building natives, rebasing the patch stack, running `headlessSuite`. `SECURITY.md` for crash and sandbox reports | 1–2 d | 2–3 d |
| J | **Release and distribution.** Modrinth and CurseForge projects (allow for moderation wait). Version scheme `x.y.z+mc1.21.1`. Changelog. Licence and attribution files in the jar (LuaJIT MIT COPYRIGHT). Dependency range `computercraft [1.121.0,1.122)` on both loaders, plus a runtime check that falls back to Cobalt on an untested CC:T. Server-side-only metadata (check vanilla and NeoForge clients can join without the mod). Optional publish GitHub Action | 1–2 d | 2–4 d |
| K | **Config.** Server-side config (NeoForge `ModConfigSpec` SERVER type; on Fabric a plain TOML or JSON file): `enabled`, `jit`, `memory_limit_mb`; read at ServerContext creation. A config UI is optional (NeoForge gives one almost free; on Fabric skip it) | 0.5–1 d | 1–2 d |
| L | **Addon compatibility pass in-game.** Generic inventory and fluid methods, plus 2–3 popular CC:T addons (peripheral and main-thread methods) | 1 d | 1–2 d |
| — | **Contingency** | +20 % | +20 % |

## 11. [should-fix] New subsection under §5: 'Start here (learning path and artifacts)'

**Problem.** The plan does not tell a newcomer what to learn first, which existing artifacts are the right starting code, or which to avoid. The C/JNI/LuaJIT learning load is the user's main risk and is not sequenced.

**Evidence.** Starting points that exist: scaffold-multiloader (CCLuaJITCommon factory with Cobalt fallback, NativeLoader 108 lines); luajit-patches/build-patched-luajit.sh; lua/cclj_prebios.lua (825 lines); harnesses/runtime/allocwrap.c (66 lines, allocator cap), rt.c (1295 lines of C-API probes); harnesses/preempt/harness.c (410 lines, interrupt protocol); bench/hostcall/jnibench.cpp (326 lines, lean JNI paths). The reference implementation is small: CobaltLuaMachine.java 329 lines, ResultInterpreterFunction.java 103, VarargArguments.java 246, TableImpl.java 135. PLAN §1.2: 'Salvageable: less than 10% of main.cpp'.

**Suggested text.**

### Start here (newcomer path)
1. **Day 1–2:** run WP 0 (HeadlessRunner, then CctSuiteRunner) with Cobalt. Then read the reference machine: `CobaltLuaMachine.java` (329 lines), `ResultInterpreterFunction.java`, `VarargArguments.java`, and `reports/cct-contract.md` §3–§6 and §11.
2. **Lua 5.1 C API** (manual §3–§4): stack discipline, `lua_pcall`/`lua_cpcall`, `lua_resume`/`lua_yield`, registry refs. Errors are `longjmp`s, which is why §4.2 forbids RAII and `catch` across Lua calls. Exercise: change `artifacts/harnesses/preempt/harness.c` (Windows) or `harness_posix.c` (Linux and macOS, with the `win32_on_posix.h` shim) to run a coroutine and report errors.
3. **JNI:** local vs global refs, `PushLocalFrame`/`PopLocalFrame`, `ExceptionCheck` after every call, and never `longjmp` across a JVM frame. Always run with `-Xcheck:jni`. Model code: the lean paths in `bench/hostcall/jnibench.cpp`. **Do not start from** the 1.20.1 or 1.14.4 `main.cpp`.
4. **Native debugging:** read `hs_err_pid*.log`. Make debug builds with `-g -DLUA_USE_APICHECK -DLUA_USE_ASSERT`. Use gdb in WSL. A JVM cannot unload a native library, so every change needs a JVM restart; the headless boot takes ~4 s.
5. **Later:** LuaJIT internals (`lj_record.c`/`lj_trace.c` for patch 01 rebases, `lib_string.c` for patch 07). Minecraft 1.21.1 specifics (MDG `runClient`, `neoforge.mods.toml`, `ModConfigSpec`) take about half a day to a day with 1.7.10 experience.

**Code to start from:**
- `scaffold-multiloader` (Gradle, NativeLoader, factory with Cobalt fallback);
- `luajit-patches/build-patched-luajit.sh`;
- `lua/cclj_prebios.lua` (embed and run before the bios);
- `runtime/allocwrap.c` (memory cap);
- `preempt/harness.c` (interrupt protocol).

## 12. [should-fix] §5 Milestones M1 (restructure into vertical slices)

**Problem.** M1 bundles in-game boot, all APIs, turtles and peripherals, 8-bit strings, and soft and hard aborts into one 2–3 week item. WP C (host core) and WP D (JNI bridge) are listed as separate 6–9 day blocks, although they can only be built and tested together. A newcomer needs slices that each end in a runnable, testable state, done headless first and in-game second.

**Evidence.** PLAN §5 M1 text; WP C and WP D are separate rows. The headless runner already exercises mainThread callbacks, events, timers, fs and terminal output without Minecraft (out-cobalt-smoke.txt). The turtle-specific path (TurtleBrain 'turtle_response', verify-cct-contract CT-4) needs in-game testing only for the final check.

**Suggested text.**

### M1 as vertical slices (each one headless and testable)
1. **S1 – hello machine:**
   - `LuaJITMachine` creates a state and runs a 3-line bios.
   - One `ILuaAPI` method call (`term.write`) works.
   - An error comes back as `MachineResult.error`.
   - `-Xcheck:jni` is clean.
   - (G1)
2. **S2 – events and filters:**
   - `handleEvent` with name and args;
   - filter handling and `terminate`;
   - `GENERIC_ERROR` when the bios returns.
3. **S3 – conversions:**
   - Latin-1 strings, `byte[]`/`ByteBuffer`, Map/Collection with cycles, depth limits;
   - method tables, `IArguments`, `LuaException` levels.
4. **S4 – real ROM:**
   - `cclj_prebios.lua` plus the real bios boots to the shell;
   - `smoke.lua` passes (except mainAdd).
5. **S5 – callbacks:**
   - MethodResult trampoline;
   - `headless.mainAdd`, `MethodResult.pullEvent`.
6. **S6 – timeouts:**
   - count hook plus `luaJIT_interrupt`, soft and hard abort, close() deferral;
   - `timeout_spec` 'in loops'; no leaked worker.
7. **S7 – suite triage:**
   - CC:T suite ≥ 1250/1407 plus `expected-failures.txt`.
   - (G2)
8. **S8 – in-game (NeoForge dev client):** computer, turtle (move, dig, inventory), monitor, disk drive, modem, plus one generic-inventory peripheral. This is the personal MVP.

## 13. [nice-to-have] WP H / WP 0 details

**Problem.** Small follow-ups to make the headless oracle fully representative.

**Evidence.** The 5 Cobalt 'errors' are all http_spec, caused by `.disableHttp()` (suite-cobalt.tsv). core's POM brings netty-codec-http as a runtime dependency, so `testImplementation` of core makes NettyHttp available. ComputerTestDelegate attaches FakeModem (TOP) and FakePeripheralHub (BOTTOM) (ComputerTestDelegate.java:122-123), which CctSuiteRunner copies. In-game ServerContext adds genericMethods (ServerContext.java:78); the headless context does not.

**Suggested text.**

- Run the suite once with HTTP enabled (drop `.disableHttp()`; NettyHttp comes in transitively through cc-tweaked-core) to bring the Cobalt baseline to 1395/1407.
- Add a CI job that runs `headlessSuite` against each new CC:T release as an upstream canary for the non-public `ILuaMachine` and `ServerContext.luaMachine` contract.
- Add `--factory cobalt` and `--factory luajit` runs of `artifacts/bench` through `HeadlessRunner --startup` for an end-to-end perf comparison that includes host calls.
- Optional: build libcclj with `-fsanitize=address` on Linux and run the headless suite under it before G3.
