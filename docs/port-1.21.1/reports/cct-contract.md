# CC:Tweaked machine contract for a LuaJIT `ILuaMachine` (CC:T 1.121.0 / MC 1.21.1 vs 1.115.0 / MC 1.20.1)

Lane: `cct-contract`. Investigator work dir:
`<scratch>\work\cct-contract`
(below abbreviated `WORK`). CC:T sources: `...\scratchpad\cct\src-1.21.1-1.121.0\dan200\computercraft` (abbreviated `SRC121`)
and `...\src-1.20.1-1.115.0\...` (`SRC115`). ROM: `...\scratchpad\cct\rom-1.21.1\data\computercraft\lua` (`ROM121`).
Nothing was written inside `<repo>` (read-only `git show` only).

## 0. Headline conclusions

1. **The Java-side machine contract did not change between 1.115.0 and 1.121.0.** `ILuaMachine`, `ILuaMachine.Factory`,
   `MachineEnvironment`, `MachineResult`, `MachineException` are binary-identical (verified with `javap` on both jars; only
   nullness annotations moved from `javax.annotation` to `org.jspecify`). `ServerContext.luaMachine` is still a public,
   non-final static read once per server start. The port's Java-side machine API work is therefore small; the hard work is
   the semantics below, which the 1.20.1 branch never implemented (its machine is a stub).
2. **The one binary-breaking API change that matters to a machine author:** `IArguments.escapes()` changed from
   `IArguments escapes()` to `void escapes()` (must mutate in place). The callers now reuse the same `args` object on the
   main thread after the call has returned (`LuaMethodSupplier.java:33-34`, `PeripheralMethodSupplier.java:34-35`).
3. **Injection by assigning `ServerContext.luaMachine` from a mod initializer is sufficient and timely on both Fabric and
   NeoForge** (the 1.21.1 "forge" artifact is NeoForge: `META-INF/neoforge.mods.toml`, depends on `neoforge [21.1.265,21.2)`).
   The field is read in the `ServerContext` constructor at `SERVER_STARTING`/`ServerStartingEvent`; no other code path
   builds a `ComputerContext` or creates computers. It is an internal `@VisibleForTesting` hook, not public API.
4. **The real contract is behavioural, and several parts of it are hard for LuaJIT** (all verified empirically here):
   * PAUSE (time-slicing) must be invisible to Lua. A `lua_yield` from a count hook is NOT invisible: inside a nested
     coroutine it surfaces as a spurious `coroutine.resume` return in user code and the next event gets swallowed; inside a
     C->Lua callback (`table.sort` comparator, `string.gsub` callback) it raises
     `attempt to yield across C-call boundary` in user code (`WORK\scripts\hook_yield_nested.c`, `hook_yield_cboundary.c`).
   * Aborts must work everywhere Cobalt can abort. A pathological `string.find` runs for >300 s in LuaJIT with an erroring
     count hook installed and the JIT off; Cobalt aborts the same call after 1.01 s (`pattern_interrupt.lua`,
     `CobaltInterruptProbe.java`). If a machine cannot stop, CC:T leaks the worker thread forever and wedges the computer.
   * CC:T 1.121.0 runs Cobalt **without a bytecode format**: `string.dump` fails and binary chunks cannot be loaded.
     Stock LuaJIT dumps and loads bytecode by default (verified) - bytecode must be blocked (LuaJIT bytecode is unverified).
   * The 1.21.1 ROM needs `_ENV`, `bit32`, `table.pack/unpack` (and `utf8`, NUL-safe patterns). With a ~150-line Lua
     prelude (`WORK\scripts\cclj_prelude.lua`) the **real 1.121.0 bios boots to the multishell prompt under LuaJIT and runs
     `ls`/`id`** in a mock harness; without the `_ENV` shim boot dies at `colours.lua:13`, without `table.pack` at
     `parallel.lua:72`, without `bit32` at `bios.lua:23`. Two ROM features stay broken even with the prelude because
     LuaJIT's pattern matcher stops at NUL: `textutils.serialiseJSON` (every string) and `help <topic>`.
5. Yielding Java methods (`MethodResult` + `ILuaCallback`: every `mainThread=true` method, every turtle command, inventory
   generics, `peripheral.call`) need continuation support. LuaJIT 2.1 has no `lua_yieldk`, so this needs a Lua-side
   trampoline. The 1.14.4 branch's design (blocking `ILuaContext.pullEvent` on the worker thread) is obsolete in modern
   CC:T and cannot be ported.

## 1. Methodology and artifacts

* Read: `SRC121/core/lua/*`, `core/lua/errorinfo/*`, `core/computer/*` (incl. `computerthread/*`, `mainthread/*`),
  `core/methods/*`, `core/asm/*`, `core/apis/OSAPI.java`, `PeripheralAPI.java`, `api/lua/*`, `core/ComputerContext.java`,
  `shared/computer/core/ServerContext.java`, `shared/CommonHooks.java`, `ServerComputer.java`; Cobalt 0.9.9 sources
  (`BaseLib`, `CoreLibraries`, `LuaC`, `LuaState`, `LuaInterpreter`, `StringMatch`, `LuaString`, `LuaThread`).
* Loader jars: extracted `cc-tweaked-1.21.1-{fabric,forge}-1.121.0-sources.jar` and `1.20.1-fabric-1.115.0-sources.jar`
  to `WORK\jars\`; `javap` on classes extracted to `WORK\classes\`.
* Diff: `diff -ruN -I '^import '` of `core/lua core/computer core/methods core/asm api/lua` + selected files ->
  `WORK\diff-1.115.0-to-1.121.0.diff` (2401 lines; most noise is annotation migration).
* Empirical (LuaJIT 2.1 ROLLING GC64 MinGW build copied to `WORK\luajit\`; Cobalt 0.9.9 jar):
  * `WORK\scripts\globals_probe.lua` - which Cobalt globals LuaJIT lacks.
  * `WORK\scripts\semantics_probe.lua` run under LuaJIT (`WORK\semantics_probe.luajit.txt`) and under Cobalt configured
    like `CobaltLuaMachine` via `WORK\scripts\CobaltProbe.java` (`WORK\semantics_probe.cobalt.txt`).
  * `WORK\scripts\hook_yield_nested.c` / `hook_yield_cboundary.c` (gcc + `lua51.dll`): pause-by-hook-yield semantics.
  * `WORK\scripts\pattern_interrupt.lua` (LuaJIT) vs `WORK\scripts\CobaltInterruptProbe.java` (Cobalt).
  * `WORK\scripts\boot_harness.lua` + `WORK\scripts\cclj_prelude.lua`: boots the real 1.121.0 `bios.lua`/ROM under LuaJIT
    with Lua mocks of term/fs/os/peripheral/redstone and a CobaltLuaMachine-style event loop. Outputs in `WORK\boot\`.

Key commands (Git Bash; `W=$WORK`, `S=...\scratchpad`):
```
javap -cp <extracted classes> dan200.computercraft.core.lua.ILuaMachine 'ILuaMachine$Factory' MachineEnvironment
javap -cp $S/cct/cc-tweaked-1.21.1-core-api-1.121.0.jar dan200.computercraft.api.lua.IArguments | grep escapes
unzip -p $S/cct/cc-tweaked-1.21.1-forge-1.121.0.jar META-INF/neoforge.mods.toml
cd $W/luajit && ./luajit.exe ../scripts/semantics_probe.lua
java -cp "$(cygpath -w $S/cct/cobalt-0.9.9.jar)" CobaltProbe.java semantics_probe.lua
gcc -O1 -I$S/luajit-win/src -o hook_yield_nested.exe hook_yield_nested.c lua51.dll && ./hook_yield_nested.exe [jit]
./luajit.exe ../scripts/boot_harness.lua <ROM121> <rom-listing.txt> <cclj_prelude.lua|none> "ls rom" "id"
CCLJ_SKIP=env ./luajit.exe ../scripts/boot_harness.lua ...   # ablation: env|table52|utf8|bit32
```

## 2. Q1 - exact signatures (identical in 1.115.0 and 1.121.0)

`SRC121/core/lua/ILuaMachine.java`:
```java
public interface ILuaMachine {
    MachineResult handleEvent(@Nullable String eventName, @Nullable Object @Nullable [] arguments);   // :32
    void printExecutionState(StringBuilder out);                                                        // :41
    void close();                                                                                       // :46
    interface Factory {
        ILuaMachine create(MachineEnvironment environment, InputStream bios) throws IOException, MachineException; // :59
    }
}
```
Erased (javap, both versions): `handleEvent(String, Object[])`, `create(MachineEnvironment, InputStream) throws IOException, MachineException`.

`MachineEnvironment.java:27-34` (record): `ILuaContext context`, `MetricsObserver metrics`, `TimeoutState timeout`,
`Iterable<ILuaAPI> apis`, `MethodSupplier<LuaMethod> luaMethods`, `String hostString`.

`MachineException.java:12-25`: `extends Exception`, single ctor `MachineException(String message)` ("user-friendly").

`MachineResult.java`: `OK` (:21, error=false,pause=false), `PAUSE` (:26), `TIMEOUT` (:31, error, message
`"Too long without yielding"`), `GENERIC_ERROR` (:36, error, message null), `error(String)` (:48), `error(Exception)` (:52,
uses `getMessage()`); accessors `isError/isPause/getMessage`. Constructor is private: you can only use these.

The bios `InputStream` must be read and compiled but **not executed** (`ILuaMachine.java:53-54`). Cobalt loads it as chunk
`"@bios.lua"` with the globals table as env (`CobaltLuaMachine.java:89`).

## 3. Q2 - event protocol, lifecycle and threads

### 3.1 Who calls what (`SRC121/core/computer/ComputerExecutor.java`)
* **create**: `createLuaMachine()` :354-384, called from `turnOn()` :386-420 on a `Computer-Worker` thread while holding
  `isOnLock`. Factory taken once from `ComputerContext.luaFactory()` in the executor constructor (:152). `MachineEnvironment`
  is built with `new LuaContext(computer)`, `executor.timeoutState()`, a **lazy** `Iterable` over the executor's API list
  (:372), `luaMethods`, `getHostString()`. `IOException` -> log + terminal "Error loading bios.lua"; `MachineException`
  -> terminal "Error loading bios.lua" + message (:376-383). Any other exception (e.g. `UnsatisfiedLinkError`,
  `ExceptionInInitializerError`) escapes `work()` and is caught by `ComputerThread` (:642-646) -> `abortWithError()`,
  but `isOn` is still false so nothing is enqueued and the computer just stays off with a blank screen.
  **Recommendation:** throw `MachineException("...natives unavailable...")` or fall back to `CobaltLuaMachine::new`.
* The machine is created **under the 30 s `BASE_TIMEOUT`** (`beforeWork` -> `startTimer`, `ManagedTimeoutState.java:106`);
  `turnOn` then sets `wasPaused = true; timeRemaining = TIMEOUT` (:418-419), so the **first** `handleEvent` is
  `handleEvent(null, null)` with a 7 s budget.
* **handleEvent**: only from `resumeMachine` (:552-563) inside `workImpl` (:468-524):
  * paused/start path: `executor.setRemainingTime(timeRemaining); resumeMachine(null, null)` (:517-519);
  * event path: `executor.setRemainingTime(TimeoutState.TIMEOUT); resumeMachine(event.name, event.args)` (:520-523).
  * Result handling (:553-562): `isError()` -> `displayFailure("Error running computer", result.getMessage())` +
    `shutdown()` (which calls `machine.close()`); `isPause()` -> `wasPaused = true; timeRemaining = executor.getRemainingTime()`
    (budget carries over across pauses); otherwise `wasPaused = false`. A `null` result (the 1.20.1 stub) throws NPE ->
    `abortWithError` -> "An internal error occurred, see logs."
  * One event per `work()` call; `work()` re-submits if paused, commands or events remain (:461-466). Queue limit 256
    events (:60, :279); events are dropped silently while a command is pending or the computer is off.
* **close**: (a) `shutdown()` :431-434 on the worker (SHUTDOWN/REBOOT/ABORT commands, error results, failed start);
  (b) `immediateFail()` :254-263 from `abortWithTimeout()` - called by the **Computer-Monitor thread**
  (`ComputerThread.java:532`) **while `handleEvent` may still be running on the worker**; (c) `abortWithError()` on the
  worker after an exception escaped `work()`. `close()` is therefore called **concurrently** and **more than once**
  (immediateFail, then shutdown again; Cobalt also closes itself inside `handleEvent` before returning errors, :139/145/148).
  It must be idempotent, thread-safe, must not free the VM while it runs, and must make a running `handleEvent` return
  promptly (Cobalt: `isDisposed = true; state.interrupt()` then the interrupt handler throws `HardAbortError`, :61,
  :159-163). `handleEvent` after `close()` throws `IllegalStateException` in Cobalt (:118).
* **printExecutionState**: only from `ComputerExecutor.writeState` (:528-534) <- `ComputerThread.WorkerThread.reportTimeout`
  (:681) on the **monitor thread**, concurrently with a running `handleEvent`. Cobalt's implementation is empty (:155-156).
* Successive `handleEvent` calls for one computer may run on **different worker threads** (any of `computer_threads`),
  never concurrently in normal operation (CAS `ON_QUEUE -> RUNNING`, `ComputerThread.java:620`). Native code must not
  cache `JNIEnv*` or rely on thread-locals (the 1.20.1 `main.cpp` correctly uses `jvm->GetEnv` per call).

### 3.2 Event semantics (CobaltLuaMachine, the reference implementation)
* `handleEvent(null, null)`: start, or resume after PAUSE. Resume args are empty; no filter check (:120, :125).
  The coroutine resumed is the currently suspended one, i.e. the exact point of suspension, possibly deep in nested
  coroutines (`state.getCurrentThread()`, :128-129).
* `handleEvent(name, args)`: if the last top-level yield value was a string filter and `name` differs and is not
  `"terminate"`, return `OK` without running Lua (:120-122; the event is consumed). Otherwise resume the bios coroutine
  with `(name, toValues(args)...)` (:125, :131).
* After resume: `null` results = suspended -> `PAUSE` (:133). Else the first yielded value becomes the new filter if it is
  a string, else no filter (:135-136). If the bios coroutine finished -> `close()` + `GENERIC_ERROR` (:138-140) (this is
  what normally happens after `os.shutdown()` lets `bios.lua` return). Else `OK`.
* Uncaught Lua error -> `close()`, warn log, `MachineResult.error(e)` (message = `LuaError.getMessage()`) (:147-151).
  Hard abort (or hard-abort flag observed after return) -> `close()` + `TIMEOUT` (:132, :144-146).
* `os.pullEventRaw` is just `coroutine.yield` (`ROM121/bios.lua:37-39`); `os.pullEvent` adds the `"terminate"` ->
  `error("Terminated", 0)` check (:41-47). `"terminate"` is an ordinary queued event (Ctrl+T:
  `ComputerActionServerMessage.java:40`), delivered past filters by the machine.
* Event argument types seen in practice: `Number` (Integer/Long/Double), `String`, `Boolean`, `null`, **`byte[]`** (`"char"`
  events, `core/input/EventComputerInput.java:38`), **`ByteBuffer`** (`"paste"`, :43), `Map`s (modem messages: arbitrary,
  possibly cyclic, data converted from another computer's Lua table), `Object[]`/`Collection`s, and objects with methods.

## 4. Q3 - TimeoutState, aborts and time-slicing

Constants (`core/computer/TimeoutState.java`): `BASE_TIMEOUT` 30 s (:46, start-up/shutdown work), `TIMEOUT` 7 s (:51),
`ABORT_TIMEOUT` 1.5 s (:56), `ABORT_MESSAGE = "Too long without yielding"` (:61). Flags: `paused`, `softAbort` (plain
fields), `hardAbort` (volatile) (:66-68). Listener API: `addListener` (runs the listener immediately, :113-117),
`removeListener` (:119-122); listeners run under the state's monitor whenever a flag changes.

`ManagedTimeoutState` (`computerthread/ManagedTimeoutState.java`): `refresh()` :41-60 sets `paused` when
`now >= pauseDeadline && shouldPause()`, `softAbort` when `now >= abortDeadline`, `hardAbort` when
`now >= abortDeadline + ABORT_TIMEOUT`; `hardAbort()` :94 sets both flags immediately; `startTimer(pauseTimeout)` :106
(abortDeadline = now + 30 s, pauseDeadline = now + scaledPeriod); `reset()` :116 clears all flags (called from
`afterWork`, `ComputerThread.java:793`). `shouldPause()` = `hasPendingWork()` (other computers queued) (:822-826).

Scheduler (`computerthread/ComputerThread.java`): Monitor wakes every 100 ms (`MONITOR_WAKEUP`, :70) or every
`scaledPeriod()` when busy (:498), calls `refresh()` on running executors (:519). Time slice: `latency = 50 ms * f`,
`minPeriod = 5 ms * f`, `f = 1 + floor(log2(threads))` (:162-170); `scaledPeriod = latency/(1+queued)` if
`1+queued < 10` else `minPeriod` (:410-414). With the default `computer_threads = 1` (`ConfigSpec.java:161-167`) a running
computer is asked to pause after 25 ms when one other computer waits, 5 ms when 9+ wait.

What happens to a machine that never returns (`checkRunners`, :510-548), times relative to the start of the event:
| t | action |
|---|---|
| 7.0 s (+<=100 ms) | `softAbort` set -> listeners. Cobalt throws one catchable `LuaError("Too long without yielding")` |
| 8.5 s | `hardAbort()` + `worker.abortWithTimeout()` -> **`machine.close()` on the monitor thread**, ABORT_WITH_TIMEOUT command |
| 10.0 s | `reportTimeout` (stack trace of the worker + `printExecutionState`) and `Thread.interrupt()` (no effect on native code) |
| 11.5 s | interrupt again, `workerFinished(runner)`: the worker is **abandoned** and a replacement thread spawned (:446-472) |

Consequences for a non-interruptible native machine: the abandoned OS thread keeps spinning forever (threads are daemon,
`ThreadUtils.java:75`, so a dedicated server can still exit; in single-player it survives world unloads); in the common
path the executor is left in `ON_QUEUE` without being queued (`workerFinished` ignores `afterWork()`'s return value), so
the computer can never be turned on/off again until restart; at server stop `ComputerThread.stop` waits 1 s
(`ServerContext.java:104`) and logs "Failed to stop computers under deadline". If the stuck thread ever returns, other
workers may already have run `shutdown()` -> `close()` on the same machine concurrently.

How Cobalt polls (`CobaltLuaMachine.java:60-68`, :95, :110-114): the listener calls `state.interrupt()` (volatile flag)
when `softAbort || paused`; the interpreter checks that flag **before every bytecode instruction**
(`LuaInterpreter.java:177`), in the compiler's input reader (`LuaC.java:205`) and inside pattern matching
(`StringMatch.java:513,528`, without suspending). The handler: hard-abort or disposed -> `HardAbortError` (a Java `Error`
that Lua `pcall` cannot catch: `LuaThread.java:321` only catches `Exception | VirtualMachineError`); first soft abort ->
catchable `LuaError(ABORT_MESSAGE)` (once per work unit, `thrownSoftAbort` reset when flags clear); paused -> `SUSPEND`
(unwinds and later resumes the whole Lua stack, nested coroutines included); else continue.

Empirical LuaJIT comparison:
* Count hooks do not fire in JIT-compiled loops (already established; re-observed: `hook_yield_nested.exe jit` -> hook
  never fired, child loop completed).
* Count hooks do not fire inside C library calls. `string.find(("a"):rep(6000), ".-.-.-b")` with an erroring count hook and
  `jit.off()`: killed by `timeout 300` after 300 s (`WORK\pattern_interrupt.txt`). Cobalt with CC:T-style interrupt after
  1 s: `ok=false err=probe:1: Too long without yielding elapsed=1.01s` (`WORK\pattern_interrupt.cobalt.txt`).
* Pause via `lua_yield` from a count hook (`hook_yield_nested.exe`, JIT off): the hook fired in the nested coroutine; the
  machine's `lua_resume` returned `LUA_YIELD` with one `nil` value (indistinguishable from `os.pullEvent()`), the user's
  scheduler saw `coroutine.resume(co) -> true, nil` with `co` still suspended, and the next event (`some_event`) was passed
  into the child as the hook's return and lost. In `table.sort` comparator / `gsub` callback the same hook-yield raised
  `attempt to yield across C-call boundary` in user code (`hook_yield_cboundary.exe`, cases 1-2); inside `pcall` it works
  (case 3). LuaJIT hooks are per `global_State`, so a hook set on the main thread fires in every coroutine.

=> A LuaJIT machine cannot implement PAUSE by hook-yield. Options: (a) never return `PAUSE` (allowed by the contract;
costs fairness: one busy computer holds a worker for up to 7 s per event, freezing all others with the default single
thread); (b) run each machine on its own native thread and hand control back and forth (the hook then *blocks* the Lua
thread and `handleEvent` returns `PAUSE`; transparent at any depth; costs one OS thread per running computer and two
context switches per event); (c) a coroutine-library rewrite that propagates a pause sentinel through every
`coroutine.resume`/`wrap` level and only pauses at yieldable points (complex, still cannot pause inside C callbacks).
Hard/soft abort additionally need a check inside JIT traces and inside long-running C functions (patched `lib_string.c`
at minimum) - see other lanes for the JIT side.

## 5. Q4 - method dispatch and yielding

* API injection (`CobaltLuaMachine.addAPI`, :98-108): one table per `ILuaAPI`, filled by `makeLuaObject` (:172-175 ->
  `luaMethods.forEachMethod`), set as global under **every** name in `getNames()` (e.g. `rs` and `redstone` share a table);
  if `getModuleName() != null` also `registry._LOADED[moduleName] = table` (the ROM's `cc.require` copies string keys of
  `debug.getregistry()._LOADED` into each `package.loaded`, `ROM121/rom/modules/main/cc/require.lua:134-140`). APIs with no
  methods log a warning. Built-in APIs: term, redstone/rs, fs, peripheral, os, http (if enabled), plus `Computer.addApi`
  ones (turtle, pocket, commands, and `ILuaAPIFactory` add-ons, `ServerComputer.java:83-89`). `ILuaAPI.startup/update/
  shutdown` are driven by `ComputerExecutor`, not the machine.
* `MethodSupplierImpl.forEachMethod` (`core/asm/MethodSupplierImpl.java:70-93`): `@LuaFunction` instance methods of the
  object's class (+ registered generic methods), then the methods of each `ObjectSource.getExtra()` object
  (`HttpResponseHandle`, `TransferredFile`), then `IDynamicLuaObject.getMethodNames()` (dispatch via
  `callMethod(context, index, args)`, `LuaMethodSupplier.java:37-39`). Each callback gets `(target, name, method, info)`;
  the function must call `method.apply(target, context, args)` with **that target** (not the outer object). Methods are
  called with dot syntax: no implicit `self`. `NamedMethod.nonYielding` (`isSimple` = return type is not `MethodResult`
  and not `mainThread`, `MethodSupplierImpl.java:138`) is available as a fast-path hint.
* Java -> Lua objects (`toValue`, :195-250): `ILuaFunction` -> function (`FUNCTION_METHOD`, :38); `IDynamicLuaObject` -> table
  of methods; other objects -> table of their methods if any, else `nil` + warning. Converted once per `toValues` call
  with identity sharing (same Java object -> same Lua table within one result).
* Call path (`ResultInterpreterFunction.invoke`, :52-73): `VarargArguments.of(args)` -> `method.apply(instance, context,
  arguments)` -> `arguments.close()` in `finally`. `LuaException` -> `LuaError(message, level)` via `wrap` (:97-102: no
  level and no adjust -> default level 1, i.e. "chunk:line: " of the calling Lua function is prefixed; level 0 -> no
  position). Any other `Throwable` -> logged + `LuaError("Java Exception Thrown: " + t, 0)`.
  The **same `ILuaContext` instance** (`environment.context()`) must be passed on every call - `PeripheralAPI` caches its
  `GuardedLuaContext` wrapper on that assumption (`PeripheralAPI.java:97-102`).
* `MethodResult` with callback: the result values are **yielded from the currently running coroutine** (whatever user
  coroutine called the Java function, :71-72); on resume, the resume values are converted with `toObjects` and passed to
  `callback.resume(Object[])` (:76-95), whose `MethodResult` is either returned to the Lua caller or yields again; error
  levels in callbacks are adjusted by `MethodResult.getErrorAdjust()` (used by `peripheral.call`: `adjustError(1)`,
  `PeripheralAPI.java:323`).
  * `MethodResult.pullEvent(filter, cb)` yields `filter` and throws `LuaException("Terminated", 0)` on `"terminate"`
    (`MethodResult.java:89-97`); `pullEventRaw` (:109-112); `yield(args, cb)` (:124-127).
  * Main-thread work: `mainThread = true` methods call `args.escapes()` and then `context.executeMainThreadTask(...)`
    (`LuaMethodSupplier.java:32-35`) -> `TaskCallback.make` (`TaskCallback.java:57-59`): `issueMainThreadTask` queues a
    `Runnable` on the server thread (`LuaContext.java:24-53`, `"Task limit exceeded"` when full) which later queues event
    `("task_complete", taskId(Long), ok(Boolean), results... | errorMessage)`; the callback is `pullEvent("task_complete")`
    and checks `response[1] instanceof Number` and `response[2] instanceof Boolean` (:39-55). Lua numbers must therefore
    come back as `Number` and booleans as `Boolean`.
  * Users of yielding results (`grep`): all `mainThread=true` functions (31 in CC:T alone), `TurtleAPI`/`TurtleBrain`
    (`"turtle_response"`), generic inventory/fluid/energy methods, `CommandAPI`, printer, disk drive, command block,
    `WebsocketHandle`, pocket API, and `peripheral.call`.
* LuaJIT has no `lua_yieldk`/continuations. A C function may `return lua_yield(L, n)` (values go to the parent's
  `coroutine.resume`, which matches Cobalt), but nothing can run after the resume. The machine therefore needs a Lua
  trampoline per Java function (or one generic one), e.g. native returns `(YIELD_MARK, cbHandle, values...)`, the Lua
  wrapper does `coroutine.yield(values...)`, then calls `native_resume(cbHandle, ...)` in a loop. This adds one Lua frame,
  so error levels must be shifted by one. The 1.14.4 branch instead blocked the worker inside `ILuaContext.pullEvent`
  (`git show origin/1.14.4_CCT:src/main/java/gay/vereena/cclj/computer/LuaJITMachine.java` lines ~221-256); that API no
  longer exists (`ILuaContext` now has only `issueMainThreadTask` + default `executeMainThreadTask`).

## 6. Q5 - value conversion rules

Lua -> Java (`CobaltLuaMachine.toObject`, :269-312; used by `VarargArguments.get`, `toObjects` for callbacks/`os.queueEvent`):
* nil -> `null`; number (int or float) -> **`Double`** always; boolean -> `Boolean`; string -> `String` with **one char per
  byte** (ISO-8859-1: `LuaString.decode` = `(char)(b & 0xFF)`, `LuaString.java:520-526`); table -> `HashMap`, recursion with
  an `IdentityHashMap` so cycles/shared sub-tables are preserved; entries whose converted key or value is `null` are
  dropped (functions, coroutines, userdata); `next` errors stop iteration; function/thread/userdata -> `null`.
* `IArguments` (`VarargArguments.java`): lazy, cached per index (:81-102); `getDouble`/`getLong` require a real number (no
  string coercion; :140-153, `getLong` truncates finite doubles, rejects NaN/inf); `getString` requires a string (via
  `get`); `getBytes`/`optBytes` return the raw Lua bytes (:156-170); `getStringCoerced` = Cobalt `tostring` without
  `__tostring` (:105-109); `getBytesCoerced` (:112-116); `getType` honours a `__name` metafield on tables/userdata (:119-129,
  :225-230); `getTableUnsafe` returns a live `TableImpl` view valid only during the call (:173-179); after
  `ResultInterpreterFunction` closes the args, any access throws `IllegalStateException` unless `escapes()` was called
  first, which eagerly converts tables and records type names (:192-219).
* `TableImpl` (`TableImpl.java`): `rawget` views; key conversion `Boolean/Double/String/byte[]/ByteBuffer` (:70-83);
  `getLong(int)` exact for integers; map views via a full conversion (:102-108). Note `IArguments` default methods
  (`IArguments.java:151-219`) give exactly the same results for an eager `ObjectArguments` implementation, except
  `getType` (no `__name`; function -> "nil") and `getStringCoerced` (Java `Double.toString` instead of Lua `%.14g`, e.g.
  `1e20` -> `"1.0E20"`).

Java -> Lua (`toValue`, :177-254; order matters): `null` -> nil; any `Number` -> float `doubleValue()` (Long > 2^53 loses
precision); `Boolean`; `String` -> bytes with **chars >= 256 replaced by `'?'` (63)** per UTF-16 unit (`LuaString.encode`,
`LuaString.java:540-546`; same rule in `LuaValues.encode`); `byte[]` (copied) and `ByteBuffer` (remaining bytes, advances
position) -> binary string; then identity cache; `ILuaFunction` -> function; `IDynamicLuaObject` -> methods table; `Map`
-> new table, nil keys/values skipped, **cycle-safe** (entry stored before recursing); `Collection` -> array 1..n;
`Object[]` -> array 1..n; empty `List.of()/Set.of()/Map.of()` singletons -> fresh table each time (`LuaUtil.java:39-54`);
objects with methods -> methods table; anything else -> nil + warning. NaN/inf pass through as doubles.

JNI implications (1.20.1 code is wrong here): use byte arrays + `new String(bytes, ISO_8859_1)` / a CC:T-style encode loop,
never `NewStringUTF`/`GetStringUTFChars` (modified UTF-8 changes bytes >= 0x80 and truncates at NUL with
`lua_pushstring`; `main.cpp:272-273, 315, 440-441, 788`) and never `US_ASCII` (`LuaJITMachine.java:14`). Note
`String.getBytes(ISO_8859_1)` maps a surrogate *pair* to one `'?'` while Cobalt produces two. Conversion of `Map`s must be
iterative or depth-limited and cycle-safe on the native side (modem messages can be cyclic).

## 7. Q6 - Lua environment CC:T provides via Cobalt, and what LuaJIT lacks

`CobaltLuaMachine` constructor (:76-93): `CoreLibraries.debugGlobals` = base + table + string + coroutine + math + utf8 +
debug (`CoreLibraries.java:44-64`); `Bit32Lib.add`; `ErrorInfoLib.add` (`registry._LOADED["cc.internal.error_info"]`,
`ErrorInfoLib.java:33`, Lua-5.2-bytecode-specific, optional for the ROM: `error_hints.lua:146-147` pcall-requires it);
`_HOST = environment.hostString()` = `"ComputerCraft <ver> (Minecraft <mc>)"` (`ServerContext.java:232-235`);
`_CC_DEFAULT_SETTINGS = CoreConfig.defaultComputerSettings` (read at machine creation; bios parses it, `bios.lua:700-722`).
No `os`/`io`/`package`/`require`/`collectgarbage`/`print`/`dofile`/`loadfile` from the VM (bios defines print/loadfile/dofile;
`os` is the Java `OSAPI` table). `_VERSION = "Lua 5.2"` (`BaseLib.java:66`). `LuaState.builder()` sets **no
`bytecodeFormat`**, so `string.dump` -> "unable to dump given function" and any `\27`-prefixed chunk -> "attempt to load a
binary chunk" (`StringLib.java:244-249`, `LuaC.java:166-171`). `load` defaults to mode `"bt"` but binary is impossible.
Cobalt string metatable `__index = string` as usual.

Globals probe (`globals_probe.lua`, stock LuaJIT, no 5.2 compat): missing `rawlen`; `table.pack/unpack`;
`string.pack/unpack/packsize/gfind`; `math.mod`; whole `utf8`; whole `bit32`. LuaJIT-only extras that must be removed or
hidden: `collectgarbage gcinfo newproxy dofile loadfile print unpack(ok, Cobalt has it too) module require package io os
jit bit` (+ `ffi`, `string.buffer` if opened).

Semantics probe, Cobalt (CC:T config) vs LuaJIT (`WORK\semantics_probe.{cobalt,luajit}.txt`):

| check | Cobalt 0.9.9 | LuaJIT 2.1 |
|---|---|---|
| `load("return _ENV",n,"t",env)() == env` | true | **false (nil)** |
| `string.dump` | error | **works** |
| `load(bytecode)` / `loadstring(bytecode)` | impossible | **loads and runs** |
| `load(bytecode, n, "t")` | - | rejected ("wrong mode") |
| `bit.band(0xFFFFFFFF,0xFFFFFFFF)` (bios `bit` = bit32) | 4294967295 | -1 |
| `bit.lshift(1,31)` / `bit.rshift(-1,0)` | 2147483648 / 4294967295 | -2147483648 / -1 |
| `table.pack/unpack`, `rawlen`, `utf8` | present | **absent** |
| `__pairs` / `__len` on tables | honoured | **ignored** |
| `tostring(2^53)` | `9007199254740992` | `9.007199254741e+15` |
| `debug.getinfo(...).currentcolumn` | 89 | nil (ROM degrades gracefully, `exception.lua:213`) |
| error message format | `chunk:1: attempt to call global 'undefined_fn' (a nil value)` | identical |
| `goto`, `\z`, `\u{48}`, `xpcall(f,h,arg)`, `math.log(8,2)`, `string.rep(s,n,sep)` | ok | ok |
| `os`, `io`, `collectgarbage` | nil | present |
| `_VERSION` | Lua 5.2 | Lua 5.1 |

ROM requirements (`grep` over 105 `.lua` files): `_ENV` 18 uses in 15 files (`colours.lua:11`, `fs.lua:12`, `term.lua:18`,
`keys.lua:137`, `turtle.lua:32`, `commands.lua:54`, `require.lua:19`, `shell.lua:70`, `edit_runner.lua:23`, ...);
`table.pack` 25 / `table.unpack` 39; `bit32.*` 20 (bios, colors, textutils, dfpwm, base64); `utf8.*` 5 (textutils, json,
snbt); `debug.getinfo/getlocal/getupvalue/getmetatable/getregistry` 13; patterns containing NUL: `textutils.lua:448`,
`help.lua:42-43`. The bios itself relies on `loadstring` (:14) and 4-arg `load(src, name, mode, env)` (:436), and keeps
two 5.1-era hooks that only make sense if `load` writes `env._ENV`: strict-globals pre-sets `tEnv._ENV` (:458-466) and
`os.loadAPI` filters the `_ENV` key (:515). The ROM's VM requirements are the same in 1.20.1 (bios diff is whitespace only).

Boot harness results (real 1.121.0 bios + ROM under LuaJIT, `WORK\boot\`):
* no prelude: `bios.lua:23: attempt to index global 'bit32' (a nil value)`.
* full prelude: boots multishell, shows the motd, runs `ls rom` and `id` correctly (`boot_full_prelude.txt`).
* minus `_ENV` shim: `bios.lua:561: Failed to load API colours due to /rom/apis/colours.lua:13: attempt to index local 'colours' (a nil value)`.
* minus `table.pack/unpack`: shell crashes `/rom/apis/parallel.lua:72: attempt to call field 'pack'`.
* minus `utf8`: boot fine (used lazily).
* with full prelude: `textutils.serialiseJSON('abc')` -> `/rom/apis/textutils.lua:448: malformed pattern (missing ']')`;
  `help credits` -> `/rom/programs/help.lua:53: malformed pattern (missing ']')` (LuaJIT `classend` stops at NUL,
  `lib_string.c:189-208`); `string.dump` -> nil; `load("\27LJ")` -> "attempt to load a binary chunk".
The `_ENV` shim (`rawset(env, "_ENV", env)` in `load`, `_G._ENV = _G`) only emulates the common read pattern; Lua 5.2
code that assigns `local _ENV = t` to change scoping will silently behave differently.

## 8. Q7 - printExecutionState and metrics

* `printExecutionState` exists only for the timeout log message (see 3.1) and is called on the monitor thread while the
  machine runs. Cobalt prints nothing. A native implementation should print only Java-side state captured safely (e.g.
  last chunk:line recorded by the hook into an atomic) and must never touch the `lua_State` from that thread.
* `MachineEnvironment.metrics` is unused by `CobaltLuaMachine`. Computer CPU time (`Metrics.COMPUTER_TASKS`) is measured
  by `ComputerThread.afterWork` (:794) around `work()`, so it covers native execution automatically. 1.115.0 also had
  `Metrics.JAVA_ALLOCATION` (per-thread Java allocation) - removed in 1.121.0; native allocations were never visible.
  CC:T has no Lua memory limit of its own (Cobalt is bounded by the JVM heap); a native VM is not, so a per-computer cap
  via a custom allocator is needed (GC64 builds allow `lua_newstate(allocf, ud)`, `lj_state.c:238-243`, `lib_aux.c:391`).

## 9. Q8 - what changed 1.115.0 -> 1.121.0 that a machine must care about

Full diff: `WORK\diff-1.115.0-to-1.121.0.diff`.
* `ILuaMachine`, `Factory`, `MachineEnvironment`, `MachineResult`, `MachineException`: no change (annotations only).
* `CobaltLuaMachine`: identical except `Bit32Lib.add(state, globals)` -> `Bit32Lib.add(state)` (Cobalt 0.9.5 -> 0.9.9).
* `IArguments.escapes()`: `IArguments` -> `void`, "require that it mutates this" (`IArguments.java:500`); callers keep using
  the original object (`LuaMethodSupplier.java:33-34`, `PeripheralMethodSupplier.java:34-35`). **Binary-incompatible for
  any custom `IArguments`.** `getAll()` now nullable elements.
* `VarargArguments`: shared `EMPTY` instance removed; `escapes()` void.
* `api.lua.LuaTable`: new defaults `get(int)` (integer keys normalised to double), `getDouble`, `getFiniteDouble`,
  `getBoolean`, `getString`, table getters (since 1.116); `TableImpl` gained `get(int)` and Boolean/Double/byte[]/ByteBuffer
  key support (previously String/Integer only). `ObjectLuaTable` gained `equals/hashCode`.
* `Generator`: `@LuaFunction` parameters of type `LuaTable` are now supported (copied via `getTable` + `ObjectLuaTable`).
* `LuaValues.badTableItem/badField` message format changed to `bad item #i (number expected, got x)` / `bad field 'k' (...)`.
* `ComputerContext`: class -> record (deprecated public canonical ctor), builder gained `http(...)`; factory default still
  `CobaltLuaMachine::new`. `ServerContext` additionally wires `.http(http)`.
* `ComputerExecutor`: HTTP API built from `context.http()`; `GlobalEnvironment.getRomMount()/getLuaBios()` replace
  `createResourceMount/createResourceFile`; `displayFailure` no longer prints "ComputerCraft may be installed incorrectly".
* `ComputerThread`: per-thread allocation tracking removed; timeout/pause/abandon logic unchanged.
* `ComputerEvents` removed -> `core/input/EventComputerInput` (same events: `"char"` with `byte[]`, `"paste"` with `ByteBuffer`).
* `OSAPI`/`PeripheralAPI`: refactors only (no protocol change).
* Bytecode: Java 21 class files (major 65) vs Java 17 (61). MC 1.21.1 needs Java 21.
* ROM: VM-relevant needs unchanged; new modules `cc.base64` (bit32), `cc.internal.json`/`snbt` (`utf8.char`),
  `edit_runner` (4-arg load with `_ENV`), `menu`.
* Cobalt 0.9.5 -> 0.9.9 semantic changes were not diffed (only 0.9.9 sources available) - open question.

## 10. Q9 - injection

* Field: `public static ILuaMachine.Factory luaMachine = CobaltLuaMachine::new;` annotated `@VisibleForTesting`
  (`ServerContext.java:54-55`; javap: `public static ...ILuaMachine$Factory luaMachine;` non-final in fabric and neoforge
  jars and in 1.20.1). Read only in the `ServerContext` constructor (`.luaFactory(luaMachine)`, :77), which runs in
  `ServerContext.create` <- `CommonHooks.onServerStarting` (:82-90) <- Fabric `ServerLifecycleEvents.SERVER_STARTING`
  (`fabric ComputerCraft.java:94-100`) / NeoForge `ServerStartingEvent` (`ForgeCommonHooks.java:48-50`). Stored in the
  `ComputerContext` record and copied into every `ComputerExecutor` (:152). So: read **once per server start** (every
  single-player world load); changes after that are ignored until the next start.
* Timeliness: Fabric `main` entrypoints (`ModInitializer.onInitialize`) and NeoForge `@Mod` constructors /
  `FMLCommonSetupEvent` all run before any server starts, independent of mod order. Assigning the static triggers
  `ServerContext.<clinit>` first (which sets the Cobalt default) and then overwrites it - no race.
* Only path that creates computers: `ServerComputer` -> `new Computer(context.computerContext(), ...)`
  (`ServerComputer.java:79`); `ComputerContext.builder` is used only by `ServerContext` (grep over common + core + fabric +
  neoforge sources). No client-side computers. CC:T's own tests are not in the jars.
* The 1.21.1 `forge` artifact is NeoForge: `META-INF/neoforge.mods.toml` (`modLoader="javafml"`, depends
  `neoforge [21.1.265,21.2)`), `META-INF/jarjar/cobalt-0.9.9.jar`; core classes are inside the main jar for both loaders
  (Fabric nests Cobalt under `META-INF/jars/`).
* Fragility: `ILuaMachine` Javadoc says there should be only one implementation and offers no registration API
  (`ILuaMachine.java:16-18`); `@VisibleForTesting` means CC:T may change it freely. Declare a tight dependency
  (`"computercraft": ">=1.121.0 <1.122"` or similar) - the 1.20.1 `fabric.mod.json` declares no CC:T dependency at all, so a
  missing CC:T gives a `NoClassDefFoundError` instead of a clean loader error. Wrap the factory so a native-load failure
  falls back to `CobaltLuaMachine::new` or raises `MachineException`.

## 11. Machine implementation contract (CC:T 1.121.0)

**Construction** - `Factory.create(MachineEnvironment env, InputStream bios)`
1. Runs on a computer worker thread under a 30 s budget. Read `bios` fully; compile as text chunk `"@bios.lua"` with the
   globals table as its environment; do not run it. Compile errors -> `MachineException(message)`; I/O -> `IOException`.
   Any other throwable leaves the computer silently off - avoid.
2. Build globals equivalent to Cobalt's: base (incl. `getfenv/setfenv/loadstring/unpack/rawlen`), `coroutine`, `string`
   (no `dump`), `table` (incl. `pack/unpack/move`), `math` (incl. `pow`, `log(x,b)`), `utf8`, `bit32` (unsigned
   semantics), the `debug` subset the ROM uses (`getinfo/getlocal/getupvalue/getmetatable/getregistry/traceback/sethook`),
   `_G`, `_VERSION`, `_HOST = env.hostString()`, `_CC_DEFAULT_SETTINGS = CoreConfig.defaultComputerSettings`.
   Do not expose `os`/`io`/`package`/`require`/`collectgarbage`/`jit`/`ffi`/`print`/`dofile`/`loadfile`.
3. `load`/`loadstring` must refuse binary chunks regardless of the `mode` argument; support `load(chunk, name, mode, env)`
   including reader functions; emulate `_ENV` well enough for the ROM (`env._ENV = env` on load, `_G._ENV = _G`).
4. Pattern functions must accept NUL bytes in patterns (Lua 5.2 behaviour) and must be interruptible.
5. For each `api` in `env.apis()`: table of its methods via `env.luaMethods().forEachMethod`; set `_G[name]` for every
   `getNames()` entry; if `getModuleName()` is non-null, set `registry._LOADED[moduleName]`.
6. Register a `TimeoutState` listener (`env.timeout().addListener`) that mirrors `isSoftAborted/isHardAborted/isPaused`
   into memory the VM can poll cheaply; it is called immediately, from the monitor thread, the worker thread (`reset`) and
   the server thread (`ComputerThread.stop` -> `hardAbort`). Remove it in `close()` (the `TimeoutState` outlives machines;
   it belongs to the executor and is reused across reboots).

**`handleEvent(String name, Object[] args)`** (worker thread; possibly a different thread each call; never concurrent with
itself unless a stuck call was abandoned)
7. `name == null`: start (first call) or continue exactly where the previous call paused. No filter check, no values.
8. `name != null`: if a string filter is set and `name` != filter and != `"terminate"`, return `OK` immediately. Else
   resume the top-level coroutine with `(name, converted args...)`.
9. Return `OK` when the top-level coroutine yields; record its first yield value as the filter if it is a string, else
   clear the filter. Return `PAUSE` only if the whole Lua state can be continued transparently later. Return
   `MachineResult.error(msg)` on an uncaught error, `GENERIC_ERROR` when the bios coroutine returns, `TIMEOUT` on hard
   abort; free/close the VM in all three cases. Never return `null`.
10. Timeout behaviour: when soft-abort is first observed, raise a catchable Lua error `"Too long without yielding"` in
    the running coroutine (once per work unit); when hard-abort is observed (or after `close()`), unwind everything with
    an error Lua `pcall` cannot swallow, within well under 1.5 s, **including inside JIT-compiled code and long C library
    calls**; otherwise CC:T abandons and leaks the worker thread and wedges the computer.
11. Java calls from Lua: build `IArguments` (eager `ObjectArguments` is acceptable; a lazy view must support
    `escapes()` mutating in place and must throw after the call when not escaped); always pass `env.context()`; call
    `LuaMethod.apply(target, context, args)` with the target supplied by `forEachMethod`; map `LuaException` to a Lua
    error at `level` (default 1; 0 = no position; adjust for any wrapper frames and `getErrorAdjust()` in callbacks);
    map other throwables to `"Java Exception Thrown: " + t` at level 0 and log them; never let a Java exception or a
    `longjmp` cross JVM frames.
12. `MethodResult` with a callback: yield its values from the **current** coroutine; when resumed, convert the resume
    values to Java (`toObject` rules) and call `callback.resume`; repeat while it yields; return the final values.
13. Conversions exactly as section 6 (Double numbers, ISO-8859-1 byte<->char, `'?'` for chars >= 256, `byte[]`/
    `ByteBuffer` -> binary strings, cycle-safe identity-preserving Map/Collection/array conversion, method tables for
    objects, `ILuaFunction`, `IDynamicLuaObject`).

**`close()`** - idempotent; callable concurrently from the monitor thread while `handleEvent` runs, and repeatedly; must
make a running `handleEvent` stop promptly, must not free native memory that a running call uses (defer the free to the
thread that owns execution), must release all JNI global refs and native memory eventually (computers reboot often).

**`printExecutionState(StringBuilder)`** - called on the monitor thread during a running call; debugging text only; must
not touch the VM; may be empty.

**Resource limits not enforced by CC:T but implicitly provided by the JVM/Cobalt** - Lua memory (JVM heap), stack depth
(Cobalt `LuaError` on overflow): the native machine needs its own per-computer memory cap and C-stack safety.

## 12. Findings

See the structured return for severities. Summary list:
1. (critical) Hook-based interruption cannot stop JIT traces or C library calls; a stuck call leaks a worker thread forever
   and wedges the computer (code path + empirical 300 s vs 1.01 s).
2. (critical) Binary chunks/`string.dump` are disabled in CC:T but enabled in LuaJIT; must be blocked (sandbox escape class).
3. (high) PAUSE cannot be done by hook-yield (spurious resumes/lost events, "attempt to yield across C-call boundary");
   choose: no PAUSE, thread-per-machine handoff, or a coroutine-propagation scheme.
4. (high) `MethodResult`/`ILuaCallback` continuations need a Lua trampoline; 1.14.4's blocking design is obsolete.
5. (high) ROM needs `_ENV`, `bit32` (unsigned), `table.pack/unpack`, `utf8` -> prelude; verified boot.
6. (high) NUL in patterns: `textutils.serialiseJSON` and `help` broken under LuaJIT; patch `lib_string.c`.
7. (high) `close()`/`printExecutionState()` concurrency and idempotency; `TimeoutState` listener lifecycle.
8. (medium) String encoding: ISO-8859-1 + `'?'`; 1.20.1 code uses modified UTF-8 / US_ASCII.
9. (medium) `IArguments.escapes()` signature/semantics change.
10. (medium) Native memory unbounded; need allocator cap.
11. (medium) Failure modes of factory: throw `MachineException` or fall back to Cobalt; declare CC:T dependency.
12. (info) Contract otherwise unchanged 1.115.0 -> 1.121.0; injection via `ServerContext.luaMachine` works on Fabric and NeoForge.
13. (low) Smaller semantic differences: `__pairs`/`__len`, `tostring` of large integers, `_VERSION`, `currentcolumn`,
    `getStringCoerced` formatting, `table.insert` bounds, error-hint module absent.

## 13. Effort notes (this lane)

Java side of the contract (factory, record use, result mapping, listener, close/concurrency, conversions, IArguments,
callbacks trampoline glue, injection, loader metadata): ~3-5 person-days experienced, ~8-15 for a modding newcomer.
Lua prelude to ROM parity (bit32/utf8/table/_ENV/load/stripping) incl. tests against the ROM: ~1-2 days / 3-5 days.
Native-side contract items that are genuinely hard (interruptible JIT + C library, transparent PAUSE or decision to drop
it, NUL-safe patterns, memory cap, safe close): ~2-4 weeks experienced (LuaJIT internals), likely 1-3 months for a
newcomer; these dominate the port.

## 14. Open questions
* Is dropping PAUSE acceptable to the user (fairness vs complexity), or is a thread-per-machine design preferred?
* `_VERSION`: report "Lua 5.1" (truthful; `_ENV` is emulated) or "Lua 5.2" (what CC:T programs expect)?
* Cobalt 0.9.5 -> 0.9.9 Lua-semantics changes were not diffed.
* Whether a LuaJIT patch can add a cheap interrupt check to trace loops (other lane).
