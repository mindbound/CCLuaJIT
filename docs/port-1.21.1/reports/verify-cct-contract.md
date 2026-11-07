# Adversarial verification of the `cct-contract` findings (CT-1 to CT-7)

Verifier key: `verify-cct-contract`. Work dir (abbreviated `VW`):
`<scratch>\work\verify-cct-contract`.
Sources: CC:T 1.121.0 sources in `...\scratchpad\cct\src-1.21.1-1.121.0` (`SRC121`), Cobalt 0.9.9 sources in `...\scratchpad\cct\cobalt-src`,
ROM in `...\scratchpad\cct\rom-1.21.1\data\computercraft\lua` (`ROM121`). LuaJIT 2.1 ROLLING (GC64, MinGW) copied to `VW\lj`.
Nothing was written inside `<repo>`; only `git show` was used there.

## Method

I re-read every cited source location myself. I then wrote my own probes; I did not reuse the investigator's probes, except
the boot harness, which I reviewed line by line and then re-ran from a copy:

| Probe (in `VW\scripts`) | What it does | Output (in `VW\out` unless stated) |
|---|---|---|
| `WedgeProbe.java` | Runs the **real CC:T 1.121.0 `ComputerThread`/`ComputerExecutor`** (core jar + Guava/slf4j/fastutil from the Gradle cache) with a fake `ILuaMachine` whose first `handleEvent` spins and ignores `Thread.interrupt()`, like native code. It logs every machine call with time and thread. After 14 s it tries shutdown/queueEvent/turnOn/reboot, then releases the spin, then calls `ctx.close(1 s)`. | `wedge.txt`, control run with 0.5 s spin: `wedge_control.txt` |
| `ThreadsProbe.java` | The same harness with 4 computer threads and 2 computers, 200 events each. It records which threads ran `handleEvent`. | `threads.txt` |
| `VCobalt.java` | Cobalt 0.9.9 set up like `CobaltLuaMachine`: no `bytecodeFormat`; interrupt handler: hard -> `Error`, first soft -> `LuaError`. A timer thread sets soft/hard abort and calls `state.interrupt()`. | `cobalt_interrupt.txt`, `bytecode.cobalt.txt`, `env.cobalt.txt`, `nul.cobalt.txt` |
| `VCobaltPause.java` | Reference: Cobalt SUSPEND (= CC:T PAUSE) 20 times inside a nested coroutine, resumed the way `CobaltLuaMachine` does it. | `cobalt_pause.txt` |
| `v_pattern.lua`, `v_jitloop.lua` | LuaJIT: does an erroring count hook stop `string.find` or a JIT-compiled loop? | `luajit_interrupt.txt` |
| `vpause.c` | LuaJIT C host: PAUSE by `lua_yield` from a count hook. Cases: nested coroutine, `table.sort` comparator, `gsub` callback, inside `pcall`, top level. | `vpause.txt` |
| `vtramp.c` | LuaJIT: what a C function that does `return lua_yield(...)` sees on resume, and a Lua trampoline for `MethodResult` callbacks. | `vtramp.txt` |
| `bytecode.lua`, `bc_mutate.lua`, `bc_run.lua` | Bytecode: `string.dump`, `load`/`loadstring`/reader with each mode. Fuzz: 90 single-byte mutations of dumped bytecode. | `bytecode.luajit.txt`, `bytecode.cobalt.txt`, `bc_fuzz.txt` |
| `v_prelude_bypass.lua` | Loads the investigator's `cclj_prelude.lua` into a sandbox. Sandboxed code then uses `debug.getupvalue(load, i)` to get the native `load`. | `prelude_bypass.txt` |
| `vhooksteal.c` | The C host sets a timeout count hook. Sandboxed code in a nested coroutine calls `debug.sethook()`. | `hooksteal.txt` |
| `v_env.lua`, `v_nul.lua` | 5.2 features and NUL-in-pattern behaviour, LuaJIT vs Cobalt. | `env.*.txt`, `nul.*.txt` |
| `boot_harness.lua` + `cclj_prelude.lua` (copied from the investigator, reviewed) | Re-ran the boot of the real 1.121.0 bios + ROM, the ablations, the REPL and `help` cases. | `VW\boot\{noprelude,full,ablation,repl,help}.txt` |

Build and run commands (Git Bash; `S=...\scratchpad`, `W=VW`):
```
gcc -O1 -I$S/luajit-win/src -o $W/lj/vpause.exe vpause.c $W/lj/lua51.dll   (same for vtramp.c, vhooksteal.c)
java -cp cobalt-0.9.9.jar VCobalt.java <file.lua> <softMs> <hardMs>
javac -d cls -cp "$(cat cp.txt)" WedgeProbe.java && java -cp "cls;$(cat cp.txt)" WedgeProbe 1000   # and 0.5 for control
./luajit.exe ../scripts/boot_harness.lua $ROM121 ../boot/listing.txt ../scripts/cclj_prelude.lua "ls rom" "id"
CCLJ_SKIP=env|table52|utf8|bit32 ./luajit.exe ../scripts/boot_harness.lua ...
```

## CT-1 - non-abortable native machine wedges the computer: CONFIRMED (critical), with corrections and additions

Code re-read: `ComputerThread.java:510-548` (`checkRunners`), `:446-472` (`workerFinished`), `:379-393` (normal `afterWork`
requeues; `workerFinished` calls `executor.afterWork()` at `:454` and ignores the result), `ComputerExecutor.java:254-263`
(`immediateFail` -> `machine.close()` then `enqueue()`, which turns RUNNING into REPEAT), `ManagedTimeoutState.java:41-60`,
`TimeoutState.java:46-61`, `ServerContext.java:104`, `ThreadUtils.java:75` (daemon). Cobalt: `LuaInterpreter.java:177`,
`LuaC.java:205`, `StringMatch.java:513,528`, `LuaState.java:79` (`private volatile boolean interrupted`),
`CobaltLuaMachine.java:60-68,321` (`HardAbortError extends Error`).

**Empirical, real CC:T scheduler (`wedge.txt`)**:
```
  0.12s [Worker-0]  handleEvent#1(null) ENTER
  7.15s [Monitor-0] listener: soft=true
  8.66s [Monitor-0] listener: hard=true ; close() call #1 (handleEvent running=true)
  8.77s / 8.87s ... [Monitor-0] close() call #2, #3 ... #20 at 10.71s      <- repeated every monitor tick
 10.16s [Monitor-0] printExecutionState (handleEvent running=true) + "Terminating computer #1 due to timeout"
 11.64s executor state -> ON_QUEUE, isOn=true                              <- worker abandoned
 14.09s shutdown(); queueEvent("key"); turnOn()  -> 3 s later: ON_QUEUE, isOn=true, no new handleEvent
 17.10s spin released: handleEvent RETURN (interrupted flag=true) -> still ON_QUEUE; reboot() -> no effect
 20.63s ComputerThread.workerCount=2 (workers array length 1)
 21.65s ctx.close(1s) -> false ; live thread: ComputerCraft-Computer-Monitor-0 TIMED_WAITING
```
Control with a 0.5 s spin (`wedge_control.txt`): shutdown, turnOn and reboot all work; `ctx.close(1s) -> true`.

LuaJIT side (`luajit_interrupt.txt`): with JIT off and a count hook (every 100 instructions) that errors after 1 s,
`string.find(("a"):rep(n), ".-.-.-b")` ran 2.15 s (n=400) and 52.6 s (n=800) with **0 hook calls**. The run time scales
roughly as n^4, so n=6000 would take hours. A `for` loop with JIT on: hook fired 0 times over 3e9 iterations. JIT off: the hook
aborted at 0.50 s. Cobalt (`cobalt_interrupt.txt`): the same `string.find` is stopped by the soft abort at ~1 s
(`probe:1: Too long without yielding`, which `pcall` catches). The hard abort then escapes `pcall(function() while true do end end)`.

Corrections:
* "can never be turned on or off again until restart" is slightly too strong. That `Computer`/executor instance is wedged:
  shutdown, turnOn, reboot and queueEvent all fail, even after the native call returns (verified). But unloading the block
  entity (`AbstractComputerBlockEntity.unload` -> `ServerComputer.close` -> `registry.remove`), for example by a chunk unload
  or by breaking the block, creates a new `ServerComputer`/executor. The leaked thread keeps spinning at 100 % of one core
  until JVM exit, and keeps its VM memory and the old filesystem mount (`shutdown()` never ran).
* Additional CC:T bug found here, which makes it worse: `workerFinished` calls `addWorker(index)` (which already does
  `workerCount++`, `:181`) and then `workerCount++` again (`:465`). After any abandonment `workerCount` is 1 too high for
  good. Every later `ComputerThread.stop()` therefore returns false ("Failed to stop computers under deadline"), the state
  never reaches CLOSED, and the Monitor thread leaks (verified: `workerCount=2` with 1 slot, monitor still TIMED_WAITING).
  Cobalt practically never reaches this code path; a non-interruptible native machine reaches it with every stuck program.
* `close()` is called on **every monitor tick** after 8.5 s, not once (about 30 calls in 3 s), from the Monitor thread, while
  `handleEvent` runs.
* Cobalt is not interruptible everywhere either: only the interpreter loop, the compiler reader and pattern matching check
  the flag. Other Java library calls (e.g. `table.sort` without a comparator, `string.rep`) are bounded by memory or are fast.
  This does not change the conclusion.

## CT-2 - bytecode: CONFIRMED (critical), plus a mitigation gap

`CobaltLuaMachine.java:59-74` builds the `LuaState` without `.bytecodeFormat(...)`. `StringLib.java:244-249` and
`LuaC.java:166-171` then refuse dump and binary chunks. In 1.20.1 `main.cpp` `initMachine` (lines ~701-750) opens
base/math/string/table/bit and removes only `collectgarbage/gcinfo/newproxy`; `string.dump` and bytecode-capable
`load`/`loadstring` stay.

`bytecode.cobalt.txt`: `string.dump` -> `unable to dump given function`. Every `load`/`loadstring`/reader variant with mode
nil/"b"/"bt"/"t" -> `attempt to load a binary chunk`.
`bytecode.luajit.txt`: `string.dump` works (67 bytes). `load(bc)`, `load(bc,n,"b")`, `load(bc,n,"bt")`, `loadstring(bc)` and
a reader function all load and run it (`from bytecode`). Only mode `"t"` rejects it.
Memory safety (`bc_fuzz.txt`): 90 single-byte mutations of a dumped function, each loaded with plain `load()` and run.
11-12 of them crash the process with SIGSEGV (rc=139). In a server this is a JVM crash at minimum. Arbitrary read/write from
crafted LuaJIT bytecode is publicly documented, but I did not reproduce a full read/write primitive. I also checked LuaJIT's
lexer (`lj_lex.c:416-441`): bytecode after a BOM or `#!` header is refused, so a first-byte check is not bypassed that way.

**Gap (missed):** the investigator's `cclj_prelude.lua` blocks bytecode in Lua wrappers that keep the native `load` as an
upvalue. CC:T exposes the full debug library (Cobalt `DebugLib.java:78-94`: getupvalue, setupvalue, upvaluejoin, getregistry,
sethook, ...), and the ROM needs `debug.getupvalue` (`cc/internal/error_hints.lua:124`). `v_prelude_bypass.lua` output:
```
string.dump after prelude: nil
load(EVIL) via prelude wrapper: attempt to load a binary chunk
debug.getupvalue(load, 2) = 'native_load' -> loaded bytecode; running it: bytecode executed inside the sandbox
```
The bytecode must therefore be refused natively: patch LuaJIT's loader, or register C `load`/`loadstring` that always pass
mode "t". No privileged function may be reachable through upvalues (of Lua or C closures) or through the registry.

## CT-3 - PAUSE via hook-yield: CONFIRMED (high)

Code: `ComputerExecutor.java:517-519,557-559`, `ManagedTimeoutState.java:41-60`, `ComputerThread.java:162-170,410-414`
(LATENCY_MAX_TASKS = 50/5 = 10, so 25 ms with 1 queued and 5 ms with 9+ queued at 1 thread), `ConfigSpec.java:167`
(default 1 thread), `CobaltLuaMachine.java:67,128-133`.

`vpause.txt` (LuaJIT, JIT off, hook on the top-level thread):
* nested: `[hook] yielding from a NESTED coroutine` (this also proves the hook is global per state) -> user scheduler sees
  `coroutine.resume returned n=1 ok=true val=nil status=suspended`; the machine gets `status=1 nresults=1 first=nil`. The
  next event (`some_event`) goes to the scheduler, which passes it into the child; the hook-yield discards it, and the child
  later waits for `child_filter`. The event is lost.
* sort / gsub: `attempt to yield across C-call boundary`, raised in user code (catchable by `pcall`). This happens even at
  top level.
* pcall and top level: transparent.
Reference (`cobalt_pause.txt`): 20 SUSPENDs during the nested loop. The user scheduler saw exactly one resume return
(`child_filter`), and the event was delivered: `child got event: some_event`.

Nuance: the machine can tell that a hook-yield happened (its own flag), but it cannot hide it from the Lua schedulers in
between (parallel, multishell), which already saw a spurious resume. If the machine then returns PAUSE, every unfiltered
coroutine gets an empty event. If it returns OK, the next event is lost. Never returning PAUSE is legal under the contract,
so "high" reflects the fairness cost (one busy computer can hold the single default worker for up to 7 s per event), not
a hard blocker.

## CT-4 - yielding Java methods need a Lua trampoline: CONFIRMED (high), now empirically

Re-read: `ResultInterpreterFunction.java:52-102` (`LuaThread.yield(state, ret)` from the current coroutine; `resume` ->
`toObjects` -> `callback.resume`, loop; `wrap` level adjust), `MethodResult.java:89-97,109-112,124-127,149-153`,
`TaskCallback.java:17-59`, `core/asm/LuaMethodSupplier.java:32-35` (`args.escapes(); context.executeMainThreadTask(...)`),
`PeripheralAPI.java:323` (`adjustError(1)`). `ILuaContext` in both 1.115.0 and 1.121.0 has only `issueMainThreadTask` and
the default `executeMainThreadTask`. 31 `mainThread = true` methods in 10 files (CommandAPI, command block, disk drive,
energy/fluid/inventory generics, printer, pocket, turtle API, turtle storage). `turtle_response` comes from
`TurtleBrain.java:636-643`. Websocket uses `MethodResult.pullEvent` (`WebsocketHandle.java:141`). LuaJIT `lua.h` has only
`lua_yield` (no `lua_yieldk`/`lua_callk`/`lua_pcallk`).
`vtramp.txt`: after `return lua_yield(L,1)`, the Lua caller gets the raw resume value (`task_complete`), and the C code after
the yield never runs (`C code after lua_yield ran: 0`). A Lua trampoline (native returns a marker + handle; Lua yields and
calls `native_resume`) handled a callback that yielded 3 times and finished
(`trampoline result: callback finished after 3 resumes`).
1.14.4 design (`git show origin/1.14.4_CCT:.../LuaJITMachine.java:219-271`): it blocks the worker in `yield`/`pullEvent`
on a monitor. Also noticed: `isSpecialEvent` checks `"task_completed"`, but `taskCompleted` queues `"task_complete"`
(lines 40 vs 298). Main-thread task completions were therefore never routed to the blocked waiter. This was not executed
(old CC:T not available), so whether main-thread methods ever worked on 1.14.4 is low confidence.

## CT-5 - ROM needs _ENV/bit32/table.pack/unpack/utf8; prelude boots: CONFIRMED (high), with corrections

Counts (1.21.1 ROM, 105 files): `_ENV` 18 in 15 files; `table.pack` 25; `table.unpack` 39; `bit32.` 26 occurrences on 20
lines in 5 files; `utf8.` 5. 1.20.1 ROM: 18/19/34/25/2, and `diff -w` of the two bios files is empty (whitespace-only).
`env.luajit.txt` vs `env.cobalt.txt`: LuaJIT has no bit32/utf8/rawlen/table.pack/table.unpack, `load('return _ENV',..,env)()`
is nil, and `bit.band(0xFFFFFFFF,0xFFFFFFFF)=-1` and `bit.lshift(1,31)=-2147483648` (Cobalt: 4294967295 / 2147483648).
Boot (re-run myself, `VW\boot`): no prelude -> `bios.lua:23: attempt to index global 'bit32'`. Full prelude -> CraftOS 1.9
prompt, `ls rom` and `id` work. Skip env -> `Failed to load API colours due to /rom/apis/colours.lua:13: attempt to index
local 'colours'`. Skip table52 -> `/rom/apis/parallel.lua:72: attempt to call field`. Skip utf8 -> boots. Skip bit32 -> bios:23.
bios `:458-466` (strict-globals pre-sets `tEnv._ENV`) and `:515` (`if k ~= "_ENV"`) confirmed.
Corrections:
* "stock LuaJIT lacks all of them" is true for the default build only. Building with `-DLUAJIT_ENABLE_LUA52COMPAT`
  (`lj_arch.h:693-697`) adds `table.pack`, `table.unpack`, `rawlen`, and `__pairs`/`__ipairs`/`__len` for tables (and Lua 5.2
  `coroutine.running`). It does not add `_ENV`, `bit32` or `utf8`.
* `bit32` is used 26 times on 20 lines (the investigator's "20" counts lines).
* The harness only mocks term/fs/os/peripheral/redstone, so "boots" means the Lua side only.

## CT-6 - NUL in patterns: CONFIRMED (high), and broader than stated

`lib_string.c:189-208` (`classend`: `if (*p == '\0') lj_err_caller(... LJ_ERR_STRPATM)`). `textutils.lua:448` (the
default, non-unicode branch of `serializeJSONString`) and `help.lua:42-43` contain NUL in bracket classes.
`nul.luajit.txt`: both patterns -> `malformed pattern (missing ']')` for any subject string. Cobalt: `abc 0`, `a<10>b 1`,
`3 4`, `1 2`. Re-ran in the boot harness: `textutils.serialiseJSON('abc')` -> `/rom/apis/textutils.lua:448: malformed
pattern (missing ']')`; `help credits` -> `/rom/programs/help.lua:53: malformed pattern (missing ']')`.
Broader: outside bracket classes LuaJIT silently truncates the pattern at NUL.
`string.match('ab\0cd','b\0c')` returns `"b"` in LuaJIT and `"b\0c"` in Cobalt, so some user code gets wrong results with no
error. Plain-text cases (`find(s,"\0")`, `%z`) work.

## CT-7 - close/printExecutionState concurrency, repeated close, listener lifecycle: CONFIRMED (high), now empirically

Code re-read (all cited lines match): `ComputerExecutor.java:254-263, 422-449, 528-534, 552-563`;
`ComputerThread.java:532, 642-646, 681, 620`; `CobaltLuaMachine.java:95, 118, 139-148, 159-163`;
`TimeoutState.java:113-122`; `ManagedTimeoutState.java:94, 116`; `ExecutorImpl` makes one `TimeoutImpl` per executor
(`ComputerThread.java:773-777`), which is handed to every machine (`ComputerExecutor.java:371`).
Empirical (`wedge.txt`): `close()` about 30 times from `Computer-Monitor-0` while `handleEvent running=true`;
`printExecutionState` twice from the Monitor thread while running; listener callbacks on the worker (addListener and reset)
and on the Monitor (soft/hard). `threads.txt`: the same machine's `handleEvent` ran on `Worker-0` and `Worker-1`.
Error path: Cobalt closes itself and `resumeMachine` -> `shutdown()` closes again (control run shows a normal single close
on shutdown).
Context: the 1.14.4 branch's `close()` calls `deinitMachine()` -> `lua_close` immediately. If that close runs on the monitor
thread during a running call, this is a use-after-free that would crash the JVM. This is likely but not executed here.

## Missed issues (this lane)

1. **(critical) The user can disable the timeout hook.** LuaJIT hooks are per `global_State`, and CC:T exposes
   `debug.sethook` (Cobalt `DebugLib.java:88`). `vhooksteal.txt`: host hook alone -> `Too long without yielding` at 0.50 s.
   After a nested coroutine calls `debug.sethook()` -> the 3e9-iteration loop runs to completion (6.27 s), with the host hook
   fired 0 times. A hook-based abort or pause therefore needs `debug.sethook`/`gethook` replaced (the ROM does not use them)
   or a per-coroutine emulation.
2. **(high) Security shims written in Lua are reachable through the debug library** (`v_prelude_bypass.txt`: native `load`
   recovered with `debug.getupvalue(load, 2)`, bytecode executed). Bytecode refusal and any other privileged function must
   be native, and must not be reachable via upvalues (Lua or C closures) or `debug.getregistry()`.
3. **(medium) CC:T `workerFinished` double-increments `workerCount`** (`ComputerThread.java:181` + `:465`). After the
   first abandoned worker, every `ComputerThread.stop()` fails and the Monitor thread leaks (verified). This is CC:T's own
   bug, but only a non-interruptible machine reaches it in practice.
4. **(medium) Silent pattern mis-matches with NUL** (see CT-6), not only `malformed pattern` errors.
5. **(info) `LUAJIT_ENABLE_LUA52COMPAT`** removes part of the prelude (table.pack/unpack, rawlen, `__pairs`/`__len`).
6. **(info/low confidence) 1.14.4 event-name mismatch** `task_completed` vs `task_complete` in its blocking main-thread path.
