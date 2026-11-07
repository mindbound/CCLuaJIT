# verify-preempt-patch: adversarial verification of the preempt-patch claims

Verifier work dir: `<scratch>\work\verify-preempt-patch` (abbreviated `W` below).
Investigator report: `...\scratchpad\reports\preempt-patch.md`. I did not use the investigator's binaries or harness. I read their logs only to cross-check counts.

## Summary of verdicts

| id | claim | verdict | my severity |
|---|---|---|---|
| PP-1 | Stock LuaJIT: hooks never run in JIT code, so CC:T timeouts cannot stop JIT loops | **confirmed** (independent: 19/20 of my own loop cases hung on stock; 20/20 stopped on CHECKHOOK and on the patched build; 1 of 1294 async yield requests honoured on stock) | critical |
| PP-4 | Cross-thread `lua_sethook` is unsafe; 1.14.4 `abortMachine` does it | **confirmed, and now empirically reproduced, worse than stated**: stuck `HOOK_ACTIVE` (hooks dead forever), the assertion `active hook flag removed`, the assertion `hot side exit while recording`, and **segfaults (6 of 8 release runs)** under stress. A control run without cross-thread calls was clean (8/8). | high (critical potential: process crash = server crash) |
| PP-5 | CHECKHOOK plus a count hook that stays installed: 25-73x slower, plus junk side traces | **partially confirmed**: the mechanism is right, but the range depends on the workload. I measured **3.5x-77x** vs JIT and **2.4x-19x** vs the plain interpreter. The cost does not depend on the hook count (N=1e4 and N=1e7 give the same 6.2 s). The traces counted were 102-410 vs 2-22. The patched build had about 0% cost and no extra traces. | medium (design trap that only matters if CHECKHOOK + a permanent hook is chosen) |
| PP-6 | A hook yield reaches only the innermost coroutine, so a CC:T pause can't be a plain hook yield | **confirmed**, and I measured the concrete CC:T consequence with a loop equivalent to `parallel.waitForAny`: the host sees a normal "wait for any event" yield, the **next event is consumed and lost**, and the program stalls until some event arrives | medium (design constraint; dropping pause support is a usable fallback) |
| PP-7 | An `xpcall` handler runs inside the hook with hooks disabled, so error-based aborts can be escaped | **confirmed** (hang with `hookmask=0x18`, HOOK_ACTIVE set). **But the stated mitigation (yield-based abort) is incomplete**: `xpcall` plus a loop inside a non-yieldable C boundary (`string.gsub` callback, `table.sort` comparator) **hangs even with yield-based abort** on the patched and CHECKHOOK builds. A one-line `lj_err_run` patch fixes all 6 variants with both abort styles (verified). | high |
| PP-8 | `__gc` finalizers, Lua hook functions and loops in C code can't be interrupted | **confirmed** on all builds. Additional facts: Cobalt polls interrupts inside pattern matching (`StringMatch.java:513,528`), polls independently of debug hooks, and has no `__gc`/`newproxy`, so **all three are regressions vs stock CC:T**. `debug.sethook()` with no arguments also silently removes the host hook: in the patched design that means preemption is gone for good and the VM livelocks in EXIT. | high |

## Method

### Builds (my own, from the vendored source)
```
git -C <repo>/vendor/luajit archive HEAD | tar -x -C $W/src-stock   # 19878ec0, read-only export
cp -r src-stock src-{checkhook,patched,patched-assert,stock-assert}
(cd src-patched && git apply $W/luajit-preempt.patch)   # copy of the investigator's patch; applies cleanly
mingw32-make -j8 BUILDMODE=static                                         # src-stock
mingw32-make -j8 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK       # src-checkhook
mingw32-make -j8 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_INTERRUPT       # src-patched
mingw32-make ... "XCFLAGS=-DLUAJIT_ENABLE_INTERRUPT -DLUA_USE_ASSERT -DLUA_USE_APICHECK"  # src-patched-assert
mingw32-make ... "XCFLAGS=-DLUA_USE_ASSERT -DLUA_USE_APICHECK"            # src-stock-assert
```
Build logs: `W/build-*.log`.

### My harnesses (written from scratch; different design from the investigator's)
- `W/vh/vh.c`: the VM thread runs a case file under `lua_pcall`, or under `lua_resume` in a coroutine. A monitor thread waits 250 ms and samples `vmstate` 50 times. It then requests an interrupt, either with an async `lua_sethook(L,h,MASKCOUNT,1)` (`sethook`) or with a count hook installed from the VM thread (N=1e4) plus `luaJIT_interrupt` (`intr`). It distinguishes two failure modes: "hook never ran within 3 s" and "hook ran but the code did not stop within 3 s". On failure it prints `vmstate` and `hookmask`. The actions are `error`, `harderr` (re-arm count 1, error every time), `hardyield` (re-arm count 1, yield if yieldable, else error) and `yield`. The runner `W/vh/run.sh` runs each case in its own process.
- `W/vh/vh2.c`: subcommands `race` (PP-4: HOOK_ACTIVE race), `bench` (PP-5), `yieldtest` (PP-1) and `events` (PP-6, a CC:T-like event driver).
- `W/vh/vh3.c`: a PP-4 race against the trace recorder. The VM thread `load`s and runs new looping functions all the time. Another thread calls `lua_sethook(L,NULL,0,0)` (mode 0), or a hook install followed by a removal (mode 1), or nothing at all (mode 2, the control).
- Case files: `W/vh/cases/*.lua`. Benchmarks: `W/vh/bench/{fib,mandel,tbl,strs}.lua`. All logs: `W/vh/logs/`.

## PP-1: hooks never run in JIT code (confirmed, critical)

Source re-read (stock copy):
- `lua_sethook` is at `lj_dispatch.c:329-340`: it stores hookf, count and mask, calls `lj_trace_abort` and then `lj_dispatch_update`.
- `lj_dispatch_update` (`:106`) sets DISPMODE_INS for LINE|COUNT and points the whole dynamic table at `lj_vm_inshook`.
- `vm_inshook` (`vm_x64.dasc:2228`) does `dec dword [hookcount]; jz -> lj_dispatch_ins`.
- `lj_trace_exit` (`lj_trace.c:920-931`) is the only way back into the interpreter dispatch from machine code.

Machine-code check: `luajit -jdump=m -e "local x=0 for i=1,1000 do x=x+i end"`. On stock, the `->LOOP` body is `xorps/cvtsi2sd/addsd/add/cmp/jle ->LOOP` with no memory access to `hookmask`. On CHECKHOOK it contains `test byte [rbx-0xef7], 0xc; jnz` both before and after `->LOOP`.

Experiment: my 20 cases (`W/vh/cases/c01..c20`), `./run.sh <variant> <method> error c`:
- **stock + async sethook: 1 stopped, 19 HANG.** In every hang `vmstate=TRACE#n` and `hookmask=0x08` (COUNT set, never serviced). The only case that stopped was `jit.off() while true do end` (3.8 µs).
  - My cases include: `while true`, `for i=1,math.huge`, tail recursion, mutual tail recursion, up-recursion, `pairs`, `math.sqrt`, global increment, `coroutine.wrap` ping-pong, table store, `s:byte`, `repeat`, a sunk allocation `{1,2,3}` (allocation sinking removes the GC exit), an inlined call, nested `for`, a side-trace branch, `select('#')`, `math.random` and `s:sub`.
- CHECKHOOK + async sethook: 20/20 stopped. Patched + `luaJIT_interrupt`: 20/20 stopped, in 0.8-13.8 µs.
- Yield test (`vh2 yieldtest`): a coroutine runs a 2e9-iteration branchy loop and requests come every 2 ms.
  - stock: **1 hook-yield out of 1294 requests**;
  - CHECKHOOK: 1282/1282;
  - patched: 1290/1290.
  - Log: `logs/pp1-yieldtest.log`.

Cross-check of the investigator's logs: `cases-stock-sethook.log` has 7 OK and 21 HANG over 28 cases (18 of the 25 normal cases plus the 3 X cases), which is consistent with their claim. Minor point: the "Trace aborted asynchronously" default case is at `lj_trace.c:739`, not 734.

## PP-4: cross-thread `lua_sethook` (confirmed and reproduced; high, could be critical)

Source:
- `lua_sethook` is `g->hookmask = (g->hookmask & ~HOOK_EVENTMASK) | mask`, a non-atomic read-modify-write.
- `lj_trace_abort` is the macro `(G2J(g)->state &= ~LJ_TRACE_ACTIVE)` at `lj_trace.h:45`.
- `lj_dispatch_update` reads `J->state` and rewrites the dispatch table and `g->dispatchmode` with no lock.
- `hook_enter`/`hook_leave` are plain `|=`/`&=` operations (`lj_obj.h:679,683`).
- On Windows, `callhook` takes `profile_lock` only while the profiler runs (`lj_profile_hook_enter`). That shows cross-thread hookmask writes need a lock.

LuaJIT #824, fetched through the GitHub API (`W/dl-gh/issue824*.json`). Mike Pall wrote that `lua_sethook()` (only when setting a count hook) is "signal-safe, but not thread-safe". The reporter traced the assert to line 336 eating the update from line 373, and the issue was closed as API misuse.

Counter-evidence found: upstream's own CHECKHOOK comment (`lj_record.c:2890-2892`) says the hook can be set "from a signal handler or another native thread". It predates #824 and is contradicted by both #824 and my tests. Implementers following the in-source comment will make this mistake.

The original mod (`git show origin/1.14.4_CCT:...`):
- `main.cpp:815-818` `abortMachine` calls `lua_sethook(L, thread_interrupt_hook, LUA_MASKCOUNT, 1)`.
- The coremod (`cclj_txfm.js`, "ComputerExecutor::abort") replaces `ILuaMachine.close()` in `ComputerExecutor.abort()` with `LuaJITMachine.abort()`.
- In CC:T 1.86.2, `ComputerThread.Monitor.run()` calls `executor.abort()` from the **monitor thread** (`ComputerThread.java:395-396`, fetched from GitHub tag v1.14.4-1.86.2 into `W/dl-cct186/`).
- So the cross-thread call path is real. A second call path, from `handleEvent` on a soft abort (`LuaJITMachine.java:196`), runs on the VM thread before `lua_resume`.
- `abort()` is guarded by an `aborted` flag, so an update lost to the race is never retried.

Experiments:
1. **HOOK_ACTIVE race** (`vh2 race`). The VM runs `jit.off()` loop under a count hook; the other thread calls `lua_sethook(L,h,MASKCOUNT,...)` in a loop.
   - Assert build (3 runs of 5 s): twice **HOOKS DEAD** (`hookmask=0x18`, ACTIVE stuck while not in a hook, 0 hook calls afterwards) and once `LuaJIT ASSERT lj_dispatch.c:376: callhook: active hook flag removed`.
   - Release build: stuck HOOK_ACTIVE in 12/12 runs, even with only 10 ms of racing.
   - At a realistic rate (1 request/ms, VM hook count 1e4): 2 of 3 10-second runs died. At 1 request per 10 ms: 0 of 3.
   - Logs: `logs/pp4-race-*.log`.
2. **Recorder/dispatch race** (`vh3`). No hook is ever installed (mode 0 is just `lua_sethook(L,NULL,0,0)` from another thread).
   - Assert build: 6/6 runs hit `lj_trace.c:794: trace_hotside: hot side exit while recording` (once `active hook flag removed`). This means the other thread's `lj_dispatch_update`/`lj_trace_abort` desynchronised the dispatch mode from `J->state`, so machine code ran while recording.
   - **Release build: Segmentation fault in 6 of 8 runs** (2/4 mode 0, 4/4 mode 1). **Control (mode 2, same workload, the racer makes no LuaJIT calls): 8/8 clean** on both builds.
   - At 1 call/ms for 20 s, the assert build still tripped `hot side exit while recording` in 3/3 runs. The release build did not crash in 3 runs of about 1300 calls each.
   - Logs: `logs/pp4-jstate-race*.log`.

Assessment: the claim is correct and understated. Besides "can never be interrupted again", cross-thread `lua_sethook` can **crash the process** (in Minecraft that is the whole JVM/server) and corrupt recorder state. The per-call probability at realistic rates is low, but it is not zero. Note: Pall's suggested safe form is a signal delivered to the VM thread (SuspendThread on Windows is the analogue). That is an alternative the investigator did not evaluate. The patch's `luaJIT_interrupt` (two plain stores) avoids the whole class of problem.

## PP-5: CHECKHOOK plus an installed count hook (partially confirmed; medium)

The upstream CHECKHOOK comment itself says "Note this is only useful if hooks are *not* set most of the time" (`lj_record.c:2888`). Benchmarks were run with `vh2 bench`, 3 passes; the C hook does one InterlockedExchange. Median ms:

| bench | stock | stock+hook 1e4 | CHECKHOOK+hook 1e4 | patched+hook 1e4 | stock jit.off | CHECKHOOK slowdown vs JIT / vs interp |
|---|---|---|---|---|---|---|
| fib(35) | 61 | 61 | 1641 | 61 | 271 | 27x / 6.1x |
| mandel(1500) | 80 | 81 | 6189 | 81 | 514 | 77x / 12x |
| tbl | 52 | 52 | 3847 | 52 | 206 | 74x / 18.7x |
| strs (tostring/GC heavy) | 152 | 153 | 538 | 154 | 228 | **3.5x / 2.4x** |

- CHECKHOOK with a Lua hook at N=1e7 instead of 1e4: mandel took 6165 ms. The cost does not depend on N, because the guard tests only the mask.
- Trace counts from `jit.attach` (start/stop/abort): CHECKHOOK+hook gives 104/308/219/102 traces started, vs 20/5/3/2 on stock. With `jit.util.traceinfo`, mandel had 410 traces `{interpreter=4 loop=51 return=101 root=253 stitch=1}` vs 4 on stock and on patched+hook.
- Patched + hook: within ±2% of stock, with the same trace counts.

Correction: the slowdown range is workload-dependent, 3.5x-77x rather than 25-73x. Code dominated by C library and GC work suffers much less. The finding matters only if someone picks "CHECKHOOK + permanent hook". Upstream itself warns against that, so medium.

Logs: `logs/pp5-bench-run1.csv`, `logs/pp5-bench-run23.csv`, `logs/pp5-linktypes.log`.

## PP-6: hook yield goes to the innermost coroutine (confirmed; medium)

- `vh intr|sethook yield cases/n1_nested_inner.lua` on both patched and CHECKHOOK: `inner.resume returned true with 0 extra values; inner status=suspended`. The host's first `lua_resume` returned the outer's own `coroutine.yield("outer-filter")`.
- CC:T-shaped test (`vh2-patched events cases/n2_parallel.lua timer key mouse`). The test re-implements ROM `parallel.waitForAny` with `os.pullEventRaw = coroutine.yield`:
  - The hook yield makes the top level yield to the host with **0 values**, which looks exactly like a normal "wait for any event".
  - The host then delivers `key`. parallel resumes the routine with `key`, the hook yield returns, and **`key` is discarded**: the program log goes "prog finished loop", then "prog got mouse", and the program never saw `key`.
  - Without an event the program would stall forever.
- Facts checked:
  - The BIOS runs the shell inside `pcall(parallel.waitForAny, ...)` (`rom-1.21.1/.../bios.lua`, final block).
  - Cobalt pauses with `UnwindThrowable.suspend()`, which unwinds the whole runtime (`LuaState.java:194-210`). `CobaltLuaMachine.handleEvent` returns `PAUSE` when results are null.
  - `TimeoutState.isPaused()` is a fairness mechanism used when other computers are waiting.
- Severity: medium rather than high. Not supporting pause is a working fallback, with a fairness cost (a busy computer monopolises a worker for up to 7 s). The investigator's token-forwarding idea, or a dedicated OS thread per computer that blocks in the hook, are alternatives.
- Log (CHECKHOOK half): the CHECKHOOK run was cut off by my 30 s timeout because my test hook did not clear itself. That incidentally reproduces PP-5. The relevant first yield is shown. Logs: `logs/pp6-*.log`.

## PP-7: xpcall handler runs inside the hook (confirmed; mitigation incomplete; high)

- Source: `lj_err_run` (`lj_err.c:879-900`) calls the handler through `lj_vm_call` before `lj_err_throw`. `HOOK_ACTIVE` is cleared only during unwinding (`lj_err.c:155,185`) or by `hook_leave` in `callhook`, so the handler runs with hooks off.
- Results (`logs/*-h.log`). Case h2 is the investigator's case; h4 and h5 are my counter-examples:

| case | patched harderr | patched hardyield | CHECKHOOK harderr | CHECKHOOK hardyield | **patched + 1-line fix** (both) |
|---|---|---|---|---|---|
| h1 `pcall` swallow | stop 9.9 µs | stop 12.9 µs | stop | stop | stop |
| h2 `xpcall` handler loop | **HANG** (hookmask 0x18) | stop 8.0 µs | HANG | stop | stop |
| h3 coroutine swallow | stop | stop | stop | stop | stop |
| **h4** `xpcall(function() string.gsub("a",".",function() while true do end end) end, function() while true do end end)` | HANG | **HANG** | HANG | **HANG** | stop |
| **h5** same with a `table.sort` comparator | HANG | **HANG** | HANG | **HANG** | stop |
| h6 h2 under `jit.off()` | HANG | stop | HANG | stop | stop |

- The patched-assert build gives identical results.
- Inside a gsub callback or a sort comparator, `lua_isyieldable` is false. The yield-based abort therefore falls back to `luaL_error`, which runs the looping handler with hooks disabled.
- Verified fix (`W/errfix-hook-xpcall.patch`): in `lj_err_run`, `if (hook_active(G(L))) ef = 0;` under `LUAJIT_ENABLE_INTERRUPT`. Errors raised from inside a hook then skip the message handler.
  - Result: 6/6 stopped with both abort styles (`logs/patched-errfix-*.log`).
  - Sanity check (`logs/errfix-sanity.log`): normal `xpcall` handlers still run with a full traceback (6 lines). Only errors raised from Lua debug hooks skip the handler.
- This is a regression vs Cobalt, whose interrupt check runs on every instruction regardless of context.

## PP-8: finalizers, Lua hooks, C loops (confirmed; high)

`logs/*-x.log`: on stock, CHECKHOOK, patched and patched-assert alike:
- `x1` newproxy `__gc` loop hangs (`hookmask=0x58` = COUNT|ACTIVE|GC; `gc_call_finalizer` uses `hook_entergc`, `lj_gc.c:507-521`).
- `x2` `debug.sethook(loop,"",1)` hangs (0x18).
- `x3` `string.find(rep('a',40), rep('a*',20)..'b')` hangs (`vmstate=C`).

Extra case `x4` `debug.sethook() while true do end`:
- It stops on CHECKHOOK + async sethook, because a new hook is installed.
- It **hangs on patched** with `hookmask=0x00` and `vmstate=EXIT`. The host's permanent hook was removed, so `intr` is never cleared and every trace exits at every check.
- One call to `debug.sethook()` therefore disables preemption permanently in the recommended design.

Cobalt comparison:
- Cobalt polls interrupts in the pattern matcher (`StringMatch.java:513` `singlematch`, `:528` `match`, via `handleInterruptWithoutYield`).
- It polls them on every interpreter instruction, independent of the debug-hook `inhook` flag (`LuaInterpreter.java:177`, `DebugState.java:261-274`).
- It has no `__gc`/`newproxy` (grep finds none).
- So all three hangs are **regressions vs stock CC:T**. This answers the investigator's open question.
- `x1` and `x2` are fixed by removing `newproxy`/`debug.sethook`, or by wrapping `debug.sethook` so it chains to the host hook. `x3` needs a LuaJIT-side poll of `g->intr` in `lib_string.c` `match()`, in the way Cobalt does it.

Other checks:
- `debug.sethook` is exposed by CC:T: `CobaltLuaMachine.java:79` calls `CoreLibraries.debugGlobals`, which calls `DebugLib.add`, and `DebugLib.java:88` registers `"sethook"`.
- The ROM never calls `sethook`: grep finds nothing.
- The ROM uses `xpcall` only as a function identity (`cc/internal/exception.lua:105`).

## Missed issues (in this lane)
1. **Cross-thread `lua_sethook` can segfault the process**, not only leave hooks dead. Reproduced in 6 of 8 release runs, with a clean control. The upstream in-source comment wrongly recommends it.
2. **Yield-based hard abort can be bypassed** (h4/h5): `xpcall` plus a non-yieldable C boundary. A one-line `lj_err_run` patch fixes it; it is verified, and the patch is at `W/errfix-hook-xpcall.patch`.
3. **All PP-8 hangs are regressions vs Cobalt.** Cobalt interrupts pattern matching, so `string.find` needs an extra LuaJIT patch (poll `intr` in `lib_string.c` `match`), not just sandbox removals.
4. **`debug.sethook()` with no arguments kills preemption** in the patched design (x4: livelock in EXIT). Any user program can do this, so `debug.sethook` must be wrapped or removed before release. A related compat note: a user's own count hooks never fire inside JIT code on the stock or patched builds.
5. **Concrete PP-6 effect: events are lost.**

## Files
- Harness sources: `W/vh/vh.c`, `W/vh/vh2.c`, `W/vh/vh3.c`, `W/vh/run.sh`.
- Cases: `W/vh/cases/`. Benchmarks: `W/vh/bench/`. Logs: `W/vh/logs/`.
- Builds: `W/src-{stock,stock-assert,checkhook,patched,patched-assert,patched-errfix}/src`.
- Fix patch: `W/errfix-hook-xpcall.patch`. Downloaded references (data): `W/dl-gh/`, `W/dl-cct186/`.
