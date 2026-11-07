# LuaJIT C-API runtime behaviour for a CC:Tweaked machine (lane: luajit-runtime)

Investigator key: `luajit-runtime`. Date: 2026-11-07.
Work dir (all sources, binaries, logs): `<scratch>\work\luajit-runtime`
(below abbreviated as `W\`). Nothing was written inside `<repo>`.

## 0. TL;DR

* **Stock LuaJIT cannot be interrupted, paused or time-sliced while it runs JIT-compiled code.** An asynchronous
  `lua_sethook(L, hook, LUA_MASKCOUNT, 1)` (exactly what the mod's `abortMachine` does) interrupted only 2 of 22 loop
  shapes with the JIT on. No `jit.opt` setting fixes this (best: 7/22). A permanently installed count hook does not
  help either (2/22): the hook never runs inside traces. With the JIT off, 22/22 are interrupted in under 0.1 ms.
* **The build-time option `-DLUAJIT_ENABLE_CHECKHOOK` fixes it without a source patch.** It is a documented
  define (`lj_record.c:2880`). With it: 25/25 shapes are aborted (latency ≤ 0.03 ms natively, about 308 ms end to end in a JVM
  with a 300 ms timer). Async pause/yield worked 100 times out of 100 in a JVM. Results stayed correct after every
  yield/resume. It costs nothing measurable when no hook is set. **But** a permanently installed hook under
  CHECKHOOK is a disaster (15 to 55 times slower than the JIT, and slower than the interpreter). So hooks must be armed only on demand,
  from another thread.
* Interrupts must be **sticky** (keep raising until the coroutine is dead). The mod's one-shot hook (clear, then error) is
  swallowed by `while true do pcall(f) end`, even with the JIT off.
* A yield from a hook must check `lua_isyieldable()`. Otherwise it raises "attempt to yield across C-call boundary"
  into user code (sort comparators, gsub callbacks, C `lua_call`).
* **Several process-killing vectors exist** and the machine must close each one:
  native C-stack overflow (coroutine nesting crashes even with a 256 MB stack; gsub/sort/format/C-call recursion
  crashes at 1 MB, and the JVM dies silently with 0xC00000FD); unprotected Lua errors or OOM
  (Windows: JVM "Internal Error (0xe24c4a02)" with hs_err; Linux: `exit(1)`); untrusted bytecode (94 of 1000
  mutated chunks crashed); uninterruptible `__gc` / `jit.attach` callbacks (hooks are disabled there).
* Memory capping works (`lua_newstate` with a custom allocator is fine on GC64, and OOM is catchable). But the GC itself allocates,
  so a hard cap can wedge the state. Use a reserve. Wrap LuaJIT's own allocator for speed.
* Thread affinity is not an issue: a state can be created, resumed and finished on 3 different threads. 32 concurrent
  states, and a 60 s, 28k-shot cross-thread `lua_sethook` race stress, ran with no crash or wrong result.

## 1. Setup and methodology

* LuaJIT: copy of the shared prebuilt tree (2.1 ROLLING, GC64, x64; the same tree the mod's submodule pins,
  `vendor/luajit` at 19878ec0). Three builds were made, all `BUILDMODE=static`:
  * `W\luajit\src\libluajit.a`: stock (`mingw32-make -j24 BUILDMODE=static`, log `W\build-static.log`).
  * `W\luajit-checkhook\src\libluajit.a`: `mingw32-make -j24 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK`
    (log `W\build-checkhook.log`). No source changes.
  * `W\luajit-linux\`, `W\luajit-linux-ck\`: same two builds, made under WSL Ubuntu with gcc (log `W\build-linux.log`).
* The main harness is `W\rt.c` (one file, compiles as C and as C++). Subcommands `a`..`j`, `gf`. Build commands:
  `gcc -O2 -Iluajit/src rt.c luajit/src/libluajit.a -o rt_c.exe`,
  `g++ -O2 -x c++ -Iluajit/src rt.c -x none luajit/src/libluajit.a -o rt_cpp.exe`, and
  `rt_ck.exe` as `rt_c.exe` but linked against the CHECKHOOK lib. MinGW-w64 gcc 16.2 (WinLibs, SEH, MSVCRT).
* Hangs are detected by a watchdog thread inside each test process. The watchdog prints `HUNG` and calls `ExitProcess(3)`
  2 s after the interrupt was requested. Crashes are detected by running each case in its own process through `W\runx.py`,
  which prints the full 32-bit Windows exit code.
* Extra programs: `W\b3x.c` (async yield latency), `W\oomwedge.c` (OOM wedge diagnosis), `W\allocwrap.c`
  (allocator options and leak check), `W\yreq.c` (yield-request patterns), `W\luasort_bench.lua`, `W\jni\LjJni.java` +
  `W\jni\ljjni.c` (LuaJIT inside a real JVM, Temurin 21.0.13), `W\lnx.c` + `W\lnx_run.sh` (Linux counterparts).
* Drivers: `W\run_a.sh` (test-a matrix, parallel via xargs), `W\gfuzz.py` (bytecode fuzz).
* Benchmarks ran while other agents were using the machine. Expect ±30 % noise on absolute timings. The ratios are robust.

## 2. Results

### a. Asynchronous interrupt (second OS thread calls `lua_sethook(L, hook_err, LUA_MASKCOUNT, 1)` after 300 ms; the hook raises an error)

Command: `W\run_a.sh ./rt_c.exe results_a.txt` (and `CFGS=... ./run_a.sh ./rt_ck.exe ...`). Each cell shows the latency in ms from
`lua_sethook` to the hook firing, or **HUNG** (no interrupt within 2 s). The runner is `lua_resume` on a coroutine unless the column
says `/pcall`. Configs:
`on` = default JIT; `off` = `jit.off()`; `O0` = `jit.opt.start(0)`; `noloop` = `-loop`; `maxside0` = `maxside=0`;
`hotexit1`; `hotloop60k` = `hotloop=60000,hotexit=60000`; `permhook` = count hook (every 1000 instructions) installed before the run, which
checks an abort flag set by the timer thread; `permhookoff` = the same with the JIT off; `onsticky`/`offsticky` = the hook stays installed and
errors on every instruction; `@CK` = CHECKHOOK build. Full table: `W\table_a.md`. Raw data:
`W\results_a*.txt`.

| shape | on | off | O0 | noloop | maxside0 | hotloop60k | permhook | permhookoff | onsticky | off/pcall | on@CK | onsticky@CK | permhook@CK |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| while true do end | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.01 | 0.02 | 0.01 | 0.00 |
| numeric for 1e12 | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.03 | 0.02 |
| repeat until false | HUNG | 0.01 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.01 | 0.01 | 0.01 | 0.02 |
| goto loop | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.01 | 0.01 | 0.01 | 0.00 |
| math.sin loop | HUNG | 0.01 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.00 | 0.01 | 0.01 |
| table alloc loop | HUNG | 0.01 | 0.01 | 0.01 | HUNG | HUNG | HUNG | 0.05 | HUNG | 0.00 | 0.01 | 0.01 | 0.01 |
| string concat loop | 0.04 | 0.01 | 0.21 | 0.08 | 0.15 | 0.03 | 2.78 | 0.00 | 0.15 | 0.01 | 0.00 | 0.00 | 0.02 |
| Lua function call loop | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.02 | HUNG | 0.00 | 0.00 | 0.01 | 0.01 |
| C function call loop | HUNG | 0.01 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.02 | 0.01 | 0.01 |
| string.byte loop | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.01 |
| tail recursion | HUNG | 0.04 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.01 | 0.01 | 0.01 | 0.01 |
| mutual tail recursion | HUNG | 0.12 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.01 |
| coroutine.wrap generator | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.03 | 0.00 | 0.01 | 0.01 |
| nested loops | HUNG | 0.00 | HUNG | HUNG | 0.02 | 0.00 | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.02 |
| rare branch (side trace) | HUNG | 0.07 | HUNG | HUNG | 0.01 | 0.00 | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.01 |
| pairs / ipairs loops | HUNG | 0.00 | HUNG | HUNG | 0.0x | 0.00 | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01–0.03 | 0.0x |
| pcall(g) loop | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.00 |
| string.format loop | 0.00 | 0.03 | 0.16 | 0.11 | 0.15 | 0.01 | 14.6 | 0.00 | 0.20 | 0.00 | 0.01 | 0.00 | 0.00 |
| recursion (non-tail) loop | HUNG | 0.01 | HUNG | HUNG | 0.01 | 0.00 | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.02 | 0.02 |
| upvalue closure loop | HUNG | 0.01 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.00 | 0.01 | 0.01 | 0.02 |
| table.insert/remove loop | HUNG | 0.00 | HUNG | HUNG | HUNG | HUNG | HUNG | 0.00 | HUNG | 0.01 | 0.00 | 0.00 | 0.00 |
| `while true do pcall(g) end`, g busy | – | HUNG(1-shot) | – | – | – | – | – | – | 0.05 | HUNG | HUNG(1-shot) | 0.01 | HUNG(1-shot) |
| same with xpcall | – | HUNG(1-shot) | – | – | – | – | – | – | 0.21 | HUNG | HUNG(1-shot) | 0.02 | HUNG(1-shot) |
| nested pcall spin | – | HUNG(1-shot) | – | – | – | – | – | – | 0.20 | HUNG | HUNG(1-shot) | 0.03 | HUNG(1-shot) |

Counts: stock `on` 2/22; `off` 22/22; `O0` 3/22; `noloop` 3/22; `maxside0` 7/22; `hotexit1` 2/22; `hotloop60k` 7/22;
`permhook` 2/22; `permhookoff` 22/22; stock `onsticky` 5/25; **CHECKHOOK + sticky: 25/25**; CHECKHOOK one-shot: 22/25 (the
3 failures are the pcall-swallow cases). The state was usable after every interrupt ("after=ok" in the raw data).
The hook fired once per interrupt with the one-shot hook, and 2–3 times with the sticky hook (once for each enclosing pcall).

Swallowing check (`rt_c.exe a pcallspin off pcall` vs `offsticky`): one-shot → `HUNG fired=1`; sticky → interrupted,
`hookcalls=2`. The mod's `thread_interrupt_hook` (`src/main/cpp/main.cpp:182-187`) clears the hook and then errors, which makes it the one-shot kind.

The same behaviour holds on Linux (WSL, `W\results_linux.txt`): stock `while true do end` HUNG; CHECKHOOK `while` and
numeric for aborted in 0.006 / 0.007 ms. It also holds in a JVM (`W\results_jni.txt`, Java thread calls the native `abort` after 300 ms):
CHECKHOOK aborted 4/4 loops (`while`, for 1e12, pcall spin, tail recursion) at about 308 ms total. The stock build hung on the first one.

Why: LuaJIT checks hooks only in the interpreter (`lj_dispatch_ins`, `lj_dispatch.c:429`). `lua_sethook` only rewrites
the interpreter dispatch table (`lj_dispatch.c:329-340` → `lj_dispatch_update`). A trace that loops back to itself never
returns to the interpreter. With `LUAJIT_ENABLE_CHECKHOOK`, the recorder emits a volatile load of `g->hookmask` plus a guard at
the head of every root trace and at ITERN loops (`lj_record.c:670`, `lj_record.c:2880-2899`). The upstream comment there
explicitly endorses "set the instruction hook via lua_sethook() with a count of 1 from a signal handler or another
native thread".

### b. Yield from a count hook (`lua_yield(L,0)` inside the hook, coroutine driven by `lua_resume`)

`rt_c.exe b` / `rt_ck.exe b` → `W\results_b_stock.txt`, `W\results_b_checkhook.txt`; `b3x_*.exe` →
`W\results_b3x_*.txt`.

| case | stock | CHECKHOOK |
|---|---|---|
| b1 interpreted, permanent hook every 1000 instructions, yields | 270001 yields, result CORRECT, 592 ms | same |
| b2 JIT, permanent hook every 1000 instructions | **0 yields** (hook never ran), correct, 123 ms | 270001 yields, correct, **3905 ms** |
| b3 JIT, one-shot yield hook armed every ~20 ms by another thread (3e8-iteration loop, 4.2 s) | 1 yield | 37 yields, correct |
| b3x: one-shot yield armed every ~5 ms, latency (9 loop shapes) | most shapes: 1 yield, latency = whole run (0.4–3 s) | every ping answered; avg 0.002–0.007 ms, max 0.1 ms; all results correct |
| b4 yield from hook in the main thread under `lua_pcall` | ERRRUN "attempt to yield across C-call boundary" | same |
| hook fires inside pcall / `__index` fn / `__tostring` (naive hook) | yields OK, results correct | same |
| hook fires inside table.sort comparator / gsub callback / C `lua_call` (naive hook) | **ERRRUN "attempt to yield across C-call boundary"** thrown into user code | same |
| same with a hook that checks `lua_isyieldable()` and defers | 0 yields, 115/200/400 deferrals, results correct | same |
| JVM: Java thread requests a yield every 10 ms, 100 times (`LjJni pause`) | 1 yield in 5.2 s | **100/100 yields**, result correct |

Yield-request pattern check (`W\yreq.c`, `W\results_yreq.txt`): the mod's approach (a C function arms a count=1 hook that
yields on the next instruction, `main.cpp:466`) and a C function that simply `return lua_yield(L,n)` both produced
1,000,000 yields for 1,000,000 calls from a hot loop, with correct results, on both builds. Resume values must be passed back
(`lua_resume(co, lua_gettop(co))`).

### c. Yield across boundaries (coroutine started with `lua_resume` from C; body calls `coroutine.yield`)

`rt_c.exe c` → `W\results_c.txt` (identical on CHECKHOOK, `W\results_c_checkhook.txt`).

| construct | LuaJIT 2.1 | Cobalt / CC:T 1.121 (source inspection) |
|---|---|---|
| plain, `pcall`, `xpcall` body, os.pullEvent-style inside pcall | **OK** | OK |
| metamethods `__index`, `__newindex`, `__call`, `__lt`, `__le`, `__eq`, `__concat`, `__add`, `__unm` | **OK** | OK |
| `for ... in <Lua iterator>`, `table.foreach`, `tostring` + `__tostring`, `select(..., coroutine.yield())`, nested coroutine.wrap, a chunk from load | **OK** | OK |
| C function that does `return lua_yield(L, n)` | **OK** | n/a |
| `table.sort` comparator | **FAIL** "attempt to yield across C-call boundary" | resumable (`TableLib.java` `@AutoUnwind heapSort`, line 295) |
| `string.gsub` callback | **FAIL** | resumable (`StringLib.java:141` `GSub extends ResumableVarArgFunction`) |
| `string.format('%s', t)` with `__tostring`; `print(t)` with `__tostring` | **FAIL** | format resumable (`StringLib.java:160`) |
| `load(reader)` where the reader yields | **FAIL** (load returns nil, err) | resumable (`BaseLib.java:316` `Load`) |
| xpcall error handler that yields | FAIL ("error in error handling") | – |
| C function doing `lua_call` / `lua_pcall` on a yielding Lua function | FAIL | n/a |
| `#t` with `__len`, `pairs` with `__pairs` | not honoured (5.1 semantics; no `LUAJIT_ENABLE_LUA52COMPAT`) | honoured |

Common CC code (`os.pullEvent` inside `pcall`, coroutine-heavy `parallel`) works. Code that yields (directly or through a
yielding CC API call such as peripheral or turtle calls) inside a sort comparator, gsub callback or `__tostring` used by
format/print breaks under LuaJIT. Mitigation: a pure-Lua `table.sort` is yieldable **and 3.2 times faster** under the JIT
(1e6 doubles with a Lua comparator: C `table.sort` 419 ms vs pure-Lua quicksort 131 ms; C sort with the default `<` takes 216 ms;
`W\results_luasort.txt`). Similar Lua-side wrappers can cover gsub-with-function, format `%s` and load(reader).

### d. Errors

`rt_c.exe d all`, `rt_cpp.exe d all`, `runx.py rt_*.exe d unprotected|atpanic|unprotected-oom` → `W\results_d.txt`;
Linux: `W\results_linux.txt`; JVM: `W\results_jni.txt`, `W\jni\hs_err_c_unprotected.log`.

| case | Windows x64 MinGW (C and C++) | Linux x64 gcc |
|---|---|---|
| `luaL_error` in a C function called from Lua during `lua_resume` | ERRRUN, message intact, coroutine dead (status 2), main state usable; resuming the dead coroutine again gives "cannot resume non-suspended coroutine" | – |
| table as error object through resume | ERRRUN, object preserved | – |
| error in xpcall handler / in `lua_pcall` errfunc | "error in error handling" / LUA_ERRERR | – |
| **`lua_error` with no protected frame** | **process dies with unhandled SEH exception 0xE24C4A02. The panic function is never called** (`lj_err.c:389` RaiseException; panic only on unwinder failure, `lj_err.c:777`). In a JVM: **fatal error "Internal Error (0xe24c4a02)", hs_err written** | prints `PANIC: unprotected error in call to Lua API (boom)` and calls `exit(1)`. In a JVM this ends the server process |
| `lua_atpanic` + longjmp | never invoked → same crash (0xE24C4A02) | invoked; longjmp worked and the state was reusable in this trivial case (not a safe design) |
| OOM in an API call outside protection (8 MB cap, `lua_createtable`) | dies with 0xE24C4A04 | (panic + exit expected) |
| C++: `luaL_error` unwinding through a g++ frame with an RAII object | error propagates, **destructor NOT run** | destructor run |
| C++: `catch(...)` around an unprotected `lua_error` | **not caught → process crash** | caught |
| C++: try/catch around `lua_call` while Lua errors | catch blocks skipped; error still reaches the outer pcall correctly | – |
| C++ `throw` inside a lua_CFunction under pcall | ERRRUN "C++ exception", state usable | – |
| JVM: Lua error, C error, OOM, Java exception converted to a Lua error, pcall catching it | all correct; SEH unwinding inside JNI frames does not interfere with HotSpot | – |

### e. Memory

`rt_c.exe e` → `W\results_e.txt`; `W\oomwedge.c`; `W\allocwrap.c` → `W\results_allocwrap.txt`.

* `lua_newstate(custom_alloc, ud)` works on x64 GC64 (returns non-NULL; the "must use luaL_newstate" restriction applies only to
  non-GC64 x64, `lj_state.c:239` and `lib_aux.c:392-395`). 35 KB after `openlibs`. `lua_close` frees everything back to 0 bytes.
* 32 MB cap: table-of-tables, `string.rep(2^30)`, string doubling, allocation inside a JIT trace, `table.concat`, and 1e8 unique
  strings all give **LUA_ERRMEM "not enough memory"**, catchable. After a full GC the state is reusable. Peak never exceeded the cap.
  OOM inside `lua_resume` also gives ERRMEM.
* **OOM wedge:** after OOM inside a coroutine, the dead coroutine keeps its stack, and so the data, alive until the thread object
  is collected. Collecting needs memory: `lua_cpcall` allocates a 48-byte C closure, and the GC wants a 256 KB string-table resize.
  Even a protected `lua_gc(COLLECT)` then fails with ERRMEM, again and again. `lua_gc` without protection at the cap crashed the
  process (0xE24C4A04). Raising the cap temporarily by more than the resize size recovered the state (1 KB was enough in the small
  repro, but 256 KB was not enough in the bigger state, which needed 256 KB + 24 B). So keep a reserve of 1 MB or more for the machine's own work.
* Without a cap (the original mod uses `luaL_newstate`, `main.cpp:704`), one computer can allocate a 2 GiB string
  (`string.rep('x', 2^31-1024)` succeeded in 1.1 s) and exhaust host RAM. GC64 removes the old 2 GB limit.
  Note: `string.rep('x', 2^31)` returns "" (int32 wrap).
* Allocator options (alloc-heavy benchmark): lj_alloc via `luaL_newstate` 279 ms. **lj_alloc wrapped with a counting/capping
  shim** (`lua_getallocf` → `lua_setallocf(wrapper)`, and restore the original before `lua_close` so that `lj_alloc_destroy` runs, `lj_state.c:232-233`)
  376 ms, cap enforced, no leak over 300 create/close cycles. `lua_newstate` with msvcrt `realloc` 922 ms and +20 MB of
  private bytes after 300 cycles.

### f. Stack depth (each case in its own process, on a thread with the given stack reservation)

`rt_c.exe f <case> <KB>` → `W\results_f.txt`; Linux `W\results_linux.txt`; JVM `W\results_jni.txt`.

| case | 256 KB | 1 MB | 16 MB / 256 MB | Linux 1 MB / 8 MB |
|---|---|---|---|---|
| Lua recursion, `__index`-fn / `__concat` / `__tostring` recursion | catchable "stack overflow" | same | same | – |
| `pcall(f)` recursion | returns at depth 32744 | same | – | – |
| parser nesting `((((`, `{{{{`, nested functions | "chunk has too many syntax levels" (LJ_MAX_XLEVEL=200) | same | – | – |
| `unpack` of 1e6 | "too many results to unpack" | same | – | – |
| **string.gsub callback recursion** | **CRASH 0xC00000FD** | **CRASH** | "stack overflow" (catchable) | SIGSEGV / **SIGSEGV** |
| **table.sort comparator recursion** | **CRASH** | **CRASH** | catchable | SIGSEGV / ok |
| **string.format('%s') + `__tostring` recursion** | **CRASH 0xC0000005** | **CRASH** | catchable | – |
| **C function → lua_call recursion** | **CRASH** | **CRASH** | catchable | – |
| **coroutine.wrap / coroutine.resume nesting** | **CRASH** | **CRASH** | **CRASH even at 256 MB** | SIGSEGV / SIGSEGV |
| load(reader) recursion | CRASH | CRASH | ok | – |
| JVM, gsub recursion on a default-stack thread; coroutine nesting on main | – | **JVM process dies with 0xC00000FD, no hs_err** | 64 MB thread: catchable "stack overflow" | – |

LuaJIT has no C-call depth limit; only Lua stack slots (`LUAI_MAXSTACK`=65500 per coroutine) bound recursion. Each
coroutine has its own Lua stack, so nesting coroutines is bounded only by the native stack.

### g. Bytecode and raw-memory entry points (`rt_c.exe g`, run in `W\gtmp`; `W\results_g.txt`; fuzz `W\results_gfuzz.txt`)

| entry point | binary chunk accepted? | with mode "t" |
|---|---|---|
| `load(dump)`, `load(dump,n,'b')`, `loadstring(dump)` | yes, runs | `load(d,n,'t')` → "attempt to load chunk with wrong mode" |
| `load(reader returning dump)` | yes | refused with 't' |
| `loadfile(host path)` / `dofile(host path)` | yes, **and they read the real host filesystem** | `loadfile(f,'t')` refused; dofile has no mode |
| `require` via package.path | yes (host FS) | – |
| `load(string.buffer)` (needs `require 'string.buffer'`) | yes | – |
| C `luaL_loadbufferx(..., "t")` | – | refused (ERRSYNTAX "attempt to load chunk with wrong mode") |
| `package.loadlib` | native DLL loading = RCE | – |
| `ffi` | arbitrary memory read demonstrated (KUSER_SHARED_DATA) | – |
| `jit.util.ircalladdr(0)` | leaks a code address (ASLR) | – |

* Removing `string.dump` (`string.dump=nil`) is safe. String methods (`rep`, `upper`, `format`) are unaffected.
* **Fuzz:** 1000 chunks, each a dump of a small function with 3 random bytes changed, loaded and run under pcall: **94
  ACCESS_VIOLATION crashes, 2 hangs**, 137 load refusals, 188 caught Lua errors, 579 ran.
* The original mod opens base (with `load`, `loadstring`, `loadfile`, `dofile`) and string (with `dump`) (`main.cpp:707-711`)
  and loads bios through `luaL_loadbuffer` without a mode. CC:T 1.121 `bios.lua` replaces `loadfile`/`dofile` (lines 423ff) but
  **does not wrap `load`**, so `load(string.dump(f))` from user code reaches LuaJIT's bytecode loader. This gives memory corruption and RCE.

### h. Thread affinity and concurrency (`rt_*.exe h 32 <secs>`; `W\results_h_*.txt`)

* h1: create a state and run it until a `coroutine.yield` on thread A; resume it to completion on thread B (result equals the
  reference); on thread C, run new JIT code plus the old workload on the same state (correct). Same on both builds.
* h2: 32 threads × 5 independent states with a JIT-heavy workload, all at the same time: 0 wrong results.
* h3: 32 worker threads, each running a JIT-heavy workload (diverse loops, which means constant trace recording), plus one
  thread that fires `lua_sethook(error)` at random states at random 1–7 ms intervals. CHECKHOOK, 60 s: 28,209 shots,
  28,148 runs, all ended with "interrupted", 0 other errors, no crash. Stock, 30 s: 11,578 shots, no crash.
* All JIT state is per `global_State`. No cross-state interference was observed. The only hazard is the obvious one: the
  thread that calls `lua_sethook` must never touch a state that is being, or has been, `lua_close`d. The mod's `abortMachine`
  (`main.cpp:823-826`) has no such synchronization.

### i. Cost of a permanently installed count hook (`rt_c.exe i`, `rt_ck.exe i`; `W\results_i_*.txt`)

ms; hook = no-op that counts calls; "calls" = how often the hook ran.

| bench | JIT, no hook | JIT + hook 1e3 / 1e4 / 1e5 (stock) | calls (stock, JIT) | JIT + hook 1e3 / 1e4 / 1e5 (CHECKHOOK) | interp, no hook | interp + hook 1e3 / 1e4 / 1e5 | calls (interp, 1e3) |
|---|---|---|---|---|---|---|---|
| numloop 3e8 | 367 | 375 / 386 / 382 | 0 | **20721 / 22504 / 21279** | 1134 | 1536 / 1583 / 1628 | 1,200,000 |
| fib(35) | 66 | 67 / 67 / 66 | 0–5 | 2306 / 2340 / 2316 | 279 | 329 / 349 / 343 | 194,094 |
| table fill/sum | 187 | 186 / 195 / 190 | 0 | 3264 / 3253 / 3252 | 321 | 315 / 332 / 319 | 100,002 |
| string tostring+concat | 162 | 150 / 147 / 146 | 0 | 522 / 566 / 512 | 248 | 241 / 241 / 247 | 21,000 |
| sieve | 295 | 298 / 313 / 318 | 0–1 | 13064 / 12804 / 12813 | 759 | 711 / 705 / 707 | 373,449 |
| strops (format/upper/gsub/find) | 71 | 71 / 70 / 71 | 0–1 | 141 / 143 / 165 | 86 | 90 / 90 / 90 | 7,800 |
| mandel | 7.8 | 7.9 / 7.8 / 8.0 | 0–2 | 400 / 387 / 392 | 66 | 122 / 118 / 115 | 93,934 |

* Stock: an active count hook does **not** disable or slow the JIT. Traces are still recorded and run at full speed, but the hook
  practically never runs inside them, so it provides neither time-slicing nor abort. Interpreted: the hook adds 0–85 %, and the cost
  is almost independent of the count interval, because any count hook switches the interpreter to the per-instruction hook dispatch.
* CHECKHOOK without a hook: same speed as stock (all differences within noise: 0.67–1.0×).
* CHECKHOOK **with a permanent hook**: every trace entry fails its guard and exits, so the code runs 3–20 times slower than the plain
  interpreter. **Never leave a count hook installed in a CHECKHOOK build.** Arm it on demand only. b3x shows that on-demand
  arming every ~5 ms costs nothing measurable (numloop 2.72 s with 384 yields vs 3.0 s stock without yields).

### j. Other things the machine must handle (`rt_*.exe j <sub>`; `W\results_j*.txt`)

| case | result |
|---|---|
| `lua_close` while a coroutine is suspended | fine |
| `__gc` (newproxy) at `lua_close` | runs |
| **`__gc` with `while true do end`, at `lua_close`** | **HUNG**; async hook never fires (`lua_close` runs finalizers under `hook_enter`, `lj_state.c:336-347`) |
| **same, triggered by `collectgarbage()` or by normal allocation** | **HUNG**, JIT on and off, stock and CHECKHOOK (`gc_call_finalizer` → `hook_entergc`, `lj_gc.c:512`) |
| **native userdata's metatable hijacked: `getmetatable(ud).__gc = function() while true do end end`** | **HUNG** (the finalizer is looked up at finalization time). Native metatables need `__metatable` protection |
| finite 1 s finalizer | the interrupt is delayed until it ends (latency 696 ms) |
| **`jit.attach(function() while true do end end, 'trace')`** | **HUNG** with the JIT on (vmevent callbacks run with hooks disabled, `lj_vmevent.c:45`) |
| user `debug.sethook()` inside any coroutine | **clears the machine's hook globally** (hooks live in `global_State`, `lj_dispatch.c:334-336`) |
| errors in `__gc` | swallowed (sent to the ERRFIN vmevent, `lj_gc.c:526`) |
| max string | `2^31-1024` bytes OK (`LJ_MAX_STR`); `string.rep(x, 2^31)` → "" |
| 2e6 unique interned strings | about 0.5 s, fine; 2000 near-identical 1 MB strings about 0.55 s |

## 3. Implications for the 1.21.1 machine design (runtime layer)

1. **Build LuaJIT with `-DLUAJIT_ENABLE_CHECKHOOK`** (or the patch lane's equivalent) for every platform. Without it, the
   "Too long without yielding" guarantee and CC:T's pause/time-slicing are impossible while the JIT is on.
2. **Never keep a count hook installed during normal execution.** Arm hooks from CC:T's timeout/monitor thread:
   * pause: a one-shot yield hook (`count=1`). Yield only if `lua_isyieldable(L)`; otherwise keep it armed and retry
     (a count hook of about 1000 instructions works too).
   * soft abort: a one-shot error hook ("Too long without yielding", catchable, as in Cobalt).
   * hard abort: a **sticky** error hook (do not clear it inside the hook) until `lua_resume` returns. Then clear it.
   * Hold a per-machine mutex around `lua_sethook` and around `lua_close`/teardown.
3. **Run all Lua API work from Java/JNI inside `lua_cpcall`/`lua_pcall`/`lua_resume`.** That covers value conversion, API registration,
   `lua_gc` and state setup. Never rely on `lua_atpanic` (it is never called on Windows; it calls exit on Linux).
4. **Allocator:** use `luaL_newstate`, then `lua_getallocf` → `lua_setallocf(counting wrapper)`, and restore it before `lua_close`.
   Enforce a soft cap plus a reserve (≥ 1 MB) for the machine's own allocations and the GC. After ERRMEM, temporarily lift the cap for a
   protected full GC, or kill the computer. Account for 2 GB strings.
5. **Library surface:** open base, string, table, math, bit (coroutine comes with base). Delete `loadfile`, `dofile`,
   `string.dump`, `newproxy`, `gcinfo`. Replace `load`/`loadstring` with natives that force mode `"t"` and support the 5.2
   env argument. Load bios with `luaL_loadbufferx(..., "t")`. Never expose `ffi`, `jit`, `package`, `io`, `os`. If `debug` is
   exposed for compatibility, wrap `sethook`/`gethook`/`setmetatable`. Give native userdata metatables a `__metatable` field.
6. **C-stack safety:** cap coroutine nesting (count nested `coroutine.resume`/`wrap` per state, for example ≤ 128–200). Bound the
   C-recursive library entry points (gsub with a function replacement, sort with a comparator, format `%s`/tostring, load with a reader,
   any native that calls back into Lua) with a depth counter or a native-stack-remaining check (`GetCurrentThreadStackLimits` /
   `pthread_getattr_np`). Better still, replace sort/gsub/format/load-reader with Lua implementations: that also closes the
   yield-across gap with Cobalt and is faster under the JIT.
7. **Windows (MinGW) glue code must be C-style:** no RAII or `catch(...)` around anything that can raise a Lua error. Free JNI
   resources before raising (`luaL_error`/`lua_error` skip destructors there). The Linux g++ build behaves differently, so test both.
8. Thread affinity needs no special handling. Any CC:T computer thread can resume any machine.

## 4. Artifacts

* Harness/sources: `W\rt.c`, `W\b3x.c`, `W\oomwedge.c`, `W\allocwrap.c`, `W\yreq.c`, `W\lnx.c`, `W\lnx_run.sh`,
  `W\luasort_bench.lua`, `W\jni\LjJni.java`, `W\jni\ljjni.c`, drivers `W\run_a.sh`, `W\runx.py`, `W\gfuzz.py`, `W\gfuzz.sh`.
* Binaries: `W\rt_c.exe`, `W\rt_cpp.exe`, `W\rt_ck.exe`, `W\b3x_c.exe`, `W\b3x_ck.exe`, `W\jni\ljjni_c.dll`, `W\jni\ljjni_ck.dll`,
  `W\lnx_c`, `W\lnx_cpp`, `W\lnx_ck`.
* LuaJIT builds: `W\luajit` (stock static), `W\luajit-checkhook`, `W\luajit-linux`, `W\luajit-linux-ck`.
* Results: `W\results_*.txt`, `W\table_a.md`, JVM crash log `W\jni\hs_err_c_unprotected.log` and `W\jni\hs_err_ck_unprotected.log`.

## 5. Confidence and limits

* High confidence (reproduced several times, on several platforms or builds): JIT uninterruptibility, CHECKHOOK fix and its
  cost, pcall swallowing, yield-across matrix, stack-overflow crashes, unprotected-error crash, bytecode acceptance and crashes,
  finalizer/jit.attach hangs.
* Medium: exact benchmark ratios (shared, noisy machine). The OOM-wedge headroom size depends on the string-table size.
* Not tested: arm64/macOS. A HotSpot-on-Linux JVM (Linux behaviour was tested only in native processes). Real CC:T ROM programs
  under CHECKHOOK (another lane). Behaviour with `LUAJIT_ENABLE_LUA52COMPAT`.
