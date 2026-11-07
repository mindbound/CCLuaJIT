# preempt-patch: preempting JIT-compiled LuaJIT code from another thread

Lane: find and prototype a robust way to interrupt / yield / abort JIT-compiled LuaJIT code from another thread, so CC:T's "Too long without yielding", pausing and hard abort work in CCLuaJIT on 1.21.1.

Work dir: `<scratch>\work\preempt-patch` (see `README.txt` there). All paths below are relative to that directory unless they are absolute.

## TL;DR

1. **Why hooks do not fire in traces.** Debug hooks exist only in the interpreter's dispatch table. Machine code never dispatches bytecode. A looping trace jumps to its own `->LOOP` label. A trace link (root, loop, tail-, up- or down-recursion) jumps straight into other machine code. Stock LuaJIT therefore never checks `hookmask` until a trace exits. Measured on the stock build: an async `lua_sethook(L, h, LUA_MASKCOUNT, 1)` from a second thread **failed to stop 18 of 25** test loops.
2. **Upstream already has the trace-side mechanism.** It is the compile-time option `LUAJIT_ENABLE_CHECKHOOK` (added 2011, commit 6bce6b1; Mike Pall calls it "unsupported"). With `make XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK` and no source patch, all **25/25** cases stopped within 2-17 µs. The check compiles to a single `test byte [rbx-0xef7], 0xc; jnz ->exit` at the start of every trace and inside every loop body.
3. **The protocol the original mod used is not safe.** That protocol is calling `lua_sethook` from another thread, as `abortMachine` did in 1.14.4. Mike Pall stated in LuaJIT issue #824 that `lua_sethook` is signal-safe but **not thread-safe**. Its read-modify-write on `hookmask` can clear `HOOK_ACTIVE`, which gives an assertion or a segfault. It also races on `J->state` (through `lj_trace_abort`) and on the dispatch table. With CHECKHOOK there is also a performance trap: a count hook that stays installed makes every trace exit at every iteration.
4. **Prototype patch: `luajit-preempt.patch`.** It adds 70 lines in 6 files, all behind `-DLUAJIT_ENABLE_INTERRUPT`:
   - a dedicated `volatile uint8_t intr` field in `global_State`;
   - `LUA_API void luaJIT_interrupt(lua_State *L)`, which does only 2 plain stores and so is safe from any thread or signal handler;
   - the CHECKHOOK-style guard at every trace start, testing `intr` instead of `hookmask`;
   - trace exits taken while an interrupt is pending neither spawn side traces nor count as hot, and they make the count hook run at the next instruction;
   - the count hook clears `intr`.

   The host installs one count hook that stays in place, from the VM thread, with a count of about 1e4.

   **Results:** 25/25 cases stopped in about 1-14 µs, including the interpreter-only case. The assert build gave the same result with no assertion failures. In the yield test, a coroutine was preempted about 400 times and the checksum still matched exactly. JIT compilation keeps working afterwards.
5. **Overhead** (8 benchmarks, medians):
   - **JIT code:** the guard has **no measurable cost**. In pass 2 (9 runs) the patched build was -0.1% geomean and patched plus the installed count hook was -0.05%. Pass 1 (5 runs) gave +2.3% and +1.9%. The largest consistent per-benchmark signal is +1.1-1.5% (nbody, spectralnorm); run-to-run noise is ±2-3%.
   - **Cold, interpreted code:** the count hook that stays installed (needed for the interpreter path) costs **+22-24% geomean**, ranging from +1% to +65%. JIT-compiled code is not affected by it.
   - **CHECKHOOK with an installed hook:** that combination is a **25-73x slowdown**, which is why the patch uses its own `intr` byte.
   - **Alternatives:** `jit.off()` is 1.2-60x slower than JIT, **6.8x geomean**. Source instrumentation costs **+27% geomean**, ranging from -8% to +163%. A child process per computer costs **7.3 µs per IPC round trip** and about 5 MB per process (see §6).
6. **Hazards the host design must handle.** Each of these was reproduced, and each defeats hook-based preemption in LuaJIT. The first two follow from Lua 5.1 hook semantics; Cobalt avoids them because it suspends the whole VM and polls interrupts outside a hook context:
   - **Hook yields go to the innermost coroutine.** A yield from a hook goes to the innermost coroutine's Lua resumer, not to the host. CC:T runs every program inside `parallel`/`multishell` coroutines, so a CC:T-style **pause cannot be a plain hook yield**.
   - **`xpcall` handlers run with hooks off.** An `xpcall` message handler runs *inside the hook context*, with hooks disabled. `xpcall(f, function() while true do end end)` makes an error-based abort hang forever. Aborting by yielding to the host and then closing the state works.
   - **`__gc` finalizers and Lua hook functions are uninterruptible.** This covers finalizers created through `newproxy` and Lua-level hooks set with `debug.sethook`. CC:T exposes `debug.sethook` through Cobalt.
   - **C-level hangs cannot be interrupted at all.** Example: catastrophic `string.find` backtracking.

**Recommendation:** ship `luajit-preempt.patch` with the 1.21.1 port, built with `XCFLAGS=-DLUAJIT_ENABLE_INTERRUPT`, and use the host protocol in §7. Building with CHECKHOOK plus async `lua_sethook` is acceptable only as a stop-gap. Do not use `jit.off()` or process isolation as the primary mechanism.

## 1. Why hooks don't fire in compiled code (source walk, LuaJIT 19878ec0)

- **Hooks are a dispatch-table feature.**
  - `lua_sethook` (`src/lj_dispatch.c:329-340`) stores `hookf`, `hookcount` and `hookmask`, aborts any recording (`lj_trace_abort`) and calls `lj_dispatch_update`.
  - `lj_dispatch_update` (`lj_dispatch.c:106-210`) sets `DISPMODE_INS` when `hookmask & (LUA_MASKLINE|LUA_MASKCOUNT)` (`:118`). In that mode it rewrites the whole dynamic dispatch table to `lj_vm_inshook` (`:161-168`).
  - `lj_vm_inshook` (`src/vm_x64.dasc:2228-2253`) does `dec dword [hookcount]; jz -> call lj_dispatch_ins`. `lj_dispatch_ins` (`lj_dispatch.c:403ff`) then calls `callhook` once the count reaches 0.
  - So a hook can fire only when the interpreter dispatches an instruction.
- **Traces never dispatch.**
  - A looping trace branches back to `->LOOP` inside its own machine code. See the stock mcode dump of `for i=1,1000 do x=x+i end`: the loop body is `cvtsi2sd/addsd/add/cmp/jle ->LOOP` with no memory access to `hookmask`.
  - Root, up-recursion, tail-recursion and down-recursion links (`LJ_TRLINK_*`, `src/lj_jit.h:246-254`) are direct jumps to another trace's machine code.
  - Only a guard failure goes back through `lj_trace_exit` (`src/lj_trace.c:874-960`) and `->vm_exit_interp`, which re-enters the dynamic dispatch. So code that keeps cycling through traces never sees a hook.
  - This is what the earlier verification observed: a count hook does not fire inside a JIT-compiled `while true do end`.
- **The recorder does react to hooks, but not in a useful way here.**
  - Every hook call aborts recording: `callhook` calls `lj_trace_abort` at `lj_dispatch.c:364`.
  - `lj_trace_abort` is a non-atomic `J->state &= ~LJ_TRACE_ACTIVE` (`src/lj_trace.h:45`). The state machine's `default:` case is the "Trace aborted asynchronously" path (`lj_trace.c:734`).
- **`IR_PROF`, the sampling profiler's guard, is the right pattern but the wrong tool.**
  - How it works:
    - `rec_profile_ins` and `rec_profile_ret` (`src/lj_record.c:756-772`) emit `IR_PROF` plus a snapshot.
    - In mode `'f'` this happens only when the function changes. In mode `'l'` it happens only when the line changes (`rec_profile_need`, `:733-754`).
    - `asm_prof` (`src/lj_asm_x86.h:2674-2680`, and equivalents in the arm, arm64, mips and ppc backends) is `test byte [&g->hookmask], HOOK_PROFILE; jnz ->exit`.
    - The fold rule drops only an `IR_PROF` that directly follows another `IR_PROF`. Otherwise it emits without CSE (`src/lj_opt_fold.c:2446-2452`).
    - `lj_trace_exit` treats a pending `HOOK_PROFILE` as "just exit to interpreter", with no hot-side-exit counting (`lj_trace.c:926-927`).
    - The trigger runs on a timer thread (Windows) or from `SIGPROF`. It sets `HOOK_PROFILE` and calls `lj_dispatch_update` under `profile_lock`.
  - Why it doesn't work here:
    - A single-function, single-line loop such as `while true do end` gets **no** `IR_PROF` in either mode.
    - The profiler is a process-wide singleton: `static ProfileState profile_state` (`src/lj_profile.c:90`), and a second VM is refused with "Profiler in use by another VM" (`:328`). That is unusable with many computers per JVM.
    - The profiler callback is not a hook, so it cannot yield.
  - What I reused from it: the idea of a guard that checks a global byte and then exits the trace, and the special-casing of those exits in `lj_trace_exit`.
- **Upstream already ships the trace-side half: `LUAJIT_ENABLE_CHECKHOOK`** (`lj_record.c:2883-2900`, plus the ITERN fix-up at `:670-672`).
  - At the end of `lj_record_setup` it emits `XLOAD u8 [&g->hookmask] volatile; BAND (LINE|COUNT); EQ 0` as a guard. That puts the check at the start of every root and side trace.
  - Because the load is volatile, the loop optimizer copies the check into the LOOP body rather than hoisting it.
  - Verified IR and mcode (b-checkhook, `-jdump=im`): `0002 u8 XLOAD [..] V / 0003 BAND +12 / 0004 > EQ +0`, repeated after `LOOP` (`0011-0013`). The x64 code is `test byte [rbx-0xef7], 0xc; jnz ->0`, and again inside `->LOOP`.

### Why a check at trace start covers every cycle

Every infinite execution that stays in machine code must repeatedly pass a trace start:

- **Loop back-edges inside a trace:** the check is copied into the loop body.
- **Every trace link** goes to the start of a trace, and every trace ends either in a link or in an exit to the interpreter.
- **Side exits** that are patched to side traces form a tree, so they cannot form a cycle on their own.
- **Stitched traces** and returns continue through interpreter dispatch, where the count hook runs.

Link types exercised in the harness: `loop`, `root`, `tail-recursion` (self and mutual), `up-recursion` (verified with `-jv`), `stitch`, `return`, and `interpreter`. I could not construct a down-recursion trace with `-jv` attempts; it is covered by construction (a self-link to the trace start).

## 2. Prior art

- **LuaJIT #779, "Is there a way to interrupt the execution of JIT code" (2021).** Mike Pall pointed to the comments at the end of `lj_record.c` and `make XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK`, called it "unsupported", said to use `LUA_MASKCOUNT` and never `LUA_MASKLINE`, and said the check counts against CPU execution bandwidth in loops that actually compute. https://github.com/LuaJIT/LuaJIT/issues/779
- **LuaJIT #824 (2022).** A background pthread calling `lua_sethook()` while a coroutine yields from the hook triggers `lj_assertG(hook_active(g), "active hook flag removed")` or a segfault in `lj_dispatch_ins`. Pall said `lua_sethook` is signal-safe for count hooks but **not thread-safe**: it relies on memory ordering, not locks. The issue was closed as invalid (API misuse). https://github.com/LuaJIT/LuaJIT/issues/824
- **LuaJIT #723, "Terminate lua code" (2021)** and **#1488, "debug hook not reached with jit.on()" (2026).** Both got the same answer: JIT code does not check hooks by default, rebuild with CHECKHOOK. https://github.com/LuaJIT/LuaJIT/issues/723 , https://github.com/LuaJIT/LuaJIT/issues/1488
- **The upstream commit.** 6bce6b118eeb, 2011-01-19: "Add compile-time option LUAJIT_ENABLE_CHECKHOOK. Disabled by default."
- **The LuaJIT FAQ.** The Ctrl-C handler sets a hook that compiled code ignores. On sandboxing untrusted code it calls the process level "the only promising approach". https://luajit.org/faq.html
- **Tarantool's LuaJIT fork.** It added a CMake option for CHECKHOOK (off by default) and fixed tests that break when it is enabled. https://lists.tarantool.org/pipermail/tarantool-patches/2022-December/027270.html ; its hook tests turn the JIT off: https://lists.tarantool.org/pipermail/tarantool-patches/2021-March/023025.html
- **OpenResty (2014).** agentzh suggested a custom OS thread that calls `lua_sethook()` with count 1, together with `-DLUAJIT_ENABLE_CHECKHOOK`, as a last resort. https://groups.google.com/g/openresty-en/c/NsmFB48gl-I
- **CraftOS-PC Accelerated**, a ComputerCraft emulator on LuaJIT: https://github.com/MCJack123/craftos2/releases/tag/v2.2.1-luajit
  - Its release notes say compiled code ignores the hook. Its workaround (hook count 1) used about 75% of CPU, and it offers `os.setHaltOnLongRunMode(false)` to turn it off.
  - Its LuaJIT fork (github.com/MCJack123/craftos2-luajit) has no trace-check changes.
  - Its `src/Computer.cpp` on the `luajit` branch installs `LUA_MASKCOUNT|LINE|RET|CALL` hooks with count 1e6. That still cannot stop JIT loops.
  - Prior art therefore leaves this problem unsolved for CC-style use.
- **Garry's Mod wiki.** It notes that hooks are not always run for JIT-compiled code, so relying on them for infinite-loop protection is unwise. https://wiki.facepunch.com/gmod/debug.sethook~diff:549695
- **lua-mtint.** It notes that JIT machine code can't be interrupted. https://awesome.ecosyste.ms/projects/github.com%2Fosch%2Flua-mtint
- **OpenResty luajit2, RaptorJIT, moonjit.** A GitHub code search of openresty/luajit2 found only upstream's CHECKHOOK block. I found no interrupt mechanism in these forks. Tarantool carries the same upstream option.
- **The original CCLuaJIT 1.14.4** (`git show origin/1.14.4_CCT:src/main/cpp/main.cpp`, lines 815-818):
  - `abortMachine` calls `lua_sethook(L, thread_interrupt_hook, LUA_MASKCOUNT, 1)` from the Java caller's thread. That is the unsafe pattern from #824.
  - LuaJIT was built without CHECKHOOK (Dockerfile: `make BUILDMODE=dynamic`), so it could never stop a JIT loop.
  - The yield-request path (main.cpp:463-466) arms a one-shot hook that yields, and runs only after a Java API call returns.

## 3. Design of the patch (`LUAJIT_ENABLE_INTERRUPT`)

Goals:
- the check is a single load, test and branch on a per-`global_State` byte;
- it costs nothing when no interrupt is pending;
- the request side is genuinely thread-safe;
- the IR is portable;
- the feature is opt-in.

Changes (`luajit-preempt.patch`, 153 lines of diff, +70 lines):

| file | change |
|---|---|
| `src/lj_obj.h` | `volatile uint8_t intr;` after `vmevmask`. This sits in existing padding next to `hookmask`, at `DISPATCH-0xef4` on x64. It is unconditional so the struct layout is the same for every build flag. |
| `src/luajit_rolling.h` | Declares `LUA_API void luaJIT_interrupt(lua_State *L);` with documentation. |
| `src/lj_dispatch.c` | `luaJIT_interrupt`: `g->hookcount = 1; g->intr = 1;` and nothing else. It does not touch `hookmask`, the dispatch table or `J->state`. `callhook` clears `intr` when it invokes a count hook. |
| `src/lj_record.c` | At the end of `lj_record_setup` (unless the trace already ended there), emits `XLOAD u8 [&g->intr] V; BAND 0xff; EQ 0` as a guard. This is the same IR shape as CHECKHOOK. `rec_itern` gets the same +3 ref fix-up that CHECKHOOK needs. |
| `src/lj_trace.c` | In `lj_trace_exit`: while `intr` is pending, do not call `trace_hotside`, so no side traces are recorded from check exits. If a count hook is installed, set `hookcount = 1` (VM thread) so the hook runs at the next interpreted instruction. |
| `src/Makefile` | Documents `#XCFLAGS+= -DLUAJIT_ENABLE_INTERRUPT`. |

Generated code on x64, checked with `b-patched/src/luajit.exe -jdump=im`: `test byte [rbx-0xef4], 0xff; jnz ->0`. It appears once in the prelude and once after `->LOOP`, with no extra register.

Host contract:
- Install `lua_sethook(L, hook, LUA_MASKCOUNT, N)` once, from the VM thread, and keep it installed.
- Interrupt with `store(flag); luaJIT_interrupt(L)` from any thread.
- The hook does `if (!xchg(flag,0)) return; ...`.

How a request reaches the hook:
- **In JIT code:** the next trace start or loop iteration exits; `lj_trace_exit` sets `hookcount = 1`; the hook runs on the next bytecode.
- **In the interpreter:** the async `hookcount = 1` is only a hint. The interpreter's non-atomic `dec [hookcount]` with store-forwarding almost always overwrites it in a tight loop. Measured: with N=1e8, `jit.off(); while true do end` took 76 ms to see the request. Interpreter latency is therefore bounded by N instructions: 1.0 µs at N=1e4, 51 µs at 1e5, 927 µs at 1e6.

Correctness notes:
- **Snapshots.** The guard is emitted right after snapshot #0. For a root trace that snapshot points at the instruction after the loop op; for a side trace it is the replayed parent snapshot. The guard comes before any side effect of the trace. In the loop body copy it uses the loop snapshot. An exit therefore always resumes the interpreter at a bytecode boundary at the start of an iteration or of the trace.
- **State after resume.** Verified by the yield test: about 400 preemptions in the middle of a loop, with the checksum identical to the uninterrupted reference.
- **Portability.** Only generic IR is used (`XLOAD`/`BAND`/`EQ`) and there are no backend changes. Upstream CHECKHOOK uses the same IR on x86, x64, arm, arm64, ppc and mips. Only x64 with GC64 was tested here; arm64 is untested (medium confidence).
- **Why not reuse `IR_PROF`.** It would need a new operand and changes in all 5 backends, and it gives no benefit over the fused `test byte [mem], imm` that x64 already produces.
- **Opt-in.** The feature is compile-time only. A runtime toggle could gate emission on a `J->flags` bit and flush traces when toggled. Not done, because CCLuaJIT always wants it on.
- **Lifetime.** `luaJIT_interrupt` must not be called after `lua_close`. The host needs a lock between `close()` and the monitor thread.
- **If no count hook is installed,** a pending `intr` makes every trace exit at every check and nothing clears it. The host must install the hook.

## 4. Empirical results (x64, MinGW gcc 16.2, Windows 11, 24 cores)

Builds, all `BUILDMODE=static` from the vendored commit:

| build | flags |
|---|---|
| `b-stock` | default |
| `b-checkhook` | `XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK` |
| `b-patched` | patch + `-DLUAJIT_ENABLE_INTERRUPT` |
| `b-patched-assert` | patch + `-DLUAJIT_ENABLE_INTERRUPT -DLUA_USE_ASSERT -DLUA_USE_APICHECK` |

Harness: `harness/harness.c`, run by `harness/run_all.sh <variant> <method>`. Each case runs in its own process with a 10 s kill timeout.

1. The main thread runs the case under `lua_pcall`.
2. After about 200 ms the interrupter thread samples `G(L)->vmstate` 100 times over 100 ms. A non-negative value means the VM is executing trace #n.
3. The interrupter then requests an interrupt and measures the time until the hook runs. The hook raises an error.

Request methods:
- `sethook`: async `lua_sethook(L, h, LUA_MASKCOUNT, 1)`, the brief's protocol and the original mod's.
- `intr`: a count hook installed permanently (N=1e4) from the VM thread, plus `luaJIT_interrupt`.

Logs: `logs/cases-*.log`.

| case | VM state at request (stock) | stock + async lua_sethook | CHECKHOOK + async lua_sethook | patched + luaJIT_interrupt | patched, assert build | link types created |
|---|---|---|---|---|---|---|
| while_true | TRACE#1 | HANG | 17.3 µs | 12.8 µs | 11.3 µs | loop |
| numeric_for_1e12 | TRACE#1 | HANG | 7.7 µs | 6.5 µs | 14.9 µs | loop |
| repeat_until | TRACE#1 | HANG | 6.2 µs | 7.1 µs | 12.8 µs | loop |
| goto_loop | TRACE#1 | HANG | 6.6 µs | 7.3 µs | 10.3 µs | loop |
| tail_recursion | TRACE#1 | HANG | 8.3 µs | 6.1 µs | 48.6 µs | tail-recursion |
| mutual_tail_recursion | TRACE#1 | HANG | 7.2 µs | 8.7 µs | 14.2 µs | tail-recursion |
| up_down_recursion_loop | TRACE#1 | HANG | 5.9 µs | 5.4 µs | 6.0 µs | up-recursion + side (-jv) |
| deep_recursion_loop | TRACE#1 | 10.0 µs | 6.4 µs | 12.5 µs | 1.1 µs | up-recursion (-jv) |
| loop_calls_lua_fn | TRACE#1 | HANG | 7.4 µs | 7.9 µs | 12.0 µs | loop |
| nested_loops | TRACE#1 | HANG | 7.9 µs | 8.7 µs | 8.3 µs | loop, root×2 |
| side_traces | TRACE#1 | HANG | 7.7 µs | 7.7 µs | 8.6 µs | loop, root×2 |
| coroutine_pingpong | INTERP | HANG | 5.5 µs | 7.7 µs | 11.4 µs | root×2, stitch×2 |
| loop_inside_coroutine | TRACE#1 | HANG | 7.4 µs | 10.3 µs | 13.3 µs | loop |
| string_concat | GC | 3.5 µs | 2.8 µs | 2.6 µs | 9.5 µs | loop, root |
| table_concat | GC | 224.6 µs | 2.5 µs | 2.9 µs | 3.1 µs | loop×2, root×3 |
| pairs_loop (ITERN) | TRACE#1 | HANG | 3.6 µs | 6.1 µs | 8.9 µs | loop, root×3 |
| ipairs_loop | TRACE#2 | HANG | 7.5 µs | 7.4 µs | 7.4 µs | loop, root |
| pcall_loop | TRACE#1 | HANG | 6.5 µs | 5.7 µs | 9.3 µs | loop |
| stitch_c_call (os.clock) | TRACE#1 | HANG | 6.0 µs | 5.1 µs | 11.1 µs | root, stitch |
| alloc_loop | GC | 12.2 µs | 1.8 µs | 1.4 µs | 1.8 µs | loop, root×3 |
| metamethod_loop | TRACE#1 | HANG | 6.2 µs | 6.0 µs | 10.9 µs | loop |
| gsub_callback | INTERP | 9.4 µs | 5.1 µs | 2.5 µs | 7.0 µs | return |
| load_loop | TRACE#103 | 4.6 µs | 8.1 µs | 1.3 µs | 4.0 µs | root×102, stitch, interp |
| interp_only_jit_off | INTERP | 6.3 µs | 4.4 µs | 1.0 µs | 8.7 µs | - |
| bit_ops | TRACE#1 | HANG | 6.6 µs | 6.5 µs | 11.2 µs | loop |
| **total stopped** | | **7/25** | **25/25** | **25/25** | **25/25** | |
| X_finalizer_loop (newproxy `__gc` with an infinite loop) | INTERP | HANG | HANG | HANG | HANG | runs with `HOOK_ACTIVE|HOOK_GC` |
| X_lua_hook_loop (`debug.sethook(function() while true do end end,"",1)`) | TRACE | HANG | HANG | HANG | HANG | the hook function runs with hooks disabled |
| X_string_find_backtrack (`string.find(rep('a',40), rep('a*',20)..'b')`) | C | HANG | HANG | HANG | HANG | stuck in C code |

Notes on the table:
- "HANG" means no hook ran within 3 s after the request. The case's own sampler confirms the VM was still in `TRACE#1` (stock) or `EXIT` (CHECKHOOK livelock on `X_lua_hook_loop`).
- The "link types" column for the recursion cases is empty in the logs because those traces were flushed by the error. They were confirmed separately with `-jv`.

### Yield-from-hook, resume, and JIT afterwards

Command: `harness-<v>.exe <m> yield x`; log in `logs/yield-*.log`.

- A coroutine runs `for i=1,1.5e9 do local m=i%7; if m==3 then h=(h*31+i)%1000000007 else s=s+m end end`.
- The interrupter requests every 2 ms. The hook calls `lua_yield(L,0)` and the host resumes immediately.

| build/method | reference time | interrupted time | hook yields | checksum | traces after reference → after interrupted run | after hooks cleared |
|---|---|---|---|---|---|---|
| stock + async sethook | 2388 ms | 2425 ms | **1** of 360 requests | OK | 2→2 | new trace compiled |
| CHECKHOOK + async sethook | 2337 ms | 2367 ms | 373 | OK (s=3857142855, h=858555405) | **2→6**: 3 junk "fallback to interpreter" stubs and 12 aborted side-trace recordings, all started from check exits (`logs/jitv-checkhook.txt`) | new trace compiled, 3e8-iteration loop in 137 ms |
| patched + luaJIT_interrupt | 2203 ms | 2326 ms | 417 | OK | 2→3: one legitimate side trace from the prelude branch (`logs/jitv-patched.txt`) | new trace compiled, 132 ms |
| patched (assert build) | 2393 ms | 2698 ms | 373 | OK | 2→3 | new trace compiled |

### Where a hook-yield goes in a nested coroutine

Command: `harness-<v>.exe <m> nested x`; log in `logs/nested.log`.

```
host lua_resume -> 0 (finished); outer saw inner.resume -> true #extra=0, inner status=suspended
```

The yield is delivered to the innermost Lua `coroutine.resume`, which sees it as a plain `coroutine.yield()`. It does not reach the host.

### Hard abort against code that catches the error

Command: `harness-patched-assert.exe intr hardabort <case> 300 [error|yield]`; log in `logs/hardabort.log`. On the first request, the hook re-arms itself with count 1 and either raises an error or yields.

| case | error-based abort | yield-based abort (state then abandoned) |
|---|---|---|
| `while true do pcall(function() while true do end end) end` | OK, 12 µs | OK, 12.5 µs |
| `while true do coroutine.resume(coroutine.create(function() while true do end end)) end` | OK, 20 µs | OK, 12 µs |
| `while true do xpcall(function() while true do end end, function(e) while true do end end) end` | **HANG** | OK, 12.7 µs |

Why the `xpcall` case hangs:
- `lua_error` → `lj_err_run` (`src/lj_err.c:879-900`) calls the `xpcall` message handler *before* unwinding.
- At that point `HOOK_ACTIVE` from `callhook` is still set, so the handler runs with all hooks disabled and can loop forever.
- PUC Lua 5.1 behaves the same way (`allowhook = 0` inside hooks).

## 5. Overhead

`bench/benchrun.c` loads one benchmark file, optionally installs a C count hook (the hook does one `InterlockedExchange`, like a real host), optionally turns the JIT off, and times `lua_pcall` with QPC. Benchmarks are in `bench/*.lua`. Results were identical across all configurations. Raw data: `logs/bench.csv` (pass 1, 5 runs) and `logs/bench2.csv` (pass 2, 9 runs). Runs were sequential and round-robin across configurations.

**JIT on, normal sizes** (pass 2, 9 runs each, median; % is relative to stock):

| bench (size) | stock | CHECKHOOK, no hook | patched, no hook | patched + count hook N=1e4 (production config) |
|---|---|---|---|---|
| fib (40) | 673.7 ms (659.9-687.4) | +0.8% | +0.5% | +0.4% |
| mandelbrot (4000) | 645.2 ms (643.0-654.2) | +0.3% | +0.1% | -0.1% |
| nbody (5e6) | 578.6 ms (572.4-587.2) | +1.4% | +1.1% | +1.1% |
| spectralnorm (4000) | 481.2 ms (473.6-488.4) | +1.2% | +1.1% | +1.5% |
| tableops (1.5e6) | 775.2 ms (759.8-815.5) | -0.3% | -0.2% | +0.8% |
| stringops (8e5) | 488.6 ms (457.6-531.9) | -2.7% | -1.2% | -1.7% |
| sha256 (48 MiB) | 548.1 ms (537.4-610.6) | -0.3% | -0.3% | -1.1% |
| methods (2e8) | 577.0 ms (570.6-601.4) | -2.0% | -1.7% | -1.2% |
| **geomean** | | **-0.2%** | **-0.1%** | **-0.05%** |

Pass 1 (5 runs, `logs/bench.csv`) gave +1.0% (CHECKHOOK), +2.3% (patched) and +1.9% (patched + hook) geomean. Run-to-run spread is about ±2-3%, and up to ±10% for the GC-heavy `stringops`.

Conclusion: the trace guard has **no measurable cost** on these benchmarks. The worst consistent per-benchmark signal is about +1-1.5% (nbody, spectralnorm, both tight float loops). This matches Pall's "counts against execution bandwidth" remark: the cost is real but small.

**Interpreter (`jit.off`), smaller sizes** (pass 1, 5 runs). This is the cost of keeping a count hook installed (`DISPMODE_INS`: every bytecode goes through `vm_inshook`):

| bench | interp stock | + count hook 1e4 (stock) | + count hook 1e4 (patched) | + count hook 1e5 (patched) |
|---|---|---|---|---|
| fib (36) | 438.6 ms | +15.3% | +15.8% | +14.5% |
| mandelbrot (1500) | 554.2 ms | +62.8% | +64.8% | +63.1% |
| nbody (1e6) | 593.3 ms | +32.0% | +33.7% | +33.1% |
| spectralnorm (1000) | 622.3 ms | +33.2% | +32.8% | +32.8% |
| tableops (6e5) | 590.3 ms | +7.3% | +10.0% | +6.0% |
| stringops (1e6) | 713.5 ms | +1.2% | +4.4% | +7.2% |
| sha256 (4) | 579.3 ms | +28.0% | +26.4% | +26.7% |
| methods (3e6) | 551.1 ms | +9.6% | +10.5% | +10.8% |
| **geomean** | | **+22.4%** | **+23.6%** | **+23.1%** |

- N=1e4 and N=1e5 cost the same, so the cost is the per-instruction `vm_inshook` dispatch, not the hook calls. N can be small (good interpreter latency) at no extra cost.
- This cost applies only to code that is interpreted: cold code and NYI paths. CC:T's event loop and coroutine plumbing are such paths, since `coroutine.*` is not compiled.
- Even with the hook, the LuaJIT interpreter remains far faster than Cobalt.

**The trap of CHECKHOOK with a count hook that stays installed** (`logs/checkhook-permhook.log`, 3 runs each, JIT sizes):

| bench | stock + hook 1e4 | patched + hook 1e4 | **CHECKHOOK + hook 1e4** | stock `jit.off`, no hook |
|---|---|---|---|---|
| mandelbrot 4000 | 638 ms | 641 ms | **46 863 ms (73x)** | 3 959 ms |
| nbody 5e6 | 580 ms | 579 ms | **16 090 ms (28x)** | 2 940 ms |
| sha256 48 | 535 ms | 542 ms | **17 793 ms (33x)** | 7 396 ms |
| fib 40 | 680 ms | 693 ms | **24 893 ms (37x)** | 3 148 ms |

- With CHECKHOOK, any count hook that is installed makes every trace exit at every check. The VM then runs **2.4-12x slower than the plain interpreter**.
- This is why the patch tests a dedicated `intr` byte. Host hooks that stay installed, as CraftOS-PC does or as the §7 protocol does, cost nothing in JIT code with the patch.

## 6. No-patch alternatives (with numbers)

| alternative | how it was measured | result | verdict |
|---|---|---|---|
| **`jit.off()` globally** (interpreter only; stock hooks then work) | `logs/bench2.csv` (`intsize_jit_on`/`intsize_jit_off`, same sizes, 3 runs) and `logs/checkhook-permhook.log` | Slowdown vs JIT: fib 4.5x, mandelbrot 6.2x, nbody 5.2x, spectralnorm 19.9x, tableops 2.0x, stringops 1.2x, sha256 11.8x, methods 59.6x; **geomean 6.8x** | Removes the reason for the mod (the interpreter is still faster than Cobalt). Only useful as a fallback mode. |
| **Disable only loop compilation** (`jit.opt`) | Source review: one `hotloop` parameter drives both loop and call hotcounts (`HOTCOUNT_LOOP=2`, `HOTCOUNT_CALL=1`). `-O-loop` only disables the loop optimization, and traces still link to themselves. | No parameter exists that keeps function traces but drops loops. Function traces still form endless cycles through tail-recursion and up-recursion links, and side traces link back to roots (the tail_recursion case hangs on stock). | **Not a solution.** |
| **Source-level instrumentation**: insert `__b=__b-1 if __b<0 then __b=__chk() end` at every `do`/`repeat`/function entry (`bench/instrument.lua`, `bench/*_i.lua`) | pass 2, 5 runs | fib +163%, mandelbrot -0.1%, nbody -7.8% (code-layout noise), spectralnorm +32.8%, sha256 +2.4%, methods +24.9%; **geomean +26.6%** | Works without patching the VM, but costs up to 2.6x. It also needs a correct Lua parser and rewriter (goto, `repeat ... until` scoping, `load()` of user strings, `string.dump` bytecode must be refused) and can be bypassed if the rewriter has any gap. Inferior to the 70-line patch. |
| **One child process per computer**, killed or `SIGSTOP`ped on timeout | `bench/ipc_pingpong.c`: 200k request/response round trips over anonymous pipes. `tasklist`: idle `luajit.exe` working set. | **7.3 µs per round trip.** About 4.6-5.2 MB working set per idle LuaJIT process. | The only approach that also stops C-level hangs (string.find backtracking) and memory bombs, and the LuaJIT FAQ recommends process-level sandboxing. But every CC:T API call (`term.write`, `redstone`, `fs`, `peripheral.call`, events) becomes IPC at about 7 µs or more plus serialization, a server needs hundreds of processes, and process management is a big redesign. Not recommended as the primary mechanism; could be an optional "paranoid" mode later. |
| **Upstream CHECKHOOK flag + async `lua_sethook`** (no source patch) | harness and benchmarks above | 25/25 stopped; about 0-1% cost when no hook is installed | Works, but it is a thread-unsafe API (LuaJIT #824: `HOOK_ACTIVE` clobbering leaves the computer permanently uninterruptible or crashes it; the `J->state` stale write in `lj_trace_abort` can corrupt the trace state). It also cannot coexist with any hook that stays installed (25-73x slowdown, above) and leaves junk side traces. **Acceptable only as a stop-gap.** |
| **Reuse the sampling profiler** (`IR_PROF`/`HOOK_PROFILE`) | Source review | Profiler state is one per process (`lj_profile.c:90,328`); no guard in single-line loops; the callback cannot yield | Not usable. |

## 7. Recommendation

### Recommended path for the 1.21.1 branch

1. **Vendor `luajit-preempt.patch`** on top of LuaJIT 19878ec0, or a newer v2.1; the patch touches stable code. Either fork the LuaJIT submodule or apply the patch in the native build. Build with `XCFLAGS=-DLUAJIT_ENABLE_INTERRUPT`, and with `-DLUA_USE_ASSERT -DLUA_USE_APICHECK` for test builds. Optionally submit it upstream as an alternative to CHECKHOOK.
2. **Never call `lua_sethook` from a thread other than the VM thread.** Use `luaJIT_interrupt` only, and guard it with a mutex against `lua_close`.
3. **Remove or neutralise these in the sandbox:**
   - `debug.sethook`: it would replace the host hook, and a Lua hook function runs uninterruptibly. CC:T's ROM does not use it, but Cobalt exposes it (`CobaltLuaMachine.java:79` → `CoreLibraries.debugGlobals`).
   - `newproxy`: `__gc` finalizers run uninterruptibly.
   - `jit.*` and `ffi`.
   - Native `xpcall`: replace it with a wrapper that calls the handler only *after* unwinding, for example `pcall`, then `handler(err)`, or make hard aborts yield-based (see below). The ROM's `cc.internal.exception` only needs `xpcall` as a stack barrier identity (`exception.lua:105`), so check that a wrapper keeps that behaviour.
4. **Map CC:T's `TimeoutState` to the hook as follows:**
   - **Soft abort** ("Too long without yielding"): `luaL_error` from the hook. This is catchable, as in Cobalt.
   - **Hard abort:** `lua_sethook(L, hard, MASKCOUNT, 1)` (VM thread). Then `lua_yield` if `lua_isyieldable(L)`, otherwise `luaL_error`. The host's `lua_resume` returns, and the host reports TIMEOUT and `lua_close`s the state. Yielding cannot be caught by `pcall`, so this matches Cobalt's uncatchable `HardAbortError`.
   - **Pause:** only possible when the hook runs on the host's main coroutine. User code always runs inside `parallel`/`multishell` coroutines, so either:
     - (a) do not support pausing (pre-1.87 CC behaviour: the computer runs until it yields or is soft-aborted), or
     - (b) yield a unique light-userdata token from the hook, and wrap `coroutine.resume`/`coroutine.wrap` in the sandbox so that they re-yield the token upward and resume downward. The top-level resume returns PAUSE.

     I did not prototype (b); it is cct-contract territory.
5. **Accept or mitigate C-level hangs.** Pattern backtracking (`string.find`/`match`/`gmatch`/`gsub` with many `*`) and huge `string.rep` cannot be preempted. Options: limit pattern complexity or input sizes in wrappers, or (worst case) abandon the stuck thread as CC:T already does for hard-abort failures.

### Host protocol sketch (C, JNI side)

```c
/* VM thread, once per computer after luaL_newstate(): */
lua_sethook(L, cclj_hook, LUA_MASKCOUNT, 10000);   /* stays installed; ~1 µs interp latency */

/* Monitor thread (CC:T TimeoutState listener), any time: */
void cclj_request(Machine *m, int what) {          /* what = PAUSE|SOFT|HARD bitmask */
  mtx_lock(&m->close_lock);
  if (m->L) { atomic_fetch_or(&m->req, what); luaJIT_interrupt(m->L); }
  mtx_unlock(&m->close_lock);
}

/* VM thread: */
static void cclj_hard(lua_State *L, lua_Debug *ar) {
  if (lua_isyieldable(L)) lua_yield(L, 0);        /* propagates up coroutine by coroutine */
  luaL_error(L, "Too long without yielding");
}
static void cclj_hook(lua_State *L, lua_Debug *ar) {
  Machine *m = machine_of(L);
  int r = atomic_exchange(&m->req, 0);
  if (!r) return;                                  /* periodic call: ~free */
  if (r & HARD) { lua_sethook(L, cclj_hard, LUA_MASKCOUNT, 1); cclj_hard(L, ar); }
  if (r & SOFT) luaL_error(L, "Too long without yielding");
  if ((r & PAUSE) && L == m->main_co && lua_isyieldable(L)) lua_yield(L, 0);
  /* else: cannot pause here; ignore (or re-request later) */
}
```

### Effort for this lane's work in the 1.21.1 port
- Patch integration and build plumbing for Windows, Linux and macOS (x64 and arm64): about 0.5-1 day for an experienced dev, 2-3 days for a newcomer. The patch is done; the work is in cross-building.
- Host interrupt, soft and hard abort, and sandbox removals (`debug.sethook`, `newproxy`, `xpcall` wrapper): about 1-2 days for an experienced dev, 3-5 days for a newcomer.
- Optional pause support through a resume-wrapper token: about 2-3 days for an experienced dev, 1 week or more for a newcomer, plus ROM compatibility testing.
- arm64 verification of the patch (Apple Silicon or Linux arm64 CI): about 0.5 day.

## 8. Files produced

- `luajit-preempt.patch`: git diff against the vendored commit 19878ec0. It applies with `git apply` (verified on a fresh clone of the vendored tree) and is included inline below.
- `base/` is a git repo: `master` is the vendored copy and `preempt` is the patched tree.
- Builds: `b-stock/`, `b-checkhook/`, `b-patched/`, `b-patched-assert/` (each with `src/luajit.exe` and `src/libluajit.a`).
- `harness/harness.c`, `harness/compat_intr.h`, `harness/run_all.sh`, and `harness/harness-{stock,checkhook,patched,patched-assert}.exe`.
- `bench/{fib,mandelbrot,nbody,spectralnorm,tableops,stringops,sha256,methods}.lua`, `bench/*_i.lua`, `bench/instrument.lua`, `bench/benchrun.c`, `bench/run_bench.sh`, `bench/run_bench2.sh`, `bench/ipc_pingpong.c`.
- `tools/apply_patch.py` (generated the patch hunks, CRLF-aware), `tools/fix_comments.py`, `tools/summarize.py`.
- `logs/`: `cases-{stock-sethook,checkhook-sethook,patched-intr,patched-assert-intr}.log`, `yield-*.log`, `nested.log`, `hardabort.log`, `jitv-{checkhook,patched}.txt`, `bench.csv`, `bench2.csv`, `checkhook-permhook.log`, `ipc.log`, build logs `../build-*.log`.
- `craftos2-luajit-Computer.cpp`: a reference copy of CraftOS-PC Accelerated's hook setup, read as data.

Commands to reproduce, run from the work dir:

```bash
(cd b-patched/src && mingw32-make -j24 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_INTERRUPT)
gcc -O2 -DHAVE_LUAJIT_INTERRUPT -Ib-patched/src harness/harness.c b-patched/src/libluajit.a -lwinmm -o harness/harness-patched.exe
harness/run_all.sh patched intr            # all cases
harness/harness-patched.exe intr yield x   # yield/resume/JIT-after test
harness/harness-patched.exe intr nested x  # nested coroutine yield semantics
harness/harness-patched.exe intr hardabort xpcall_handler_loop 300 yield
RUNS=9 bench/run_bench2.sh > logs/bench2.csv && python -I tools/summarize.py logs/bench2.csv jit_stock,jit_checkhook,jit_patched,jit_patched_hook1e4
```

## 9. The patch (inline)

```diff
diff --git a/src/Makefile b/src/Makefile
index 99642be..8b59b1a 100644
--- a/src/Makefile
+++ b/src/Makefile
@@ -100,6 +100,10 @@ XCFLAGS=
 # Note: this does not provide full compatibility with Lua 5.2 at this time.
 #XCFLAGS+= -DLUAJIT_ENABLE_LUA52COMPAT
 #
+# Allow interrupting JIT-compiled code asynchronously via luaJIT_interrupt().
+# Adds a check of a global flag at every trace entry and loop iteration.
+#XCFLAGS+= -DLUAJIT_ENABLE_INTERRUPT
+#
 # Disable the JIT compiler, i.e. turn LuaJIT into a pure interpreter.
 #XCFLAGS+= -DLUAJIT_DISABLE_JIT
 #
diff --git a/src/lj_dispatch.c b/src/lj_dispatch.c
index b9748bb..ccc8caf 100644
--- a/src/lj_dispatch.c
+++ b/src/lj_dispatch.c
@@ -339,6 +339,28 @@ LUA_API int lua_sethook(lua_State *L, lua_Hook func, int mask, int count)
   return 1;
 }
 
+#ifdef LUAJIT_ENABLE_INTERRUPT
+/* Request an interrupt. Async-safe: may be called from any thread or from a
+** signal handler. It only performs two plain stores and never touches the
+** dispatch table, the trace recorder state or the hook mask (unlike
+** lua_sethook(), which is unsafe to call from another thread).
+**
+** Compiled code polls G(L)->intr at every trace entry, i.e. at every loop
+** iteration and at every trace link, and exits to the interpreter, where
+** lj_trace_exit() makes the count hook due. The next count hook invocation
+** clears the request. The host must keep a count hook installed (set from
+** the VM thread). Setting hookcount here is only a hint: the interpreter's
+** non-atomic decrement may overwrite it, so in pure interpreter loops the
+** hook runs once the regular count runs out.
+*/
+LUA_API void luaJIT_interrupt(lua_State *L)
+{
+  global_State *g = G(L);
+  g->hookcount = 1;  /* Best effort, races with the interpreter are benign. */
+  g->intr = 1;
+}
+#endif
+
 LUA_API lua_Hook lua_gethook(lua_State *L)
 {
   return G(L)->hookf;
@@ -362,6 +384,10 @@ static void callhook(lua_State *L, int event, BCLine line)
   if (hookf && !hook_active(g)) {
     lua_Debug ar;
     lj_trace_abort(g);  /* Abort recording on any hook call. */
+#ifdef LUAJIT_ENABLE_INTERRUPT
+    if (event == LUA_HOOKCOUNT)
+      g->intr = 0;  /* The count hook services any pending interrupt. */
+#endif
     ar.event = event;
     ar.currentline = line;
     /* Top frame, nextframe = NULL. */
diff --git a/src/lj_obj.h b/src/lj_obj.h
index d8b6964..3285cee 100644
--- a/src/lj_obj.h
+++ b/src/lj_obj.h
@@ -640,6 +640,7 @@ typedef struct global_State {
   uint8_t hookmask;	/* Hook mask. */
   uint8_t dispatchmode;	/* Dispatch mode. */
   uint8_t vmevmask;	/* VM event mask. */
+  volatile uint8_t intr;	/* Interrupt request, see luaJIT_interrupt(). */
   StrInternState str;	/* String interning. */
   volatile int32_t vmstate;  /* VM state or current JIT code trace number. */
   GCRef mainthref;	/* Link to main thread. */
diff --git a/src/lj_record.c b/src/lj_record.c
index 3f25000..7a9c2de 100644
--- a/src/lj_record.c
+++ b/src/lj_record.c
@@ -669,6 +669,9 @@ static LoopEvent rec_itern(jit_State *J, BCReg ra, BCReg rb)
     IRRef ref = REF_FIRST + LJ_HASPROFILE;
 #ifdef LUAJIT_ENABLE_CHECKHOOK
     ref += 3;
+#endif
+#ifdef LUAJIT_ENABLE_INTERRUPT
+    ref += 3;
 #endif
     if (J->cur.nins > ref ||
        (LJ_HASPROFILE && J->cur.nins == ref && J->cur.ir[ref-1].o != IR_PROF)) {
@@ -2898,6 +2901,24 @@ void lj_record_setup(jit_State *J)
     emitir(IRTGI(IR_EQ), tr, lj_ir_kint(J, 0));
   }
 #endif
+#ifdef LUAJIT_ENABLE_INTERRUPT
+  /* Poll for asynchronous interrupt requests (luaJIT_interrupt()) at the
+  ** start of every trace. Every cycle through compiled code passes a trace
+  ** start: a loop iteration (the load is volatile, so the loop optimization
+  ** copies the check into the loop body) or a link to the start of a trace
+  ** (root, loop, tail-, up- and down-recursion links). Side traces get the
+  ** check, too, and it's executed before any side effect of the trace.
+  ** On x64 this compiles to 'test byte [DISPATCH+ofs], 0xff; jnz ->exit'.
+  ** The exit resumes the interpreter at a bytecode boundary (snapshot #0 or
+  ** the loop snapshot), where the count hook runs.
+  */
+  if (J->state != LJ_TRACE_END) {  /* Not for side traces that bail out. */
+    TRef tr = emitir(IRT(IR_XLOAD, IRT_U8),
+		     lj_ir_kptr(J, (void *)&J2G(J)->intr), IRXLOAD_VOLATILE);
+    tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, 0xff));
+    emitir(IRTGI(IR_EQ), tr, lj_ir_kint(J, 0));
+  }
+#endif
 }
 
 #undef IR
diff --git a/src/lj_trace.c b/src/lj_trace.c
index a5e316e..076a67e 100644
--- a/src/lj_trace.c
+++ b/src/lj_trace.c
@@ -928,9 +928,17 @@ int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
   } else if (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize) {
     if (!(G(L)->hookmask & HOOK_GC))
       lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
+#ifdef LUAJIT_ENABLE_INTERRUPT
+  } else if (G(L)->intr) {
+    /* Interrupt pending: just exit to interpreter, no hot side exit. */
+#endif
   } else if ((J->flags & JIT_F_ON)) {
     trace_hotside(J, pc);
   }
+#ifdef LUAJIT_ENABLE_INTERRUPT
+  if (G(L)->intr && (G(L)->hookmask & LUA_MASKCOUNT))
+    G(L)->hookcount = 1;  /* Run the count hook at the next instruction. */
+#endif
   /* Return MULTRES or 0 or -17. */
   ERRNO_RESTORE
   switch (bc_op(*pc)) {
diff --git a/src/luajit_rolling.h b/src/luajit_rolling.h
index 2d04402..e4662eb 100644
--- a/src/luajit_rolling.h
+++ b/src/luajit_rolling.h
@@ -73,6 +73,16 @@ LUA_API void luaJIT_profile_stop(lua_State *L);
 LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
 					     int depth, size_t *len);
 
+/* Asynchronous interrupt request. Can be called from any thread or from a
+** signal handler. Only available with -DLUAJIT_ENABLE_INTERRUPT builds.
+** JIT-compiled code exits to the interpreter at its next loop iteration or
+** trace link and the count hook runs at the next interpreted instruction.
+** Requires a count hook (lua_sethook with LUA_MASKCOUNT) installed from the
+** VM thread. In pure interpreter loops the request may only be seen once the
+** hook count runs out, so pick the count accordingly (e.g. 1000-100000).
+*/
+LUA_API void luaJIT_interrupt(lua_State *L);
+
 /* Enforce (dynamic) linker error for version mismatches. Call from main. */
 LUA_API void LUAJIT_VERSION_SYM(void);
 
```

## 10. Open questions / not verified
- **arm64 (Apple Silicon, Linux aarch64) and 32-bit targets were not built or run.** The IR is the same shape as upstream CHECKHOOK, so codegen should be fine, but this is unverified.
- **No down-recursion (`LJ_TRLINK_DOWNREC`) trace could be produced.** It is covered by construction, since it is a self-link to the trace start.
- **The resume-wrapper token scheme for CC:T pause was not prototyped.**
- **The memory-ordering argument for ARM is reasoned, not tested.** The argument: `luaJIT_interrupt` uses plain stores, and a delayed or lost `hookcount` hint only adds latency of up to N instructions.
- **I did not check whether Cobalt interrupts pathological pattern matching.** If it does not, the C-level-hang limitation is shared with stock CC:T.
