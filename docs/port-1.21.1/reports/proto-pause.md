# Prototype: pause token protocol (P10)

*Written on 2026-11-07 by an independent review/prototype agent. Section references (§, P-numbers, WP letters) point to PLAN.md **v1**; PLAN.md v2 has incorporated or answered every item (see its change log). Paths under `scratchpad/work/...` refer to the session scratch workspace; the reusable files were copied into `../artifacts/`.*

## Summary

The pause token protocol works, and all 7 required tests pass on Windows x64 (JIT on, JIT off, and an assert/API-check build of LuaJIT) and on Linux x64 under WSL2. The prototype is cclj_co.c (about 450 lines of C plus a 12-line Lua continuation) with a CC:T-like host, pausehost.c. The host runs Lua on a thread with a 1 MB stack, a second thread requests pauses every 0.3-1.5 ms, and events are delivered the way CobaltLuaMachine.handleEvent does it (string filter from the top-level yield; resume with no values after PAUSE).

- **Correctness:** paused runs produce byte-identical results, host-level yields and event counts compared with a run without pauses. The ROM parallel test got through 2,719 pauses (61,575 at a 20-60 µs pause rate). The 7-level nesting test paused at every level. The naive hook-yield control fails the same test within 8 ms, so the test does detect the known breakage.
- **C-call boundaries:** pauses inside table.sort comparators, gsub callbacks, xpcall handlers, string.format("%s")→__tostring, load readers, and coroutines resumed from those are deferred. "attempt to yield across C-call boundary" never reached user code. Pauses do happen inside pcall/xpcall bodies and inside metamethods the VM calls directly.
- **Aborts:** hard-abort latency over 133 runs was at most 42 µs (median 2.5-3.9 µs), including pcall-swallowing loops at 6 levels and a looping xpcall handler. Soft abort stays catchable, with a "chunk:line:" prefix.
- **Nesting limit:** deep recursion stops with a clean Lua error. Without a limit, a 1 MB thread dies with a stack overflow at about 12,600 native or 4,500 wrapper levels.
- **Cost:** about 13-15 ns per resume with the JIT on (1.33-1.72x on pure ping-pong microbenchmarks). Each pause costs about 1-2.5 µs.

Two design changes came out of testing. First, my first deferral design re-armed the interrupt flag from the hook; that made JIT code inside sort/gsub callbacks 4-7x slower, so it was replaced by a re-check ticker every 250 µs (overhead about 0%). Second, on Linux each gsub-callback nesting level costs about 9.2 KB of C stack (glibc's 8 KB luaL_Buffer), so a stack-pointer floor check is required in addition to the count limit.

Remaining limitation (fairness only, not correctness): code that spends almost all its time inside non-yieldable callbacks with tiny gaps between them can delay a pause by up to about 0.5 s in my adversarial tests. Optional checkpoint wrappers around xpcall and load(reader) cut this to the length of one callback (533 ms → 6 ms and 282 ms → 17 ms).

**Works:** True

## Test results

- T1 ROM parallel.waitForAny/waitForAll (3 CPU routines + 1 event waiter + rednet-like routine) -> PASS. Windows JIT on: 2,719 pauses (5,809 propagation steps); host-level yields 4501 = reference; events 4501 = reference; checksums, waiter event log and scheduler resume count identical; 2,440 ms paused vs 2,599 ms reference. At 20-60 µs pause rate: 61,575 pauses, PASS. Assert build: 2,667 pauses, PASS. JIT off: 2,042 pauses, PASS. Linux: PASS. Naive control (plain hook-yield, native coroutine library): FAIL after 8 ms ('generator protocol broken'), as expected.
- T2 deep nesting, 7 levels (alternating / all resume / all wrap) -> PASS x3 on every variant. Pauses at every level, e.g. 32/70/121/114/189/201/167 (levels 1..7); at 20-60 µs: 915..4,667 per level. Results identical to the reference.
- T3 semantics (yield_values, status_running, errors, toplevel_yields, pcall_yield, pingpong_under_pauses) -> PASS x6 on Windows, assert build, JIT off, fast pauses and Linux. Covered: exact vararg counts for 0/nil/20-value yields; status 'running'/'normal'/'suspended'/'dead'; running() identity; string error positions; table error identity; traceback of a dead coroutine; wrap re-raises errors unchanged; 'cannot resume dead/running/normal coroutine'; top-level zero-value yields kept apart from pauses with filters preserved; 1.2 M resumes under 20-60 µs pauses with an exact sum.
- T4 C boundaries (17 workloads) -> PASS x17 everywhere. Deferred, with 0 pauses inside the region and no C-call-boundary error leaking: sort comparator, sort with __lt, gsub callback, xpcall handler, string.format %s->__tostring, load reader, wrap generator inside gsub, gsub->sort nesting. Pausable inside (hundreds of pauses each): pcall body, xpcall body, __index, __lt, __add/__concat/__eq/__call/__len, tostring()->__tostring, __pairs, table.foreach, gmatch loop. Deferral overhead (final design): sort 513->515 ms, gsub 684->685 ms (first design: 509->1955 and 679->4167 ms). Starvation worst case: up to about 0.56 s max deferral, 0 pauses for load_reader on Linux; with CHECKPOINT=1: 6 ms (xpcall) and 17 ms (load).
- T5 aborts (9 cases: hard with pcall loops, xpcall+sort looping handler, gsub loop, abort landing in manager code, while paused, while deferring; soft caught, soft uncaught, soft then hard) -> PASS on all runs: 12 Windows runs (incl. assert build and JIT off) and 5 Linux runs. Hard-abort latency over 91 Windows samples: min 0.0, median 2.5, max 41.6 µs. Linux (42 samples): median 3.9, max 38.5 µs. Soft abort message matches '^.+:%d+: Too long without yielding$'.
- T6 nesting limit on a 1 MB VM thread -> PASS x7. Count limit 200: 'coroutine nesting too deep (limit 200)' for wrap and resume recursion. Stack used per level: 224 B wrapper on Windows (native resume 80 B), 192 B on Linux; with a gsub callback per level 1,584 B (Windows) / 9,200 B (Linux). With the count limit disabled, the stack-floor check (256 KB reserve) stops nesting at 489 gsub levels / 3,457 plain levels on Windows and 84 gsub levels on Linux ('C stack overflow'). Pauses propagate through 199 levels (about 196 steps per pause); an error raised at the deepest allowed level unwinds cleanly. Without any limit: native nesting crashes at about 12,600 levels (0xC00000FD on Windows, SIGSEGV at 13,000 on Linux); wrapper at about 4,500 (Windows) / 5,400 (Linux).
- T7 performance -> PASS. JIT on, Windows, ns per operation native vs wrapper: ping-pong 23.4/37.8 (1.61x), of which the C part alone is 30.4 (1.24x); wrap generator 24.3/36.7 (1.51x); 3-level pass-through 69.4/105.4 (1.52x); 4-coroutine scheduler 126.8/169.2 (1.33x). Linux: 21.1/36.3 (1.72x) ... 119.2/169.8 (1.42x). JIT off: 1.69-2.77x. Pause cost: 53,549 pauses added 69 ms (about 1.3 µs per pause, 6 levels); CPU-bound chain at 1 ms pauses 456 vs 457 ms; interpreter-heavy chain at 20-40 µs pauses about 2.4 µs per pause.

## Design notes

PAUSE TOKEN PROTOCOL: final design, prototyped and tested in cclj_co.c.

1. Machine state.
   - Shared with other threads (on their own cache line, separated by padding from the VM-thread fields): `req` bits PAUSE|SOFT|HARD (atomic), `running` (the VM thread is inside lua_resume(top)), and the close lock.
   - VM thread only: `pausing`, `aborting`, `nonyield` (number of active nested resumes whose caller cannot yield), `depth`, `max_depth` (200), `stack_floor`.
   - The machine pointer is found from L through the allocator ud (lua_getallocf). The memory-cap allocator wrapper (P9) provides that ud anyway.

2. Sandbox coroutine library.
   - `create`, `yield`, `status`, `running` and `isyieldable` stay native. Their semantics are unchanged because the real child runs through lua_resume while its parent is blocked in C, so the parent's status is genuinely "normal".
   - `resume` and `wrap` are replaced by a C core `raw_resume(co, ...)` / `raw_wresume` (upvalue = machine) plus this Lua continuation, which needs no tables and only tail calls:
     ```lua
     local function cont(co, ok, ...) if ok == nil then return cont(co, raw_resume(co)) end return ok, ... end
     function resume(co, ...) return cont(co, raw_resume(co, ...)) end
     local function wcont(co, ok, ...) if ok == nil then return wcont(co, raw_wresume(co)) end return ... end
     function wrap(f) <check f is a function>; local co = create(f); return function(...) return wcont(co, raw_wresume(co, ...)) end end
     ```
   - A real resume always returns at least true/false. Zero values therefore mean "I was resumed by the host after a pause; continue my child".

3. raw_resume, step by step:
   1. Check argument 1 is a thread.
   2. Check status exactly like LuaJIT's ffh_resume (`co->cframe`, `co->status`, `top == base`) and also refuse the main thread. Messages use Cobalt wording: "cannot resume running/normal/dead coroutine". wrap raises them with the caller's position (luaL_where level 2).
   3. Nesting check: `depth >= 200` gives "coroutine nesting too deep (limit 200)"; `&local < stack_floor` gives "C stack overflow". resume returns them as `false, msg`; wrap raises them.
   4. Grow the child's stack with lua_checkstack and xmove the arguments.
   5. `ny = !lua_isyieldable(L)`; `nonyield += ny`; `depth++`; `status = lua_resume(co, n)`; then undo both counters.
   6. If `aborting`: yield (if ny == 0) or raise an error, without running any Lua.
   7. If `status == LUA_YIELD && pausing`: settop(co, 0); if ny == 0, `return lua_yield(L, 0)` (propagate upward). The cannot-propagate fallback (cancel, re-request, re-resume) is unreachable and was never hit (nfailprop = 0 in every run).
   8. Otherwise return `true` + results, or `false, err`. wrap does lua_error(err) with the value unchanged. That is Cobalt semantics; native LuaJIT prefixes a position to string errors.

4. Count hook (installed once, count 10^4).
   - HARD: set `aborting`; lua_sethook(count 1) from the VM thread; yield if yieldable, else raise an error.
   - SOFT: clear the bit; raise a catchable error luaL_where(L, 0) .. "Too long without yielding".
   - PAUSE, case 1: if `nonyield == 0 && lua_isyieldable(L)`, clear the bit, set `pausing = 1`, lua_yield(L, 0). This is a hook-yield, which is transparent on resume and discards resume values.
   - PAUSE, case 2: otherwise defer. Leave the bit pending and do NOT re-arm `intr`; re-arming made JIT code in non-yieldable regions 4-7x slower. Interpreted code meets the hook again within 10^4 instructions.

5. Deferral re-check.
   - A per-process ticker calls `cclj_poke(m)` every about 250 µs for machines that are running with PAUSE pending: a racy pre-check, then lock, then luaJIT_interrupt. This bounds the re-check rate for JIT code inside non-yieldable callbacks.
   - The ticker and the TimeoutState listener must not write shared memory in a tight loop. A spinning lock on another core made the VM 3-5x slower on Linux.
   - Optional (recommended): sandbox `xpcall` and `load` with a reader function become `after(native(...))`, where `after` calls a C `checkpoint()`. checkpoint performs a pending pause via a C-function yield of zero values; on resume it returns nothing.

6. Host (handleEvent).
   - Before each lua_resume(top): clear the PAUSE bit (= TimeoutState.reset); `pausing = 0`; `running = 1`.
   - After it returns:
     - `aborting` → TIMEOUT, then close.
     - YIELD + `pausing` → PAUSE. Keep the old filter. The next handleEvent(null, null) resumes top with zero values.
     - YIELD → OK; the filter is the first value if it is a string.
     - 0 (bios returned) → GENERIC_ERROR.
     - error → error(msg).
   - HARD pending while paused → TIMEOUT without resuming.
   - `stack_floor` = current thread's stack low bound + 256 KB, recomputed on every handleEvent, because CC:T may move a computer between worker threads (GetCurrentThreadStackLimits / pthread_getattr_np).
   - Defensive: CC:T never delivers an event while paused (wasPaused path). If it ever did, the host must first continue the chain with zero values.

7. Nesting limit justification (1 MB worker stacks).
   - Measured C stack per nesting level: wrapper 224 B (Windows) / 192 B (Linux); native fast-function resume 80 B.
   - Worst measured mixed level (a gsub callback per level): 1,584 B on Windows; 9,200 B on Linux (glibc BUFSIZ = 8192 → the luaL_Buffer inside the gsub frame).
   - Count limit 200 (= PUC LUAI_MAXCCALLS) costs about 45 KB for plain nesting and is far above real CC:T depth (about 6-10).
   - The SP floor is mandatory, not a backstop: on Linux it, not the count limit, stopped gsub-mixed nesting (at 84 levels). Patch 08 should apply the same floor in lua_call paths. Pure gsub recursion without coroutines would overflow a 1 MB stack after about 100 levels on Linux. The ROM never nests anywhere near this deep.

SEMANTICS AND LIMITATIONS
- **Non-yieldable contexts where pauses are deferred:** C table.sort comparator and __lt via sort, string.gsub callback, xpcall handler, string.format %s → __tostring, load reader, and any coroutine resumed from inside one of these.
  - This is correct but can starve pauses when nearly all time is spent inside such callbacks: up to about 0.5 s measured. Fairness only; soft abort still applies at 7 s.
  - Fixes: pure-Lua sort and gsub (already planned) and the checkpoint wrappers.
- **Pausable in LuaJIT:** pcall and xpcall bodies, all VM-called metamethods, tostring()→__tostring, __pairs, table.foreach, gmatch loops.
- **isyieldable():** returns false inside C callbacks (native LuaJIT behaviour; Cobalt returns true).
- **Wrapper frames:** the `resume`/`cont` Lua frames appear in the parent coroutine's stack. The curated debug library must refuse getupvalue/setupvalue on them (otherwise raw_resume becomes reachable; that would only break pause transparency for the caller, not safety).
- **debug.sethook:** must stay removed. User hooks would replace the host hook.
- **Not tested:** arm64 / macOS; JNI integration; the CC:T test-rom under the full machine.

## Files (scratch workspace)

- `work/proto-pause/cclj_co.c`
- `work/proto-pause/cclj_co.h`
- `work/proto-pause/plat.h`
- `work/proto-pause/pausehost.c`
- `work/proto-pause/build.sh`
- `work/proto-pause/run_tests.sh`
- `work/proto-pause/tests/lib/cc.lua`
- `work/proto-pause/tests/t1_parallel.lua`
- `work/proto-pause/tests/t2_nesting.lua`
- `work/proto-pause/tests/t3_semantics.lua`
- `work/proto-pause/tests/t4_cboundary.lua`
- `work/proto-pause/tests/t5_abort.lua`
- `work/proto-pause/tests/t6_depth.lua`
- `work/proto-pause/tests/t7_bench.lua`
- `work/proto-pause/microbench/resume_rt.c`
- `work/proto-pause/linux/build-linux.sh`
- `work/proto-pause/linux/run-linux.sh`
- `work/proto-pause/linux/run-linux.out`
- `work/proto-pause/logs/run_tests.out`
- `work/proto-pause/logs`
- `work/proto-pause/linux/logs`
- `work/proto-pause/pausehost.exe`
- `work/proto-pause/pausehost-assert.exe`
- `work/proto-pause/luajit-assert`

## Suggested plan updates

TL;DR row "Is it feasible at all?": replace the last sentence with:
"Transparent pause (time-slicing) is now prototyped and tested: a C/Lua coroutine.resume/wrap wrapper plus a hook-yield that propagates up the coroutine chain (Windows and Linux, JIT on and off, thousands of pauses with byte-identical results). Interruptible C pattern matching is the one remaining unprototyped item."

§3 table, P10 row, Status column:
"Prototyped and tested (proto-pause lane). The sandbox replaces coroutine.resume/wrap with a C core plus a 12-line Lua continuation; the hook yields only when the whole resume chain can yield, and each wrapper yields upward until lua_resume(top) returns PAUSE. Measured: ROM parallel scheduler 2,700 pauses (61k at 20-60 µs) with identical results and no lost or spurious events; 7 nesting levels all paused; about 1-2.5 µs per pause; +13-15 ns per resume (1.3-1.7x on coroutine microbenchmarks, about 0% on real workloads). Pauses inside non-yieldable C callbacks (C sort, gsub, xpcall handler, format→__tostring, load reader) are deferred: correct, but they can delay a pause by up to about 0.5 s (fairness only). Pure-Lua sort/gsub and the xpcall/load checkpoint wrappers reduce this to one callback's duration."

§3 table, P8 row, append:
"Measured C stack per coroutine nesting level: wrapper 224 B (Windows) / 192 B (Linux); with a gsub callback per level 1.6 KB (Windows) / 9.2 KB (Linux, glibc's 8 KB luaL_Buffer). The resume wrapper enforces both a count limit of 200 and a stack-pointer floor (refuse when less than 256 KB of the thread's stack remains, recomputed per handleEvent). On Linux the floor, not the count, is what stops gsub-mixed nesting. Without them a 1 MB thread dies at about 12,600 native or 4,500 wrapper levels."

§4.3, replace the "Pause token protocol (designed, needs prototyping)" bullet list with:
"Pause token protocol (prototyped: proto-pause/cclj_co.c):
 1. coroutine.resume/wrap = C raw_resume (status checks as in ffh_resume, refuse the main thread, nesting count/SP-floor check, ny = !lua_isyieldable(caller), nonyield += ny, depth++, lua_resume, undo) plus a Lua continuation that loops when raw_resume returns zero values. create/yield/status/running/isyieldable stay native. wrap re-raises child errors unchanged (Cobalt semantics).
 2. Hook PAUSE: if m->nonyield == 0 && lua_isyieldable(L), clear the bit, set m->pausing, lua_yield(L, 0) (hook-yield). Otherwise defer: leave the bit pending and do NOT re-arm intr (that costs 4-7x in JIT code).
 3. A wrapper whose child returns LUA_YIELD with m->pausing set does settop(child, 0) and return lua_yield(L, 0), up to lua_resume(top) → MachineResult.PAUSE.
 4. handleEvent(null, null): clear the PAUSE bit and pausing, resume top with zero values. Each wrapper's continuation re-resumes its child with zero values, down to the hook-yielded coroutine. The filter is kept across pauses.
 5. A per-process ticker re-pokes (luaJIT_interrupt) running machines with a pending deferred pause every 250 µs. Optional sandbox xpcall/load(reader) wrappers call a C checkpoint() right after the native call returns.
 6. Hard abort: hook re-arms at count 1 and yields or errors; wrappers seeing m->aborting yield or error immediately. Measured latency at most 42 µs. HARD while paused → TIMEOUT without resuming.
 Request side: never write shared memory in a tight loop; keep shared fields on their own cache line."

§4.3 "Curated environment", coroutine entry:
"coroutine (resume/wrap wrapped; create/yield/status/running/isyieldable native); optionally xpcall/load wrapped with a pause checkpoint; the curated debug library must refuse getupvalue/setupvalue on the wrappers."

§4.4 MachineResult mapping, PAUSE entry:
"PAUSE (pause token protocol, §4.3; keep the filter; the next call is handleEvent(null, null))"

§5 table, WP F:
"Pause token protocol (P10): productionise the prototype (cclj_co.c), ticker, checkpoint wrappers, run CC:T coroutine_spec | 1-2 d | 3-5 d". Total row: subtract about 1-2 d experienced and 3-5 d newcomer.

§5 Milestones: move "Pause/time-slicing" from M3 into M2 (low risk, small), or note "can ship with M1 once the host core exists".

§7 Decision 3: "Pause/time-slicing: prototyped and cheap (+13-15 ns per resume). Recommended for v1 (M2), because CC:T's default single worker thread otherwise lets one busy computer stall all others for up to 7 s."

§6 Risks, add:
"Pauses are deferred inside non-yieldable LuaJIT C callbacks (fairness only); coroutine.isyieldable() returns false inside C callbacks (Cobalt: true); wrap error re-raise follows Cobalt (no position prefix)."

## Effort update

WP F (pause token protocol): was 2-4 d experienced / 6-10 d newcomer; now 1-2 d experienced / 3-5 d newcomer. A working, tested C+Lua implementation exists (cclj_co.c), and the remaining work is integration into the host core and JNI machine: PAUSE mapping, a per-process poke ticker, stack-floor computation per handleEvent from the current thread's bounds, checkpoint wrappers in the curated environment, and running CC:T's coroutine_spec. The nesting-limit work moves from WP C/patch 08 into this same wrapper (count 200 + SP floor, already written). Patch 08 is still needed for C recursion without coroutines (gsub/sort/format), and it matters more on Linux (9.2 KB per gsub level).
