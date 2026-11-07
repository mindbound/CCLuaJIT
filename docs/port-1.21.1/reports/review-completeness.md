# Plan review: completeness and risk (PLAN.md v1)

*Written on 2026-11-07 by an independent review/prototype agent. Section references (§, P-numbers, WP letters) point to PLAN.md **v1**; PLAN.md v2 has incorporated or answered every item (see its change log). Paths under `scratchpad/work/...` refer to the session scratch workspace; the reusable files were copied into `../artifacts/`.*

## Verdict

From a completeness and risk view, the plan is strong on the VM-internal hazards it already found (JIT preemption, bytecode, unprotected errors, C-stack, allocator cap, close concurrency). It is not yet a plan I could sign off for public servers. The gaps are in what sits around the VM: object lifetime across the JNI boundary, the rest of the sandbox surface, server-wide resources, CC:T integration details, operations, licensing and testing. I checked each item below against CC:T 1.121.0 sources or ran it on the combined patched LuaJIT build.

Items confirmed by experiment or source:
1. Plain `string.find` is quadratic, and planned patch 07 does not cover it: a 6 MB input ran 105 s inside `lj_str_find`, which JIT traces also call.
2. `debug.getlocal`/`getinfo` on a suspended coroutine can take the private native functions held in the Lua trampoline's locals, and `debug.getmetatable` ignores `__metatable`. Section P7 only covers `getupvalue`, and `getfenv`/`setfenv` (the Luanti CVE-2026-41196 class) are not mentioned.
3. CC:T relies on Java GC to close leaked file handles (WeakReference-based `maximum_open_files`) and websockets (`GlobalCleaner`). A pinned Java handle table would break both, and the plan never says how Java objects are released.
4. LuaJIT's `math.random` uses a fixed seed. Every computer produces the same sequence, so rednet's message-ID de-duplication drops messages between computers that boot together.
5. Only a per-computer memory cap is planned. Natives plus JIT machine code have no server-wide bound, which matters for container OOM-kills.
6. If `close()` holds the machine lock while calling `removeListener`, it inverts lock order with `TimeoutState` listeners, which run under the monitor. That can deadlock the Computer-Monitor thread.
7. Without PAUSE, one player can freeze all computers for up to 7 s per event under the default `computer_threads=1`. PAUSE is outside the M2 public-release gate.
8. `unpack` and `string.byte` stop at 8,000 values, but CC:T's own `vm_spec` requires 2^19. Raising `LUAI_MAXCSTACK` to 65000 was tested and lifts the limit to about 30k–64k.
9. `tostring` and `%p` print raw heap addresses.
10. Eager `ObjectArguments` turns `term.write(cyclic table)` into a `StackOverflowError`, and CC:T logs every Java exception with a stack trace, so a player can flood the log.

Other gaps:
- **Licensing:** CCPL is non-commercial and share-alike for copied code, so no CobaltLuaMachine or ROM code can go into the MIT repo. MPL-2.0 code must stay in separate MPL-licensed files.
- **Version and conflict handling:** the scaffold's open version range conflicts with the plan's "pin", nothing probes the contract at runtime, and nothing detects other mods that set `ServerContext.luaMachine` or mixin into `CobaltLuaMachine` (CCGraphics and cride both do).
- **Config, observability and admin controls:** none are specified. CC:T's `Metric` registry, log markers and `SanitisedError` could all be reused.
- **Platforms:** musl is not detected, and the profiler/vmevent code is not compiled out. The C-stack guard margin does not account for HotSpot guard zones or page size, and arm64 preemption has not been validated.
- **Upstream LuaJIT:** the 3c4f9fe2 pin is a permanent fork. Upstream v2.1 has had many JIT and OOM fixes since the pin, some likely to apply.
- **Antivirus:** LuaJIT is used by gaming-themed malware loaders, so a LuaJIT DLL extracted into a game folder risks AV quarantine.
- **Testing:** CC:T's test-rom is runnable headlessly, but its Java driver and fixtures are not in the published jars and are hard-wired to a Cobalt subclass. Several specs will diverge by design, and there is no soak test, threads>1 test, or add-on matrix.

With the suggested text below, I estimate about +8–14 person-days for an experienced developer and a larger M2 gate.

## 1. [must-fix] §4.4 JNI bridge (conversion rules: 'objects with methods → method tables') and P7

**Problem.** The plan never says how Java objects held by Lua are kept alive and released. This covers fs/http/websocket handles, peripheral objects, ILuaFunction and IDynamicLuaObject. LuaJIT tables have no __gc. If handles are pinned in a Java-side table until close(), they leak for the machine's lifetime. That breaks CC:T's own GC-driven cleanup: a program that forgets close() hits the maximum_open_files limit (default 128) or leaks websockets until reboot, where Cobalt would have closed them. If handles live in host userdata, P7's `__metatable` does not protect them, because the curated debug.getmetatable (needed by the ROM) bypasses `__metatable`. That re-opens the getmetatable(ud).__gc(ud) use-after-free that code-audit already reproduced.

**Evidence.** CC:T 1.121.0: core/filesystem/FileSystem.java:39-40 (`HashMap<WeakReference<FileSystemWrapper<?>>, SeekableByteChannel> openFiles` + ReferenceQueue: files are closed when the Lua-visible wrapper becomes unreachable); core/apis/http/websocket/Websocket.java:234 `GlobalCleaner.register(handle, () -> close(1001, ""))`; AbstractHandle returns direct ByteBuffers (allocateDirect, :149). Probe work/review-completeness/probes/p5.lua: `setmetatable(t,{__metatable='locked'})` → getmetatable gives 'locked', `debug.getmetatable(t)==mt` is true. code-audit.md:74 (gc-upvalue UAF).

**Suggested text.**

**Java object lifetime (new subsection in §4.4).**
- Each Java object converted to Lua (method table, ILuaFunction, IDynamicLuaObject) gets one host userdata. The userdata holds a generation-tagged integer handle into a per-machine Java `Object[]`/`Int2ObjectMap`. All method closures of that object capture it as an upvalue.
- Do not use per-object JNI global refs. Do not store per-machine global refs either: pass `this` (a local ref) down each native call and clear it on return, so close() has nothing to leak.
- The userdata `__gc` is a C function. It only pushes the handle id onto a native 'dead handles' ring, preallocated outside the Lua allocator. It never calls JNI, never allocates and never raises.
- After every handleEvent and in close(), Java drains the ring and drops those references. CC:T's WeakReference/Cleaner logic (FileSystem.openFiles, Websocket GlobalCleaner) then auto-closes leaked files and websockets as it does under Cobalt.
- Stale or forged handles (generation mismatch, wrong machine) raise a Lua error. They never index the table blindly.
- Host metatables carry `__metatable`. The curated `debug.getmetatable` returns nil (or the `__metatable` value) for every host-owned metatable, and the curated `debug.setmetatable` (if exposed) refuses host userdata.
- Tests:
  - open 1,000 files in a loop without close(), let them become garbage, assert no 'Too many files open';
  - 10,000 http/websocket handle churn, assert the Java handle table stays bounded;
  - `debug.getmetatable(handle_ud)` returns nil;
  - calling a method after a forced handle reuse errors cleanly.

## 2. [must-fix] P7 (curated debug library) and §4.3 'Curated environment'

**Problem.** P7 restricts only getupvalue, setupvalue and upvaluejoin. Two other routes reach privileged objects.
(a) `debug.getlocal(co, level, n)` and `debug.getinfo(co, level, 'f')` work on suspended coroutines. The P12 Lua trampoline yields while holding the private native resume function and the callback handle in locals, so the parent coroutine can read and call them. The same applies to Lua sort, gsub and load wrappers while they call user code.
(b) getfenv/setfenv are not mentioned. Cobalt exposes them, so the ROM and user code expect them. This is the Luanti CVE-2026-41196 class: getfenv(builtin) returns the original globals. setfenv on a prelude function also hijacks its global lookups.

**Evidence.** Probe work/review-completeness/probes/p5.lua on the combined build: the trampoline's locals `nr` (native fn) and `hd` (handle) were read via `debug.getlocal(co,1,i)` on the suspended coroutine. `pcall(nr, 999)` printed 'PRIVILEGED CALL with handle 999'. prior-art.md §3: Luanti GHSA-g596-mf82-w8c3 / CVE-2026-41196 (CVSS 10.0) 'only exploitable when using LuaJIT', root cause getfenv on builtins. cct-contract §11.2 says base includes getfenv/setfenv.

**Suggested text.**

P7 addition: the curated debug and base functions treat every host-owned function (C functions and functions compiled from the host prelude chunk, recorded in a native weak set at load) as opaque.
- `debug.getinfo(...)` with option 'f' returns nil for host frames; `getinfo` never returns `func` for them; 'u' and 'S' fields stay allowed.
- `debug.getlocal`/`setlocal` (thread or level form) refuse frames whose function is host-owned.
- `debug.getupvalue`/`setupvalue`/`upvalueid`/`upvaluejoin` refuse host-owned functions.
- `getfenv(f)` returns the sandbox `_G` for host-owned functions and for numeric levels that land on a host frame. `setfenv` refuses host-owned functions and such levels.
- At state creation, set the environment of every C function and prelude function to the single sandbox `_G` (the Luanti fix). Never keep a richer 'real' globals table. Remove `jit`, `io`, `os`, `package`, `ffi` and `bit` from `_G` and from `registry._LOADED`/`_PRELOAD`.
- All native entry points validate their arguments as attacker-controlled (handle generation, owning machine, current coroutine), because reachability alone is not a boundary.
- Regression tests:
  - getlocal on a suspended trampoline;
  - getinfo 'f' inside a sort comparator and a gsub callback;
  - getfenv(print) == _G;
  - setfenv on a prelude function errors;
  - calling a stolen native function with a forged handle errors.

## 3. [must-fix] P9 / §4.3 'Allocator cap' and §6 Risks

**Problem.** Only a per-computer cap is planned. Server-wide native memory is unbounded: (computers × cap) + JIT machine code. Machine code is mmap'd outside the allocator, up to maxmcode=512 KB per state by default, and is not counted by the cap. Public servers usually run in containers (Pterodactyl, Docker) with a memory limit just above -Xmx. Exceeding it gets the whole server OOM-killed (SIGKILL, unsaved chunks lost). Placing many computers is cheap for a player. Under Cobalt all Lua memory is bounded by -Xmx collectively.

**Evidence.** lj_jit.h:136 `maxmcode 512` (KB/state); lj_mcode.c:112 mmap'd mcode, not via lua_Alloc. probes/p4.lua: 4e6 small tables = 368 MB native heap in one state. luajit-runtime.md §e: one state allocated a 2 GiB string without a cap. PLAN.md §4.3 lists only 'configurable per-computer limit'.

**Suggested text.**

**Memory budget (replace the 'Allocator cap' bullet).**
- Two caps, both enforced in the counting allocator wrapper with one process-wide atomic counter:
  - (1) `per_computer_memory`, default 64 MiB;
  - (2) `global_native_memory`, default min(1 GiB, 25% of `Runtime.maxMemory()`), across all LuaJIT states.
- An allocation that would exceed either cap returns NULL, which gives a catchable 'not enough memory' in that computer. The 1 MiB per-state reserve is exempt but counted.
- Set `jit.opt.start('maxmcode=256')` (configurable) at state creation. Add maxmcode × live states to the global figure as an estimate.
- Keep the per-computer default small enough that any single linear C operation (rep, concat, GC full cycle, lua_close) stays well under 0.5 s. Measured here: full GC of a 368 MB heap took 0.19–0.25 s; `('x'):rep(2^30)` took 0.53 s.
- Document for container hosts: container limit ≥ Xmx + JVM overhead + `global_native_memory` + maxmcode × computers.
- Expose current/peak global and per-computer bytes as metrics (see Observability).

## 4. [must-fix] P2 / WP B (patch 07 scope) and §3 table

**Problem.** Patch 07 is scoped to pattern backtracking in lib_string.c match(). Two other paths are uninterruptible C code with superlinear or large running time, both reachable from tiny inputs.
(1) Fixed-string search: string.find with plain=true, or with any pattern without specials, uses `lj_str_find`, a naive memchr+memcmp O(n·m) loop. It is also called directly from JIT traces (IRCALL_lj_str_find) and at record time.
(2) The parser: load() of a large chunk is not polled. Cobalt polls interrupts in its compiler input (LuaC.java:205).
Both exceed CC:T's 1.5 s hard-abort window, so the worker thread is abandoned and the computer wedges. Cobalt's plain indexOf is also quadratic, so this is partly an upstream CC:T issue, but the port should not ship it.

**Evidence.** probes/p2.lua on the combined build: subject 4e6 'a', needle 2e6 'a'..'b' → `plain find` 105.13 s (only ~6 MB of memory); 2e6/5e5 → 18 s plain and 17 s with no-specials pattern. lib_string.c:486 and lj_ffrecord.c:980-982 call lj_str_find; lj_str.c lj_str_find is memchr+memcmp. probes/p3.lua: loadstring of a 38 MB hash constructor took 1.50 s, and 20 MB of unique-string constants took 1.03 s. cobalt-src LuaString.indexOf is also O(n·m).

**Suggested text.**

**WP B, patch 07 (interruptible or bounded C paths), widened scope:**
- (a) Poll `g->intr` in lib_string.c `match()`, `do_match`/`max_expand`/`min_expand`, AND in the outer loops of `str_find_aux`, gmatch and gsub (every ~4K steps). Raise 'Too long without yielding' with no position, which matches CC:T test-rom timeout_spec.
- (b) Replace `lj_str_find` with a linear-time algorithm, e.g. Two-Way (port musl's memmem, MIT). This covers interpreter, JIT IR call and recorder use with no polling.
- (c) Poll the interrupt in the lexer's input fill (`lex_next`/buffer refill) every 64 KiB, raising through the normal lexer error path.
- (d) Keep an inventory table in the repo of every C function reachable from Lua, with its complexity bound. Rule: anything worse than O(bytes touched), or able to touch more than the per-computer cap, must poll.
- Add harness cases `X_plain_find_quadratic`, `X_nospecial_find_quadratic`, `X_huge_parse` and `X_gsub_many_matches`. Each must stop in < 100 ms after the interrupt.

## 5. [must-fix] P4 / §4.3 Interrupt protocol and §4.4 'close()'

**Problem.** The plan says the TimeoutState listener sets the request 'under the close mutex', and that close() 'removes the TimeoutState listener'. CC:T runs listeners while holding the TimeoutState monitor: refresh/reset/hardAbort/startTimer are synchronized and call updateListeners. removeListener also needs that monitor. If close() holds the machine mutex while calling removeListener, while the monitor thread runs refresh() → listener → waits for the machine mutex, the two threads deadlock. A deadlocked Computer-Monitor thread means no computer on the server can ever be timed out again.

**Evidence.** core/computer/TimeoutState.java:109-122 (`updateListeners` @GuardedBy(this); `synchronized addListener/removeListener`); ManagedTimeoutState.java:41 `synchronized refresh()`, :106 `synchronized startTimer`, :116 `synchronized reset()` → updateListeners. ComputerThread calls refresh() from the monitor thread and reset() from workers (cct-contract §3.1/§4).

**Suggested text.**

P4 design rule (add to §4.3/§4.4):
- Lock order is `TimeoutState monitor` → `machine.lock`, never the reverse.
- close() calls `env.timeout().removeListener(listener)` FIRST, without holding `machine.lock`. Only then does it take `machine.lock` to set CLOSED and request the hard interrupt.
- The listener body takes only `machine.lock` (or a tryLock/atomic CAS on the native-pointer field), does the two stores of `luaJIT_interrupt`, and never calls back into CC:T.
- After removeListener returns, no listener invocation is in flight, because it is serialized on the monitor. Only then may the owner thread free the state.
- Test: a stress harness running close() from 4 threads against refresh()/reset() at 1 kHz for 10 minutes, under the real CC:T ManagedTimeoutState, with a watchdog asserting the monitor thread keeps ticking.

## 6. [must-fix] P11/P13 (ROM compatibility) and §4.3 Curated environment

**Problem.** LuaJIT seeds math.random with a fixed seed. Every computer, and every reboot, produces the identical sequence. The ROM's rednet uses `math.random(1, 2147483647)` as the message ID and drops any message whose ID it has seen in the last 9.5 s, including IDs it sent itself. Computers that boot together (every server start, chunk load) and talk over rednet at startup therefore silently lose messages. Player programs that derive IDs, nonces or keys from math.random also become predictable. rom-compat classified this as 'cosmetic'. Cobalt seeds each state from java.util.Random.

**Evidence.** probes: three separate `luajit -e "print(math.random(1,2147483647), math.random())"` runs all print `1705545026 0.698852465637164`. ROM rom/apis/rednet.lua:182-183 (`message_id = math.random(1, 2147483647); received_messages[message_id] = os.clock() + 9.5`), :477-478 drops already-seen IDs. cobalt-src lib/MathLib.java:163-167 seeds from `random.seeder.nextLong()`. rom-compat.md:210 calls it cosmetic.

**Suggested text.**

P11 addition: at state creation, before the bios runs, seed the VM PRNG from Java `SecureRandom` (two 64-bit words passed to a native `cclj_seed`), or call `math.randomseed(x)` with a SecureRandom double. That gives distinct sequences per computer and per boot, matching Cobalt.
Also match Cobalt's argument errors ('interval is empty' for `math.random(0)` and `math.random(3,1)`) in the C wrapper.
Test: boot 50 machines in one JVM and assert their first 3 `math.random(1,2^31-1)` values are pairwise distinct. Add a rednet test: two computers booted simultaneously exchange first messages and both are received.

## 7. [must-fix] §5 Milestones (M2 'Gate for any public release') and Decision 3

**Problem.** PAUSE is in M3, so a public release at M2 runs with 'never return PAUSE'. With CC:T's default computer_threads=1, any player can freeze every other computer on the server for up to ~7 s per event, repeatedly, without being aborted: busy for 6.9 s, then `os.queueEvent('x'); os.pullEvent('x')`. Stock CC:T pauses the same program after 5–50 ms when others are queued. CC:T's vruntime scheduler only makes the hog wait its turn; it does not shorten its slice. The M2 gate also lacks the other items in this review.

**Evidence.** cct-contract §4: scaledPeriod 25 ms with one waiter, 5 ms with 9+ waiters; TIMEOUT 7 s; ConfigSpec.java:161-167 computer_threads default 1. ComputerThread.java:152,308-359 (virtualRuntime fairness only orders runs). verify-perf-bench.md:293 'one player's one-liner stalls every computer on the server'.

**Suggested text.**

Replace M2 with: **M2 – Server-safe (gate for any public release):**
- sandbox hardening (P6, P7 incl. getlocal/getinfo/getfenv/setfenv and getmetatable filtering);
- Java object handle lifecycle with GC-driven release;
- per-computer AND global native memory caps;
- C-stack guard;
- patch 07 widened (patterns, `lj_str_find`, lexer);
- close/listener lock order;
- PRNG seeding;
- **PAUSE token protocol (WP F) or, if deferred, a documented requirement `computer_threads ≥ 4` plus a measured worst-case stall table in the README** (a stall is a fairness regression, not just a perf issue);
- crash/fuzz harnesses and the soak test pass;
- CC:T test-rom passes except the checked-in expected-failures list;
- config kill-switch, metrics and logging (see Observability) in place;
- Linux x64 + arm64 dedicated-server runs green.
Move WP F from M3 to M2 if the first release targets public servers (Decision 2).

## 8. [should-fix] New §6 Risks item 'Licensing' (and WP A release plumbing)

**Problem.** The plan says nothing about licensing. The repo and mod are MIT, but the reference material is mixed-license. CobaltLuaMachine.java and ILuaMachine.java (and 80 of 105 ROM .lua files, incl. bios.lua) are LicenseRef-CCPL. CCPL §5/§7 require copied sections to remain CCPL, non-commercial only, with source available at no cost, so they cannot be relicensed MIT. code-audit.md mis-states the CCPL as 'Do not distribute without permission'. ResultInterpreterFunction, VarargArguments, TableImpl, the test-rom specs and the ComputerTestDelegate/BasicEnvironment harness are MPL-2.0: file-level copyleft that can be combined with MIT. artifacts/bench/lua/b10_compile.lua already embeds CCPL ROM code.

**Evidence.** SPDX headers in src-1.21.1-1.121.0/dan200/computercraft/core/lua/{CobaltLuaMachine,ILuaMachine}.java = LicenseRef-CCPL; {MachineResult,ResultInterpreterFunction,TableImpl,VarargArguments}.java = MPL-2.0; ROM: 80 files CCPL, 25 MPL-2.0. CCPL text fetched from cc-tweaked/CC-Tweaked LICENSES/LicenseRef-CCPL.txt (saved at work/review-completeness/dl/CCPL.txt) §5 'All distributions of this mod must remain licensed under the CCPL', §7 'provided they are non-commercial ... must ... remain licensed under the CCPL'. CCLuaJIT LICENSE = MIT. artifacts/bench/lua/b10_compile.lua:7-9 carries 'SPDX-License-Identifier: LicenseRef-CCPL'.

**Suggested text.**

**Licensing (add to §6 and WP A).**
- Do not copy code from CCPL files (CobaltLuaMachine, ILuaMachine, bios.lua, ROM APIs). Implement from the behavioural contract (cct-contract §11), and record in CONTRIBUTING that the machine is a clean re-implementation.
- Code adapted from MPL-2.0 CC:T files (VarargArguments/ResultInterpreterFunction semantics, test-rom, ComputerTestDelegate, BasicEnvironment) stays in separate files with their SPDX headers and LICENSES/MPL-2.0.txt.
- Ship in the jar under META-INF/licenses/:
  - LuaJIT COPYRIGHT (LuaJIT MIT, Lua MIT, dlmalloc notice);
  - Cobalt/LuaJ MIT, if Bit32Lib or StringMatch logic is ported to C;
  - CraftOS-PC MIT, if anything is borrowed;
  - the MPL-2.0 text, if test code is published.
- Use REUSE-style per-file SPDX headers and run `reuse lint` in CI.
- Do not commit CCPL ROM code (e.g. bench b10_compile.lua) into the MIT tree. Generate it at test time from the CC:T jar, or keep it under a clearly CCPL-marked directory.
- mod metadata `license` stays 'MIT' only if the shipped jar contains no MPL/CCPL code; otherwise 'MIT AND MPL-2.0'.

## 9. [should-fix] §6 Risks 'CC:T internals are not public API' and §4.1 Mod/loader layer

**Problem.** The version strategy is undefined and contradicts itself. The plan says 'pin a CC:T version range'. cct-contract recommends `<1.122`. The scaffold ships an open `>=1.121.0` (fabric) / `[1.121.0,)` (neoforge). A tight upper bound makes servers fail to start whenever a modpack bumps CC:T. An open bound with no runtime check risks a silent binary or semantic mismatch: 1.115→1.121 already changed `IArguments.escapes()` in a binary-incompatible way. Nothing detects other mods that also assign `ServerContext.luaMachine` (last writer wins silently), or add-ons that mixin into CobaltLuaMachine and silently stop working under LuaJIT.

**Evidence.** scaffold fabric.mod.json `"computercraft": ">=1.121.0"`; neoforge.mods.toml `versionRange = "[1.121.0,)"`; cct-contract §10 recommends `<1.122`. GitHub code search: Platratio34/CCGraphics and mlntcandy/cride have `@Mixin(CobaltLuaMachine.class)` (toValue/toObject/handleEvent injections); SirEdvin/Minecraft-Modding-Libs testiarium CctComputers.kt sets `ServerContext.luaMachine = CctLuaMachineFactory`; CC:T's own gametest TestHooks.kt sets it too. CC:T 1.121.0 is tagged for 1.21.1, 1.20.1 and 26.3, so the same version line keeps moving.

**Suggested text.**

**CC:T compatibility policy (replace the §6 bullet).**
- Declare `computercraft >=1.121.0` with no hard upper bound.
- At mod init, run a reflective contract probe:
  - signatures of ILuaMachine/Factory;
  - MachineEnvironment record components;
  - `IArguments.escapes()` returning void;
  - `ServerContext.luaMachine` public static non-final;
  - `LuaMethod.apply`, `MethodResult` accessors.
  On any mismatch, log ERROR naming the CC:T version and leave Cobalt in place (the server still starts).
- At SERVER_STARTED, assert `ServerContext.luaMachine == ourFactory`. If not, log WARN 'another mod replaced the Lua machine factory (<class>); CCLuaJIT inactive'.
- Wrap the previous factory value so the Cobalt fallback goes through whatever factory was installed before ours.
- If mods known to mixin into CobaltLuaMachine (ccgraphics, cride) are loaded, log WARN listing them, and add them to the docs incompatibility table.
- On first native load, compile (do not run) every ROM .lua file with the LuaJIT parser. Any failure means CC:T introduced syntax we do not support: fall back to Cobalt with a clear log.
- A weekly CI job runs the test-rom suite against the newest CC:T release for 1.21.1 and opens an issue on failure.

## 10. [should-fix] §4.1 'Graceful fallback ... config switch' / WP A

**Problem.** Admin controls amount to one per-server switch. CC:T reads `ServerContext.luaMachine` once per server start, so that switch needs a restart. On a public server an admin needs a live kill switch, per-computer opt-out or quarantine, limit tuning and a JIT toggle. A JIT miscompile is a silent wrong-result bug, and running interpreter-only (still 3.7x Cobalt) is the safe fallback. No config spec exists.

**Evidence.** cct-contract §10: luaMachine read only in the ServerContext constructor. The scaffold factory is already a wrapper (`nativesAvailable() ? new LuaJITMachine : new CobaltLuaMachine`), so a per-create decision is cheap. Upstream LuaJIT v2.1 fixed several x64/ARM64 JIT miscompiles after the pin (e.g. 1edc3e52 XLOAD fusion, 5ed524c0, 849bbbe7).

**Suggested text.**

**Config (`config/ccluajit-server.toml`, server-side, reloadable where noted).**
- `enabled` (bool, live: affects the next computer boot; the factory wrapper is consulted per `create()`);
- `jit` (bool, live per boot; false = interpreter only);
- `per_computer_memory` (MiB, default 64);
- `global_native_memory` (MiB, default min(1024, 25% Xmx));
- `max_coroutine_depth` (default 128);
- `max_conversion_values` (default 1,000,000; see conversions);
- `jit_maxmcode_kb` (default 256);
- `cobalt_computers` (list of computer IDs that always use Cobalt);
- `auto_quarantine_after` (N hard aborts that failed to stop in 1.5 s → that computer ID uses Cobalt until restart; default 1);
- `log_rate_limit_seconds` (default 60).
The factory resolves the computer ID by finding `OSAPI` in `env.apis()` and calling `getComputerID()`.
Admin command `/ccluajit` (permission level 3):
- `status`: natives, platform, security mode, live states, global bytes;
- `top [memory|aborts]`;
- `dump <id>`;
- `quarantine <id>` / `unquarantine <id>`;
- `disable`: live; new boots use Cobalt; optionally reboots LuaJIT computers.

## 11. [should-fix] New subsection 'Observability' (§4.4) and §8 printExecutionState

**Problem.** The plan has no logging or metrics design. Admins cannot see which computer hit a limit, used native memory, triggered a soft/hard abort, the C-stack guard or the coroutine limit, or was abandoned. CC:T already provides public hooks: Metric.Counter/Event constructors self-register in `Metric.metrics()` and are shown by `/computercraft track` and the JMX MBean, and MachineEnvironment.metrics() is the per-computer observer. Player-controlled text (error messages, chunk names, labels) reaches logs: CC:T sanitizes it with `SanitisedError` (200 chars, escapes control and ANSI chars) and tags it with markers filtered by `log_computer_errors`. Without rate limiting a player can flood the server log.

**Evidence.** core/metrics/Metric.java:21-38 (public Counter/Event ctors register in `allMetrics`); shared/computer/metrics/ComputerMBean.java:46 iterates `Metric.metrics()`; CommandComputerCraft.java:102-107 `track` takes any metric. core/util/SanitisedError.java (MAX_LENGTH 200, escapes). core/Logging.java:14-17 markers COMPUTER_ERROR(.VM/.JAVA); ConfigSpec.java:128-133,399 `log_computer_errors` filter. ResultInterpreterFunction.java:60 logs a full stack trace for every Java exception.

**Suggested text.**

**Observability.**
- Register static metrics at mod init, before ServerContext/ComputerMBean exist:
  - `Metric.Event("luajit_memory","bytes",Metric::formatBytes)` (peak per handleEvent);
  - Counters `luajit_soft_aborts`, `luajit_hard_aborts`, `luajit_oom`, `luajit_cstack_limit`, `luajit_coroutine_limit`, `luajit_abandoned`.
  Report them per computer via `env.metrics().observe(...)`, so they appear in `/computercraft track` and JMX.
- Every player-triggerable log line:
  - uses `Logging.VM_ERROR` / `Logging.COMPUTER_ERROR` markers, so `log_computer_errors=false` silences it;
  - wraps untrusted text in `SanitisedError` or the same 200-char escaping;
  - includes computer ID and label;
  - is rate-limited per computer (`log_rate_limit_seconds`).
- The native hook stores the last 'chunk:line' into an atomic per machine. printExecutionState prints it (sanitized) without touching the VM.
- Startup banner: CCLuaJIT version, LuaJIT commit + patch list, `jit.security()` string (assert prng=1, strhash=1, mcode=1), platform, caps.
- Ship split debug symbols per release, so hs_err native frames can be symbolized.

## 12. [should-fix] WP H Testing (and §5 estimates)

**Problem.** WP H says 'headless CC:T runner ... running CC:T's own Lua test suite' but leaves out what that takes. The suite lives in projects/core/src/test/resources/test-rom (mcfly.lua plus ~85 spec and support files). Its Java driver (ComputerTestDelegate.java, 542 lines) and fixtures (BasicEnvironment, TestFiles) are in CC:T's test/testFixtures source sets, not in the published jars. The driver is hard-wired to a CobaltLuaMachine subclass using Cobalt DebugHook for coverage. Some specs diverge by design: vm_spec needs 2^19 unpack, coroutine_spec yields inside debug hooks, error_hints_spec needs cc.internal.error_info, timeout_spec needs patch 07 and exact messages. There is no baseline-under-Cobalt step, no expected-failures mechanism, no computer_threads>1 run, no long soak (reboot/chunk-unload churn for native and JNI leaks), no ASan build, and no add-on compatibility matrix. 5–8 days is optimistic.

**Evidence.** gh api tree of cc-tweaked/CC-Tweaked@mc-1.21.x: 88 .lua under test-rom (saved work/review-completeness/dl/tree.json); tag v1.21.1-1.121.0 exists. dl/ComputerTestDelegate.java:110-118 (`ComputerContext.builder(environment).luaFactory(CoverageLuaMachine::new)`, mounts startup `loadfile('test-rom/mcfly.lua', nil, _ENV)('test-rom/spec')`), :301-330 Coverage machine uses org.squiddev.cobalt.debug.DebugHook. dl/timeout_spec.lua expects `^.+:%d+: Too long without yielding$` for loops and exact 'Too long without yielding' for string.find/match. dl/vm_spec.lua `{ (" "):rep(2^19):byte(1,-1) }`. dl/ComputerTest.java expects '/test.lua:1: Too long without yielding'.

**Suggested text.**

**WP H (revised, experienced 8–12 d / newcomer 15–24 d).**
- (1) Vendor, under MPL-2.0 with headers, CC:T `projects/core/src/test/resources/test-rom` and `ComputerTestDelegate.java` + `testFixtures/.../BasicEnvironment.java` + TestFiles at tag `v1.21.1-1.121.0`. Replace `CoverageLuaMachine::new` with a parameter: `cobalt` | `luajit` | `luajit-nojit`. Use the CC:T core jar + POM deps; skip `:http` keywords offline via `-Dcc.skip_keywords`.
- (2) Run under Cobalt first (must be 100%). Then run LuaJIT against a checked-in `expected-failures.txt` (test name + reason + issue link). CI fails on new failures AND on unexpected passes. Pre-triaged:
  - vm_spec (unpack limit);
  - coroutine_spec 'within debug hooks';
  - error_hints_spec;
  - timeout_spec until patch 07.
- (3) Port CC:T `ComputerTest.testTimeout` (exact message).
- (4) Run everything with `computer_threads` = 1 and 4.
- (5) Soak, 24 h, Linux x64 + arm64, inside a container with a memory limit: 200 computers running a chaos script (random reboot/shutdown, chunk unload/reload, OOM, soft/hard abort, fs/http/websocket handle churn without close, deep coroutines, cyclic queueEvent). Assert:
  - no JVM crash;
  - live native states == running computers;
  - global native bytes back to baseline after all shut down;
  - Java handle tables bounded;
  - RSS stable;
  - `/proc/self/maps` count stable.
- (6) An ASan/UBSan build of libcclj (LUAJIT_USE_SYSMALLOC, `-joff`) for the fuzzers. Differential fuzzing of string.format/patterns/tostring among JIT on, JIT off and Cobalt.
- (7) Add-on matrix, in-game on NeoForge + Fabric: Advanced Peripherals, UnlimitedPeripheralWorks, Create (CC integration), Tom's Peripherals; plus CCGraphics/cride to verify the incompatibility warning.
- (8) `-Xcheck:jni` on all JVM tests.

## 13. [should-fix] P11/P13 (ROM compatibility) and WP B

**Problem.** `unpack` and `string.byte(s,1,-1)` fail beyond 8,000 values (LUAI_MAXCSTACK=8000). Common CC idioms such as `string.char(table.unpack(bytes))` and `{s:byte(1,-1)}` on strings over 8 KB throw 'too many results to unpack' / 'string slice too long'. CC:T explicitly tests 2^19 values (vm_spec). The plan's divergence list does not mention it. Raising LUAI_MAXCSTACK is a one-line build change that helps a lot but cannot reach 2^19 (LUAI_MAXSTACK is 65500 per coroutine).

**Evidence.** probes/p1.lua on the combined build: byte 2^19 → 'string slice too long'; unpack 8001 → 'too many results to unpack'; byte 7999 ok. I rebuilt a copy (work/review-completeness/lj-maxcstack) with `LUAI_MAXCSTACK 65000`. probes/p6.lua: string.char(unpack) ok at 8001/30000/60000/64000; byte(1,-1) ok to 30000 and 'stack overflow' (catchable) at 60000; 2^19 still refused; JIT loop with 20k-value unpack ×200 correct; deep recursion still bounded.

**Suggested text.**

Add patch 09 (`luaconf.h`: `LUAI_MAXCSTACK 65000`) to the stack.
- Validate with the assert build (`-DLUA_USE_ASSERT -DLUA_USE_APICHECK`), the preemption harness and the fuzzers.
- Document the remaining divergence: over ~60k values per unpack/byte call errors, while Cobalt allows at least 2^19. Add vm_spec to expected-failures with that reason.
- List it in the P13 incompatibility table shown to players.

## 14. [should-fix] §4.4 Conversion rules / IArguments and P5 protected boundary

**Problem.** Several conversion-boundary hazards are not covered.
(a) Eager ObjectArguments converts every table argument even when the method never reads it (cost O(table) per call). Its default getStringCoerced calls `hashCode()` on the converted Map, so `term.write(t)` with a self-referencing table throws StackOverflowError on the worker thread (Cobalt prints 'table: …'). CC:T then logs a full stack trace per call (log flood).
(b) Lua→Java conversion has no size budget. 256 queued events × a large table converted to boxed HashMaps can exhaust the JVM heap, and LuaJIT builds such tables 9x faster outside -Xmx. CC:T's IArguments contract explicitly allows throwing LuaException 'if the conversion would allocate too much memory'.
(c) Top-level error objects and `getType` must not run metamethods (`__tostring`/`__index`) from host code. Cobalt uses rawToString.
(d) ByteBuffer semantics: direct buffers (fs reads use allocateDirect), arrayOffset/position, and position advance are not stated.
(e) No Lua API call that can raise may run while a JNI resource is held (critical arrays, Get*ArrayElements, pushed local frames).

**Evidence.** IArguments.java:173-185 default getStringCoerced → `String.format("%s: %08x", getType(index), value.hashCode())`; IArguments.java:165-167 javadoc on LuaException for excessive allocation. probes/SelfMap.java: HashMap containing itself → StackOverflowError. ResultInterpreterFunction.java:59-61 logs each Throwable with stack. cobalt LuaError.java:171-175 rawToString (no __tostring). AbstractHandle.java:149 `ByteBuffer.allocateDirect`; WebsocketHandler.java:58-66 events carry byte[]. cct-contract §6 'ByteBuffer (remaining bytes, advances position)'.

**Suggested text.**

**Conversion rules (append to §4.4).**
- Implement IArguments lazily over the native argument snapshot, like VarargArguments. Convert a table only when get/getTable/escapes asks for it.
  - Override getType (honour `__name` via rawget on the metatable), getStringCoerced/getBytesCoerced (Lua `%.14g`-style numbers and 'table: <id>' with no Java hashCode), and getTableUnsafe.
  - `escapes()` converts eagerly in place.
- Lua→Java conversion is iterative, identity-preserving and raw-only (lua_rawget/lua_next, never metamethods), with a budget of `max_conversion_values` nodes per call. Exceeding it raises LuaException 'table too large to convert'.
- Java→Lua is iterative with depth ≥ 512 (Minecraft NBT depth). The node budget is enforced by the memory cap under protection.
- Errors that escape the bios are stringified raw: string as-is, else '<type>: <id>'. Truncate to 64 KiB.
- ByteBuffer → string copies `remaining()` bytes from `position` (heap: array + arrayOffset; direct: GetDirectBufferAddress) and advances position, as Cobalt does. `byte[]` is copied verbatim.
- JNI phase rule: gather Lua values into C buffers under protection → do all JNI calls with no Lua API calls → release arrays and pop the local frame → push results under protection. Never hold Get*Critical across any call.

## 15. [should-fix] §4.2 Native layer / §6 Risks (platforms) / P8 (patch 08)

**Problem.** Several platform-specific issues are missing.
(1) musl/Alpine is not detected. Some Docker server images are Alpine-based. A glibc .so either fails to load (fine, fallback) or, with gcompat present, may load and fail later.
(2) The LuaJIT profiler (SIGPROF + setitimer, process-global state) and vmevent callbacks (uninterruptible jit.attach) are still compiled in. They are unreachable once `jit` is removed, but compiling them out is free defense in depth and avoids SIGPROF clashes with spark/async-profiler.
(3) Patch 08's margin is unspecified. HotSpot reserves yellow/red/reserved guard pages plus a shadow zone (~20 pages on x64/aarch64) that JNI upcalls bang. These scale with page size (16 KB on macOS arm64, 64 KB on some aarch64 kernels). Successive handleEvent calls run on different worker threads, so stack bounds must be recomputed per entry.
(4) Preemption (patch 01) was validated only on x64. ARM64 Linux (popular for hosted servers) and macOS arm64 are untested.

**Evidence.** port-mechanics.md:158 'musl/Alpine is not covered'; scaffold NativeLoader.platformId only checks os.name/os.arch. lj_arch.h:595-598 (`LUAJIT_DISABLE_PROFILE`, else POSIX `LJ_PROFILE_SIGPROF`); lj_vmevent.h:34 `LUAJIT_DISABLE_VMEVENT`; luajit-runtime.md §j jit.attach hang. cct-contract §3.1: handleEvent 'possibly a different thread each call'. luajit-runtime.md §5: arm64/macOS not tested.

**Suggested text.**

§4.2 additions:
- (1) Add `-DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT` to the build flags, and keep the LUAJIT_SECURITY_* defaults (assert `jit.security()` at startup before removing `jit`).
- (2) NativeLoader: on Linux, detect musl (`/proc/self/maps` contains 'ld-musl', or `/lib/ld-musl-*.so.1` exists). If found, skip natives and fall back to Cobalt with an INFO message. A musl x64/arm64 build is optional.
- (3) Patch 08 / host: at every native entry, compute the current thread's stack bounds (Windows `GetCurrentThreadStackLimits`, Linux `pthread_getattr_np`, macOS `pthread_get_stackaddr_np`/`pthread_get_stacksize_np`). Set the guard to stop C recursion when remaining < max(256 KiB, 32 × page size), which leaves room for HotSpot's guard + shadow zones and for Java peripheral code called through JNI.
- (4) CI runs the 25-shape preemption harness, the C-stack harness and the JVM smoke test natively on linux-arm64 and macos-arm64 runners, not just a load test.
- (5) If dlopen fails with a noexec or 'failed to map segment' error, log a hint to move the game dir or set `-Dccluajit.nativeDir`.

## 16. [should-fix] §4.2 'Pin LuaJIT to 3c4f9fe2' and §6 'Maintaining a LuaJIT patch stack'

**Problem.** Upstream has no build switch to disable the v3.0 syntax merged into v2.1. Pinning before it therefore turns the pin into a permanent fork that must cherry-pick security and correctness fixes by hand. The plan treats this as 'each LuaJIT bump needs a rebase' and has no intake process. Upstream v2.1 has had a steady stream of JIT/VM fixes since the pin, some directly relevant to this design: OOM during trace stitching (the allocator cap makes OOM routine), select() beyond varargs, and an ARM64 `__metatable` getmetatable bug. Which of them apply to the pin needs triage.

**Evidence.** `gh api repos/LuaJIT/LuaJIT/commits?sha=v2.1&since=2026-07-11`: c5569108 'Handle OOM error during trace stitching', a4c06453 'Don't compile select() indexing beyond varargs', 95f931cb 'Check for IR_NEWREF in IR_ALEN forwarding', 27f169c6 next() recording fix, ce8ec8dc 'ARM64: Fix getmetatable() for cleared __metatable field', 2c9f5cf6 'ARM64: Fix BC_JFORI on empty FP range', bc9261a3, 849bbbe7/acf8fcbd floor/ceil arg check, 2a8e017e, ea9eb7ed; HEAD lj_parse.c has `TK_continue` with no disabling define (work/rebase-ea9eb7ed/src/lj_parse.c:1110, :3053).

**Suggested text.**

§6 replacement: **LuaJIT is a maintained fork.**
- Branch `cclj/v2.1-pre3` = 3c4f9fe2 + patches 01–09.
- Monthly, and on any upstream commit touching lj_record/lj_asm*/lj_opt*/lj_err/lj_state/lj_gc/lib_*: triage each v2.1 commit since the last sync as apply / not-affected / syntax-only. Cherry-pick the applicable ones (first batch to triage now: c5569108, a4c06453, 95f931cb, 27f169c6, ce8ec8dc, 2c9f5cf6, bc9261a3, 849bbbe7, 2a8e017e, ea9eb7ed). Re-run all harnesses and fuzzers before release.
- Long-term alternative to evaluate in M3: track upstream HEAD with a small 'syntax gate' patch that makes the lexer reject v3.0 tokens (continue/?./+=/bit operators/const), so upstream fixes come for free.
- Budget 0.5–1 d/month of maintenance.

## 17. [should-fix] P13 / §4.3 Curated environment (address disclosure)

**Problem.** LuaJIT `tostring(table|thread|userdata)` and `string.format('%p', v)` print raw heap addresses. Cobalt prints an identity hash and has no %p. Since the plan says 'every native bug is a server crash', leaked addresses defeat ASLR and turn a memory-safety bug into an exploit. `tostring(print)` also shows 'builtin#N' instead of 'function: <hex>' (a minor parity difference).

**Evidence.** probes/p1.lua on the combined build: `string.format('%p', {})` → `0x0211a1bca528`; `tostring({})` → `table: 0x0211a1bcc1a8`; `tostring(coroutine.create(print))` → `thread: 0x0211a1bcd240`; `tostring(print)` → `function: builtin#30`. cobalt LuaValue.java:260 `typeName + ": " + Integer.toHexString(hashCode())`; cobalt StringFormat has no 'p' conversion.

**Suggested text.**

Patch 10 (or host wrapper): `tostring`/`luaL_tolstring` for GC objects prints `<type>: %08x`, where the value is a 32-bit hash of (address XOR a per-state random key from SecureRandom), matching Cobalt's format. `string.format('%p')` raises 'invalid conversion' as in Cobalt. Functions print `function: %08x` the same way.

## 18. [should-fix] §4.1 Native loading / §6 Risks (distribution)

**Problem.** The plan does not consider antivirus or platform-moderation friction. An unsigned DLL built with static MinGW and containing LuaJIT, extracted into a game directory and loaded at runtime, closely resembles current malware: LuaJIT-based loaders spread as game cheats and tools are an active 2024–2026 campaign family. AV quarantine between extraction and System.load surfaces as an UnsatisfiedLinkError with no explanation.

**Evidence.** Web: Netskope 'TroyDen's Lure Factory' (LuaJIT-based infostealers via 300+ GitHub packages, gaming cheats); Morphisec Lua malware targeting student gaming engines; ANY.RUN SmartLoader reports (2025). scaffold NativeLoader extracts into `<gamedir>/.ccluajit/natives/<sha256>/`.

**Suggested text.**

Release plumbing (WP A/G):
- Reproducible native builds with published SHA-256s.
- VirusTotal scan of each release DLL/.so/.dylib in CI (fail on >2 engines).
- PE version resource with product name, source URL and version. No packers.
- Pursue free OSS Authenticode signing (e.g. SignPath Foundation) and notarize macOS dylibs if feasible.
- If load fails, NativeLoader checks whether the extracted file vanished or was locked and logs: 'native library may have been quarantined by antivirus; see <docs link>; falling back to Cobalt'.
- Document the cache path for AV exclusions.
- Mod-page description states plainly that the jar contains native code, and links to the source and build workflow.

## 19. [nice-to-have] §6 Risks / release process

**Problem.** A sandbox escape in this mod means remote code execution on every server running it, yet there is no security-reporting channel or incident plan. The only emergency lever is removing the mod, because the config switch needs a restart.

**Evidence.** PLAN.md §6 'Every native bug is a server crash'; prior-art.md §3 (Luanti CVE-2026-41196 CVSS 10.0, corsix bytecode→native exec); no SECURITY.md in the repo (`ls` of the repo root: LICENSE, README.md, build files only).

**Suggested text.**

Add SECURITY.md with GitHub private vulnerability reporting enabled, a 90-day disclosure policy and supported-versions table. Keep an incident playbook: `/ccluajit disable` (live) → patch release → advisory. Offer a 'secure defaults' statement in the README listing what is removed (bytecode, debug.sethook, jit/ffi/io/os, getregistry) and the caps.

## 20. [nice-to-have] §4.1 Mod / loader layer

**Problem.** CCLuaJIT is server-logic-only (it registers no payloads or registries), but the plan does not say whether clients need it. Requiring it client-side for multiplayer adds friction and puts natives on every client.

**Evidence.** cct-contract §10: only ServerContext (server start) uses the factory; no client-side computers. scaffold neoforge.mods.toml dependencies `side="BOTH"`; scaffold loads natives lazily on first boot.

**Suggested text.**

Declare the mod server-side optional. NeoForge: `displayTest="IGNORE_ALL_VERSION"` in neoforge.mods.toml. Fabric: environment '*' with no client entrypoint. Document that players do not need it to join, and that single-player uses it through the integrated server.
