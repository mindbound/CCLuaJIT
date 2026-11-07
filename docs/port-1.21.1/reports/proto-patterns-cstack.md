# Prototype: patch 07 (interruptible patterns) and patch 08 (C-stack guard)

*Written on 2026-11-07 by an independent review/prototype agent. Section references (§, P-numbers, WP letters) point to PLAN.md **v1**; PLAN.md v2 has incorporated or answered every item (see its change log). Paths under `scratchpad/work/...` refer to the session scratch workspace; the reusable files were copied into `../artifacts/`.*

## Summary

Patches 07 and 08 are written, tested, and saved as git-diff files. Each file starts with a short description, which git apply ignores. Both apply on top of 01-06.

Patch 07: interruptible C pattern matching. Gated by LUAJIT_ENABLE_INTERRUPT.
- The pattern matcher (every match() step, plus the max_expand and matchbalance scans) and table.sort's partition loops now poll patch 01's G(L)->intr flag.
- When the flag is set, lj_dispatch_intrpoll() runs the host's normal count hook inside a protected C frame (lj_vm_cpcall). There the hook can raise the soft- or hard-abort error, but it cannot yield (lua_isyieldable()==0; lua_yield gives a catchable "attempt to yield across C-call boundary").
- Results:
  - X_string_find_backtrack is interrupted in 1.8 us.
  - All 17 pathological find/match/gmatch/gsub/%b/.*-scan/sort-killer cases are interrupted in about 2-50 us. The 01-06 baseline hangs on all 17.
  - Plain string.find (lj_str_find) is also called from JIT traces, where polling is unsafe. Instead it now switches to the Two-Way algorithm after a work budget, so the worst case is O(n+m). A case that took about 90 s, including on a trace, now takes 10-21 ms.
  - Pattern semantics are unchanged: about 2.18M differential-fuzz results are identical to 01-06, including under interrupt stress, and also with the hook forced at every poll.
  - Overhead: +1.8% geomean on 19 pattern/sort microbenchmarks. The worst case is greedy .* backtracking at +8.7%.

Patch 08: C-stack guard. Gated by LUAJIT_ENABLE_CSTACKGUARD.
- New API luaJIT_setcstacklimit(L, lo, hi) sets a red zone on the C stack. Every point where C code re-enters the VM checks it and raises a catchable "C stack overflow" error inside the zone.
- Checked entry points: lua_call, lua_pcall/lua_cpcall (returned as a status), lua_resume (coroutine stays resumable), metamethod calls from the C API, luaL_callmeta, parser nesting, and the x64 VM fast path of coroutine.resume/wrap.
- Results on 1 MB stacks, across 31 recursion shapes:
  - Windows threads: 0 crashes. The 01-06 baseline crashed in 21 of the 29 shapes it was run on.
  - Linux pthreads: 0 crashes; baseline crashed in 22/31.
  - JDK 21 Java threads with default stacks: the JVM survives; the baseline JVM dies silently.
  - The state stays usable after every case.
- Overhead: about +0.5% on top of 07; coroutine switches change by under 1%.

Final 01-08 build:
- Full preempt harness: no regressions (25/25 loop shapes, yield and hard-abort tests pass). The only hangs left are the two expected ones the sandbox removes: newproxy finalizers and Lua hooks.
- compat_check.lua: same output as 01-06.
- Patch stack: 01-08 applied to pristine vendor/luajit reproduces the development tree exactly and builds. 07+08 also apply and pass smoke tests on the 3c4f9fe2 rebase tree.

**Works:** True

## Test results

- Preempt harness (final 01-08 build, intr error): 26/28 OK. X_string_find_backtrack (was HANG) interrupted in 1.8 us. 25/25 loop shapes OK (0.6-28.5 us). X_finalizer_loop and X_lua_hook_loop still HANG, as expected (the sandbox removes newproxy and debug.sethook) -> PASS
- Preempt yield/nested/hardabort (final): yield 453 hook-yields, checksum matches, JIT still compiles; nested unchanged; hardabort 3 cases x error/yield 6/6 OK -> PASS
- compat_check.lua (final): NUL patterns true/3, tostring(0.1+0.2)=0.30000000000000004, 2^53-1 exact, load(string.dump)=nil, goto as field and as statement, table.pack/unpack, ffi absent, jit on. Identical to 01-06 -> PASS
- pharness (Windows): 17 endless cases (find/match/gmatch/gsub backtracking, 15 captures, .-.-.-.-.-b, gsub/gmatch O(n^2), %b() O(n^2), 200 MB .* scan, frontier, anchored, sort killer 100k, gsub fn repl) in modes error/pcall/hard/hardyield/yieldbad, plus 6 finite cases x pause/gcstress: 97/97 OK. Error-mode latency 6.6-13.2 us in the final run. Baseline 01-06: 17/17 HANG -> PASS
- Hook semantics inside C: lua_isyieldable()==0 in 100% of C-context hook calls. lua_yield in the hook gives the catchable 'attempt to yield across C-call boundary'. Soft abort is catchable by Lua pcall. Hard abort escapes 'while true do pcall(f) end' and yields at the next bytecode boundary in hardyield -> PASS
- Pause deferral (hook re-arms with luaJIT_interrupt while not yieldable): all 6 results match the reference. The yield happens after the C function returns. Slowdown while a pause is pending: 0-38%, bounded by the LJ_INTR_CSKIP=4096 rate limit -> PASS
- gcstress (hook grows the Lua stack by 4000 slots and runs a full GC inside C matching, hundreds of times per case): results identical to the reference -> PASS
- Plain find worst case (string.rep('a',4e6) vs ('a'):rep(2e6)..'b', also as a JIT-compiled call): base about 90 s, hangs in TRACE#2; final 10-21 ms -> PASS
- Differential fuzz: 4 seeds x 20k random patterns x 5 subjects x 5 ops (find, plain find, match, gmatch, gsub with string/table/function replacement, NUL included), about 2.18M result lines. Base 01-06 vs final with 1 ms interrupt stress: IDENTICAL -> PASS
- Re-arm stress fuzz (hook at every poll, LJ_INTR_CSKIP=0, stack growth + periodic GC in the hook): 3 x 3000 patterns IDENTICAL with about 700k in-C hook calls each. Chunk-boundary differential (4218 cases around 4096-char scan boundaries) IDENTICAL, also under re-arm stress (42.6M in-C hook calls) -> PASS
- Two-Way fallback standalone test: 3M random cases 0 mismatches vs naive; 3M with forced mid-search hand-over 0 mismatches; text searches same speed as stock; adversarial 4e6/2e6 in 8 ms -> PASS
- Linux x64 (WSL gcc, final): pattern interrupts from a second pthread 1.7-5.5 us (find, gsub O(n^2), gmatch, pcall-caught, sort), state usable afterwards -> PASS
- Overhead 07 (19 microbenchmarks, 7 rounds x best-of-3 medians, final builds): +1.77% geomean vs 01-06. Worst: greedy_dotstar +8.7%, balance_b +6.9%, gsub_string_repl +4.8%. sort, plain find and coroutines about 0% -> PASS
- Overhead 08: final 01-08 +2.25% geomean vs 01-06 (+0.5% over 07). Coroutine switch +-1% after 64-byte padding: an unpadded 16-byte global_State growth alone cost +5%, reproduced with a build without the guard flag. Sort with comparator (lua_call per compare) +1.8% -> PASS
- C stack, Windows native, 1 MB threads, 128 KB margin (final): 31 recursion shapes give 0 crashes and catchable errors; state usable 31/31; second run identical. Shapes: gsub cb, gsub table __index, sort cmp, sort __lt, format %s, load reader, xpcall handler, coroutine wrap/resume/pre-created chain, host lua_call/pcall/cpcall/gettable/settable/equal/lessthan/concat/callmeta/resume, parser at depth, mixed chain. Baseline 01-06: 21/29 crash (0xC00000FD/0xC0000005) -> PASS
- C stack size/margin sweep: 256 KB stack + 64 KB margin 0/31 crashes (final); 8 MB 0 crashes; 1 MB with 32 KB margin OK, with 16 KB margin crashes (Windows guard region). Minimum safe margin is about 32 KB native -> PASS
- Depth reached (Windows, 1 MB, 128 KB margin): gsub cb 682, sort cmp about 3000, format %s about 2860, coroutine nesting about 11,450, host lua_call about 7150, host lua_resume about 5700. Linux gsub cb only 102 (luaL_Buffer = BUFSIZ = 8 KB per level) -> info
- Linux x64 (WSL, 1 MB pthread stacks, 128 KB margin, final): 31/31 completed, baseline 22/31 SIGSEGV -> PASS
- JVM (JDK 21 Temurin, JNI, Java threads with default 1 MB stack, final): margins 64/128/256 KB, 8/8 cases including JNI upcalls into Java recursion at every level give catchable 'C stack overflow', JVM alive. Baseline: JVM dies silently (0xC00000FD, no hs_err) in 5/5 -> PASS
- Red-zone refusal of lua_resume (host wrapper) and of stock coroutine.resume leaves the coroutine resumable; resuming later returns 'got again', status suspended -> PASS
- Patch application: build-patched-luajit.sh with 01..08 on pristine vendor/luajit 19878ec0 reproduces the dev tree exactly (0 differing files) and builds. 08 applies alone on 01-06. 07+08 apply on 3c4f9fe2+01-06 (rebase tree); that build passes 31/31 C-stack cases, 5 pattern interrupts, pause, compat_check -> PASS
- Gating: 01-08 built with neither LUAJIT_ENABLE_INTERRUPT nor CSTACKGUARD compiles (a gating bug was found and fixed) and emits no asm guard. Two-Way plain find stays active there -> PASS

## Design notes

PATCH 07: interruptible C pattern matching. Gated by -DLUAJIT_ENABLE_INTERRUPT; requires patch 01.

1. Where polls are placed.
- lib_string.c MatchState gains `global_State *g` (set in all three drivers: str_find_aux, gmatch_aux, gsub).
- match() calls ms_poll at every entry, i.e. at every backtracking step, including the '?', capture, max_expand and min_expand paths. A poll is `if (ms->g->intr) ms_intr(ms)`; the slow path is out of line (LJ_NOINLINE).
- The linear scans in max_expand and matchbalance poll whenever the scan pointer crosses a 4096-byte address boundary (`((uintptr_t)p & 4095) == 0`). This is register-only work per character.
- lib_table.c auxsort polls (lj_intr_poll) in both partition scans and once per partition. An adversarial "median-of-3 killer" input makes the default-comparator sort O(n^2) without running any Lua code.

2. The poll itself: lj_dispatch_intrpoll(L) in lj_dispatch.c.
- It acts only if all of these hold: g->intr is set, a count hook is installed, no hook is active, and the code is not on a trace (jit_base == NULL).
- It saves L->top, resets hookcount, and runs callhook(L, LUA_HOOKCOUNT, -1) inside lj_vm_cpcall. callhook clears g->intr, as patch 01 does for the normal count hook.
- On error it restores the hook state and re-throws with lj_err_throw(L, status) in the context of the library function. This is the same unwinding path as the existing lj_err_caller("pattern too complex") raised from inside match(). Like other hook errors (patch 02), it skips xpcall message handlers.
- Because the hook runs inside a CP frame without the RESUME bit, lua_isyieldable() returns 0 and lua_yield() raises a catchable error. This prevents the state corruption a hook-yield from inside a C function would otherwise cause.

3. Why this is safe.
- Strings are anchored by stack slots or upvalues, and LuaJIT's GC does not move objects.
- MatchState and luaL_Buffer live on the C stack and use stack indices only.
- gmatch's position upvalue lives in the GCfuncC object, not on the Lua stack.
- The hook may grow the Lua stack and run the GC; no Lua-stack pointers are held across a poll, and L->top is restored afterwards.
- Polls are only reached from lua_CFunctions run by the interpreter. The pattern matcher is never recorded by the JIT.

4. Deferral and rate limit.
- A hook that cannot service a request in C (for example a pause) re-arms it with luaJIT_interrupt(L). That call sets hookcount=1, so the count hook runs at the next bytecode boundary.
- After a hook call from C returns normally, the next LJ_INTR_CSKIP (4096, overridable with -D) polls are skipped. The counter is the new unconditional field g->intrskip, which the count hook resets. A new request from another thread is therefore seen after at most 4096 polls.

5. lj_str.c lj_str_find (plain find).
- This function is also called from traces and from the recorder, where polling is impossible. Its memchr/memcmp loop now carries a candidate budget of 64 + 8*slen/plen.
- When the budget runs out, it hands over to an O(1)-space Two-Way (Crochemore-Perrin) search, which bounds the worst case to O(n+m). Results are unchanged, and typical searches never reach the hand-over.
- This part is always on (not gated), like patches 04 and 05.

6. What the host must do.
- The count hook may now run with a C function as frame 0. Hard and soft aborts work unchanged (error if not yieldable).
- For a pause, the hook must check lua_isyieldable() and, if it cannot yield, re-arm (`req |= PAUSE; luaJIT_interrupt(L)`).

7. Not polled, because they are bounded by memory and therefore by the allocator cap (P9): string.rep, table.concat, lj_buf growth, string.format (width at most 99), and unpack (LUAI_MAXCSTACK).

8. Known limitations of 07.
- Polls do nothing while a hook is active (finalizers, Lua hooks), the same as before.
- While a deferred pause is pending inside a long C match, matching runs up to about 40% slower.

PATCH 08: C stack guard. Gated by -DLUAJIT_ENABLE_CSTACKGUARD.

1. API and state.
- `LUA_API void luaJIT_setcstacklimit(lua_State *L, const void *lo, const void *hi)` sets the red zone [lo, hi) of the thread that runs L; lo == hi disables it.
- The host computes the bounds per worker thread:
  - Windows: GetCurrentThreadStackLimits.
  - Linux: pthread_getattr_np + pthread_attr_getstack.
  - macOS: pthread_get_stackaddr_np minus pthread_get_stacksize_np.
- Use lo = stack low limit and hi = lo + margin. Call it before every lua_resume on the current worker thread; cache the bounds in a thread-local.
- New global_State fields cstack_lo and cstack_hi, plus a 6-word pad so the struct grows by exactly 64 bytes. They are unconditional, so the layout does not depend on build flags.

2. The check.
- lj_cstack_inzone(g, sp, slack) is `sp < hi && sp + slack >= lo`, with sp = __builtin_frame_address(0) (or _AddressOfReturnAddress on MSVC). The common case costs one compare.
- A zone that belongs to another thread is inactive rather than wrong.

3. Guard points and what each does in the zone.
- lua_call raises lj_err_cstkov ("C stack overflow").
- lua_pcall raises the error inside a protected C frame, then replaces the function and its arguments with the error object, so it never throws. Its message handler is not called for this error.
- lua_cpcall substitutes a C function that raises the error (returned as a status).
- lua_resume drops the arguments, pushes the preallocated message string (fixstring at state creation, no allocation), and returns LUA_ERRRUN. The coroutine is unchanged and can be resumed later.
- The metamethod paths of lua_equal, lua_lessthan, lua_concat, lua_gettable, lua_getfield, lua_settable, lua_setfield and luaL_callmeta raise the error. These cover gsub table replacement, sort __lt, __tostring via string.format and host calls.
- Parser synlevel_begin raises it as a lex error.

4. Coroutine fast path (x64 only).
- vm_x64.dasc gets a guard in the coroutine.resume and coroutine.wrap fast functions: `cmp rsp,[hi]; jae 1f; cmp rsp,[lo]; jae ->fff_fallback`.
- The C fallback ffh_resume re-checks with 4 KB of slack, so the retry cannot loop. resume then returns (false, "C stack overflow") and wrap raises.
- The DynASM flag CSTACKGUARD is derived automatically from LJ_HASCSTACKGUARD_FF, which is defined only for x64 GC64.

5. Not guard points, because each is a single bounded re-entry:
- The xpcall message handler call in lj_err_run. Recursion through it is already capped by the LUA_ERRERR logic.
- __gc finalizer calls.
- JIT compiler, trace exits, GC and hook execution at depth. These rely on the margin.

6. Margin.
- Measured: 32 KB works natively; 16 KB fails on Windows.
- In the JVM, 64, 128 and 256 KB all passed, including JNI upcalls at every level.
- Recommended: 128 KB on 1 MB CC:T threads. That leaves 680 gsub levels on Windows, but only about 100 on Linux, because LuaJIT's luaL_Buffer is BUFSIZ (8 KB) per gsub frame.

7. Limitations of 08.
- The VM fast-path guard exists only for x64 GC64. On arm64 the PLAN's C coroutine wrapper (lua_resume is guarded) covers coroutines. Porting the 4-instruction check to vm_arm64.dasc is untested.
- msvcbuild.bat would not pass the DynASM flag.
- Not tested on macOS or arm64.

## Files (scratch workspace)

- `work/proto-patterns-cstack/07-interruptible-patterns.patch`
- `work/proto-patterns-cstack/08-cstack-guard.patch`
- `work/proto-patterns-cstack/patchstack/build-patched-luajit.sh`
- `work/proto-patterns-cstack/harness/pharness.c`
- `work/proto-patterns-cstack/harness/cstack.c`
- `work/proto-patterns-cstack/harness/runlua.c`
- `work/proto-patterns-cstack/harness/strfind_test.c`
- `work/proto-patterns-cstack/harness/strfind_twoway.h`
- `work/proto-patterns-cstack/harness/run_pharness.sh`
- `work/proto-patterns-cstack/harness/run_cstack.py`
- `work/proto-patterns-cstack/jni/CStackJni.java`
- `work/proto-patterns-cstack/jni/cstackjni.c`
- `work/proto-patterns-cstack/linux/plinux.c`
- `work/proto-patterns-cstack/lua/patfuzz.lua`
- `work/proto-patterns-cstack/lua/chunkfuzz.lua`
- `work/proto-patterns-cstack/lua/patbench.lua`
- `work/proto-patterns-cstack/lua/cobench.lua`
- `work/proto-patterns-cstack/lua/killer.lua`
- `work/proto-patterns-cstack/lua/genkiller.lua`
- `work/proto-patterns-cstack/tools/final_verify.sh`
- `work/proto-patterns-cstack/tools/benchrun.py`
- `work/proto-patterns-cstack/tools/build.sh`
- `work/proto-patterns-cstack/final/preempt.txt`
- `work/proto-patterns-cstack/final/compat.txt`
- `work/proto-patterns-cstack/final/pharness.txt`
- `work/proto-patterns-cstack/final/fuzz.txt`
- `work/proto-patterns-cstack/final/cstack-1m-128k.txt`
- `work/proto-patterns-cstack/final/cstack-256k-64k.txt`
- `work/proto-patterns-cstack/bench-final.txt`
- `work/proto-patterns-cstack/bench-07variants.txt`
- `work/proto-patterns-cstack/bench-08-coroutines.txt`
- `work/proto-patterns-cstack/bench-08-pad.txt`
- `work/proto-patterns-cstack/logs-cstack-base-1m.txt`
- `work/proto-patterns-cstack/logs-cstack08-1m-m16.txt`
- `work/proto-patterns-cstack/logs-cstack08-1m-m32.txt`
- `work/proto-patterns-cstack/logs-cstack08-8m.txt`
- `work/proto-patterns-cstack/logs-jni-final.txt`
- `work/proto-patterns-cstack/logs-linux-final.txt`
- `work/proto-patterns-cstack/logs-cstack-linux-1m.txt`
- `work/proto-patterns-cstack/logs-fuzz-rearm.txt`
- `work/proto-patterns-cstack/logs-phbase-error.txt`
- `work/proto-patterns-cstack/logs-r3c-cstack.txt`
- `work/proto-patterns-cstack/tree`
- `work/proto-patterns-cstack/stackbuild`

## Suggested plan updates

1. TL;DR table, row "Is it feasible at all?". Replace the last sentence ("Two items remain designed but unprototyped: transparent "pause" (time-slicing) and interruptible C pattern matching.") with:
"One item remains designed but unprototyped: transparent "pause" (time-slicing). Interruptible C pattern matching (patch 07) and the C-stack guard (patch 08) are written and tested on Windows x64, Linux x64 and inside a JDK 21 JVM."

2. §3, row P2, Status column. Replace "Fix known, not written: patch 07, which polls the interrupt flag in `lib_string.c` matching." with:
"**Solved and tested.** Patch `07` polls the interrupt flag at every pattern-match step, in the `.*` and `%b` scans and in `table.sort`. The count hook then runs in a protected, non-yieldable C frame: errors work, yields are refused. 17/17 pathological find/match/gmatch/gsub/sort cases stop in 2–50 µs, where the 01–06 baseline hangs on all of them. Plain `string.find` gets a linear-time Two-Way fallback; it is also used on JIT traces, where polling is impossible (worst case ~90 s → 12 ms). Semantics are identical in a 2.2M-result differential fuzz. Overhead is +1.8% geomean on pattern microbenchmarks (worst +9%)."

3. §3, row P8, Status column. Replace with:
"**Backstop solved and tested:** patch `08` adds `luaJIT_setcstacklimit(L, lo, hi)` (a red zone on the C stack). Every C→VM re-entry raises a catchable "C stack overflow" in the zone: `lua_call`/`pcall`/`cpcall`/`resume`, C-API metamethods, the parser, and the x64 fast path of `coroutine.resume`/`wrap`. Over 31 recursion shapes on 1 MB stacks (Windows, Linux, JDK 21 Java threads): 0 crashes, and the state stays usable. The baseline crashed in 21 of 29 shapes on Windows and 22 of 31 on Linux, and killed the JVM silently. Overhead is about 0.5%. Keep the coroutine-nesting limit and Lua `sort`/`gsub` in the sandbox: they also give portable behaviour, and the VM fast-path check exists only on x64."

4. §4.2, the "Build LuaJIT with" bullet. Change the flags to
`-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD`
and replace "Patches 07–08 are to be written." with:
"Patches 01–08 apply to 19878ec0, and 07–08 also apply to 3c4f9fe2 + 01–06."

5. §4.3, Interrupt protocol. Add these bullets:
- "The count hook can now also be called from inside long C library functions (pattern matching, `table.sort`). There, frame 0 is a C function and `lua_isyieldable()` returns 0. Soft and hard abort still raise errors there. A PAUSE that cannot yield must be re-armed with `req |= PAUSE; luaJIT_interrupt(L)`; the request is then seen at the next bytecode boundary, and patch 07 rate-limits repeated calls from C."
- "C stack: before every `lua_resume` on a CC:T worker thread, call `luaJIT_setcstacklimit(L, lo, lo + 128 KB)`. Get `lo` from `GetCurrentThreadStackLimits` (Windows), `pthread_getattr_np` (Linux) or `pthread_get_stackaddr_np` minus `pthread_get_stacksize_np` (macOS), cached per thread. Measured: 32 KB is enough natively, and 64–256 KB all worked in the JVM. On Linux, LuaJIT's `gsub` costs 8 KB of C stack per nesting level (BUFSIZ buffer), so a 1 MB thread allows only ~100 nested `gsub` callbacks."

6. §5, WP B row. Change "write 07 (interruptible patterns) and 08 (C-stack guard)" to "07/08 done; port the coroutine fast-path check to arm64 or rely on the C wrapper", and lower the estimate to 1–2 d experienced, 3–5 d newcomer.

7. artifacts/luajit-patches/README.md:
- Add table rows:
  - `| 07 | 07-interruptible-patterns.patch | -DLUAJIT_ENABLE_INTERRUPT (Two-Way find always on) | pattern matcher + table.sort poll the interrupt flag and run the count hook in a protected non-yieldable C frame; lj_str_find gets a linear-time Two-Way fallback | proto-patterns-cstack |`
  - `| 08 | 08-cstack-guard.patch | -DLUAJIT_ENABLE_CSTACKGUARD | luaJIT_setcstacklimit(L, lo, hi); catchable "C stack overflow" at every C→VM re-entry (x64 VM fast path for coroutine.resume/wrap) | proto-patterns-cstack |`
- Add -DLUAJIT_ENABLE_CSTACKGUARD to the recommended flags.
- Replace the "Still to write" section with "Open: arm64 port of the 4-instruction coroutine fast-path check (vm_arm64.dasc); msvcbuild.bat does not pass -D CSTACKGUARD to DynASM."
- In "Verified", replace "X_string_find_backtrack (needs patch 07...)" with "X_string_find_backtrack: interrupted in 1.8 µs with 07".

## Effort update

WP B (LuaJIT patch stack): patches 07 and 08 are written and tested. They were verified on Windows x64 (native threads and a JDK 21 JVM) and on Linux x64, both on the 19878ec0 base and on 3c4f9fe2 + 01-06. One test-and-rework round has been done, including fixing an alignment regression and a gating bug.
- Remaining WP B work, about 1-2 days for an experienced developer, 3-5 days for a newcomer:
  - port the coroutine fast-path guard to arm64, or rely on the planned C coroutine.resume/wrap wrapper;
  - run on macOS and arm64 in CI;
  - add pharness, cstack and the fuzzers to CI;
  - adapt build-patched-luajit.sh to pass -DLUAJIT_ENABLE_CSTACKGUARD.
- This takes about 1-2 days (experienced) and 2-5 days (newcomer) off the PLAN's WP B estimate.
- The host-side work in WP C is roughly unchanged:
  - per-thread stack bounds and a luaJIT_setcstacklimit call before each resume: about 0.25 day;
  - handling hook calls from C context and re-arming deferred pauses: about 0.25 day.
