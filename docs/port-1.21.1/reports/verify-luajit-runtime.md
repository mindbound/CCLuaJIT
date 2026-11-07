# Adversarial verification of the luajit-runtime claims (RT-1, RT-3 … RT-11)

Verifier key: `verify-luajit-runtime`. Date: 2026-11-07.
Work dir (all sources, binaries, logs): `<scratch>\work\verify-luajit-runtime`
(abbreviated `V\` below). Nothing was written inside `<repo>` and nothing was taken from the investigator's
work dir except reading `rt.c`/`oomwedge.c` to understand one scenario (RT-10). All experiments below are my own code.

## 0. Verdict summary

| claim | verdict | severity (mine) | one-line reason |
|---|---|---|---|
| RT-1 stock JIT cannot be async-interrupted | **confirmed** (with caveat) | critical | 3/17 shapes interrupted JIT-on (concat, format, tostring), 17/17 JIT-off; jit.opt variants 0–2/8; same in a JVM and on Linux. Caveat: "impossible" holds for the default build only — the stock source has the `LUAJIT_ENABLE_CHECKHOOK` compile option (lj_record.c:2880). Also: the mod as written never turns the JIT on at all (see M-1), so RT-1 did not bite the old mod; it bites any port that enables the JIT. |
| RT-3 one-shot hook swallowed by pcall | **confirmed** | high | mod-identical hook: pcall/xpcall spin HUNG with hookcalls=1 (JIT off); sticky hook interrupted with 2 calls. |
| RT-4 count-hook time slicing useless under JIT, costly interpreted | **confirmed** | high | JIT: 0–4 hook calls per benchmark, full speed; interpreter: +14…+92 % independent of interval. |
| RT-5 yield from hook in non-yieldable frames errors | **confirmed** | high | mod's exact hook (clear + `lua_yield`) inside sort comparator / gsub callback / main thread under pcall → "attempt to yield across C-call boundary"; `lua_isyieldable` check fixes it. |
| RT-6 native C-stack overflow kills process/JVM | **confirmed** (minor nuance) | critical | gsub/sort/format-tostring/coroutine nesting crash 0xC00000FD at 256 KB/1 MB; JVM -Xss1m dies 0xC00000FD, no hs_err; Linux 8 MB segfault. Nuance: a memory cap *does* bound coroutine nesting if the native stack is ≥ ~0.55× the cap (≈270 B C stack vs ≈512 B heap per level), so "nothing but the native stack bounds it" holds only for an uncapped state; with CC:T's default 1 MB threads it still crashes. |
| RT-7 unprotected errors/OOM crash JVM; atpanic useless on Windows | **confirmed** | critical | 0xE24C4A02 natively, panic fn never called; JVM "Internal Error (0xe24c4a02)" + hs_err; Linux panic called then exit(1). Unprotected lua_gc at cap → 0xE24C4A04, but only when the GC must allocate (state-dependent). |
| RT-8 bytecode accepted; malformed bytecode corrupts memory | **confirmed** | critical | `load/loadstring/load(reader)` run `string.dump` output; mode `'t'` refuses; my fuzz: 245/1000 ACCESS_VIOLATION + 2 breakpoint + 11 hangs. Mod opens base+string and only nils collectgarbage/gcinfo/newproxy; CC:T 1.121 bios does not wrap `load` (and its `loadfile` passes `mode` through). |
| RT-9 `__gc` / `jit.attach` callbacks hang uninterruptibly | **partially confirmed** | high (critical only if exposed) | mechanism fully reproduced with entry markers (collectgarbage, allocation, lua_close, jit.attach → HUNG; 1 s finalizer delays interrupt 688–695 ms; errors swallowed). But the mod already sets `newproxy=nil` (main.cpp:734-735), never opens `jit`/`ffi`/`debug`, and LuaJIT ignores `__gc` on tables (verified), so stock CC user code cannot reach it unless the port widens the surface (e.g. exposes `debug`, see M-3). |
| RT-10 cap works but can wedge; uncapped state eats RAM | **confirmed** | high (wedge part: medium) | 32 MB cap: all OOMs catchable, state reusable; after coroutine OOM even `lua_cpcall(gc)` fails (48 B closure alloc), cap+1 MB recovers; uncapped `string.rep('x',2^31-1024)` succeeds (~4 GB GC count at peak). Allocator ratios differ from the claim (wrapper +2–5 %, msvcrt +59 %, +13.6 MB private after 300 cycles) but the direction holds. |
| RT-11 MinGW C++: Lua errors skip destructors and `catch(...)` | **confirmed** | high | Windows/MinGW g++ 16.2: destructor_ran=0, `catch(...)` → 0xE24C4A02, C++ throw under pcall → "C++ exception"; Linux g++ 15.2: destructors run, catch(...) catches (even inside a C function, which would *swallow* Lua errors on Linux). |

Missed issues (details in §3): **M-1** the mod never enables the JIT compiler (no `luaopen_jit`), both on 1.20.1_CCT and on the
"working" 1.14.4_CCT branch; **M-2** further unprotected `luaL_error` paths in handleEvent (`to_java_value` after `lua_resume`);
**M-3** if the port exposes LuaJIT's `debug` library (CC:T exposes a full one), `debug.setupvalue` on the mod's C closures makes
`invoke_java_fn` dereference NULL (crash, verified), and `debug.getupvalue/getregistry` hand user code the JavaFN userdata and its
metatable (re-opens RT-9); **M-4** 1.14.4's `abort()` is latched by `aborted` and is armed at most once per machine.

## 1. Setup

* LuaJIT: copied the shared prebuilt tree to `V\luajit`, verified the key sources are byte-identical to the mod's submodule
  (`cmp` of lj_record.c, lj_dispatch.c, lj_err.c, lj_gc.c, lj_state.c, lib_base.c, lj_api.c, luaconf.h, lib_string.c vs
  `vendor/luajit/src` at 19878ec0 → all "same"). Rebuilt: `mingw32-make clean; mingw32-make -j24 BUILDMODE=static` (log
  `V\build-static.log`). `luajit -v` = 2.1.ROLLING, JIT on, GC64 (default on x64).
* Linux: copied the tree to `V\luajit-linux`, built in WSL Ubuntu (gcc 15.2.0): `make clean; make -j24 BUILDMODE=static`
  (log `V\build-linux.log`).
* Toolchain: MinGW-w64 gcc/g++ 16.2.0 (WinLibs, SEH, MSVCRT); Temurin JDK 21.0.13; Python 3.13 (`python -I`).
* Harnesses (all mine): `V\v.c` (subcommands a/i/b/f/d/j/e), `V\gcv.c` (finalizer/attach with entry markers), `V\modlibs.c` (mod-style
  library opening), `V\ymod.c` (mod's exact yield-request hook), `V\coronest.c` (coroutine nesting vs cap), `V\mem2.c` (unprotected GC at
  cap, allocator options), `V\vcpp.cpp` (C++ interop), `V\lnx.c` (Linux counterparts), `V\dbgup.c` (debug-library vector),
  `V\jni\VJ.java` + `V\jni\vj.c` (real JVM), `V\scripts\fuzz1.lua` + `V\scripts\fuzzdrv.py` (bytecode fuzz), `V\runx.py` (prints full
  32-bit exit code). Async tests: the runner thread does `lua_resume` on a coroutine; the main thread sleeps 300 ms, calls
  `lua_sethook(co, hook, LUA_MASKCOUNT, 1)` and waits 2 s; on timeout it prints HUNG and `ExitProcess(3)`.

## 2. Per-claim evidence

### RT-1 — async interrupt of JIT code (confirmed, with caveat)

Source: `lua_sethook` (lj_dispatch.c:329-340) only stores hookf/hookmask/count, aborts recording and calls `lj_dispatch_update`; the
count hook is executed only from `lj_dispatch_ins` (lj_dispatch.c:429-432), i.e. from the interpreter's hooked dispatch table.
A looping trace never returns to the interpreter.

Commands (each case own process, 12 parallel): `V\v.exe a <shape> <cfg>`; results `V\res_a.txt`, raw `V\out_a1\*.txt`.

| shape | on | off | onperm (count hook every 1000 checking a flag) | offperm |
|---|---|---|---|---|
| while, fornum, repeat, goto, tab-alloc, luacall, sin, floor, pcall(g), coroutine.wrap gen, ipairs, pairs, tail recursion, **os.clock loop** | **HUNG** (hookcalls=0) | interrupted ≤0.015 ms | **HUNG** | interrupted |
| `'x'..i` concat | 0.109 ms | 0.005 | 10.6 ms | 0.003 |
| string.format | 0.149 ms | 0.008 | 16.2 ms | 0.018 |
| tostring({}) | 0.030 ms | 0.007 | 7.2 ms | 0.020 |

Counts: on 3/17, off 17/17, onperm 3/17, offperm 17/17. Even a loop calling a real C function (`os.clock`) hangs with the JIT on
(trace stitching runs it without passing through the hooked dispatch).

jit.opt variants (`PREFIX="jit.opt.start(...)" V\v2.exe a <shape> on`, `V\res_opt.txt`; note: a first attempt was invalid because
xargs stripped the quotes, rerun with `xargs -d '\n'`): `O0` 1/8 (tab), `-loop` 1/8 (tab), `maxside=0` 2/8 (pairs, ipairs),
`hotexit=1` 0/8, `hotloop=60000,hotexit=60000` 2/8 (pairs, ipairs). Matches the investigator's pattern.

JVM (`java -Xss1m -cp . VJ vj.dll abort`, `V\res_jni.txt`): Java thread runs `while true do end` via `lua_resume`, main thread calls the
native abort after 300 ms → "ABORT HUNG (worker still running after 3 s)"; with `jit.off()` → "status=2 interrupted, abort ok".
Linux (`V\res_linux.txt`): "abort JIT while-true: HUNG (fired=0)".

Caveat: the stock sources contain `LUAJIT_ENABLE_CHECKHOOK` (lj_record.c:2880-2899, "Use this only if you want to *asynchronously*
interrupt the execution"), so "impossible" is true for the default build configuration, not for LuaJIT as such. Also see M-1:
the mod never enabled the JIT, so the old mod was not affected; a port that turns the JIT on is.

### RT-3 — one-shot hook swallowed (confirmed)

`V\v.c` `hook_oneshot` is a copy of main.cpp:182-187 (clear hook, luaL_where, lua_error). `V\res_rt3.txt`:

```
a pcallspin  offoneshot  HUNG hookcalls=1
a pcallspin  offsticky   INTERRUPTED latency=0.007ms hookcalls=2 status=2 msg=interrupted after=ok
a xpcallspin offoneshot  HUNG hookcalls=1
a xpcallspin offsticky   INTERRUPTED latency=0.007ms hookcalls=2
a while      offoneshot  INTERRUPTED ... msg=[string "while true do end"]:1: aborted!
```

(`pcallspin` = `local function g() for i=1,1e7 do end end while true do pcall(g) end`.) The swallow is probabilistic in principle
(depends on the hook landing inside g) but g dominates the time. Context: CC:T's own soft abort is also catchable once
(CobaltLuaMachine.java:61-64) but CC:T backs it with a hard abort that throws `HardAbortError` on every interrupt check
(line 61). The 1.14.4 mod wires the hard abort (`ComputerExecutor.abort` → coremod → `LuaJITMachine.abort()`, cclj_txfm.js) to the
same one-shot hook, and `abort()` returns early once `aborted` is set (LuaJITMachine.java:139-142 on 1.14.4) — see M-4.

### RT-4 — count-hook time slicing (confirmed)

`V\v.exe i <bench> <cfg> <interval>`, 2 runs each, `V\res_i.txt`:

| bench | JIT no hook | JIT + hook 1e3/1e4/1e5 | hook calls (JIT) | interp no hook | interp + hook 1e3/1e4/1e5 | hook calls (1e3) |
|---|---|---|---|---|---|---|
| numloop 3e8 | 371 ms | 370/370/372 | 0 | 849 | 1133–1188 / 1117–1124 / 1136–1151 (+32…+40 %) | 900,000 |
| mandel 400² | 5.0 | 5.2–5.9/4.9–5.1/5.1–5.2 | 0–2 | 41.5 | 76–80 (+83…+92 %) | 60,290 |
| fib(32) | 14.8 | 14.4–14.7 | 0–4 | 64.5 | 73.4–75.5 (+14…+17 %) | 45,819 |

Same conclusion as the claim (the interpreted overhead is near-independent of the interval because any count hook switches the whole
dispatch table to the hooked path). My upper bound is +92 % rather than +85 %.

### RT-5 — yield from hook (confirmed)

`V\v.exe b <where> naive|checked <jit>` (`V\res_b.txt`, permanent yield hook every 1000):
plain/pcall body: interpreted 4000 yields, correct; JIT on: 0 yields (hook never runs). sort comparator and gsub callback with the naive
hook: status=2 "attempt to yield across C-call boundary" (both JIT modes); with `lua_isyieldable()` check: correct result
(sort: 36 yields + 3074 deferrals interpreted; gsub: 783 deferrals).

Mod's exact pattern (`V\ymod.c`, `V\res_ymod.txt`): a C function arms `thread_yield_request_handler_hook` (main.cpp:396-399, armed at
main.cpp:466) →

```
coroutine: yields=3 status=0 -> plain n=3
coroutine: yields=0 status=2 -> ...table.sort...: attempt to yield across C-call boundary
coroutine: yields=0 status=2 -> ...gsub...: attempt to yield across C-call boundary
main thread lua_pcall: status=2 -> ...: attempt to yield across C-call boundary
```

Source: lua_yield (lj_api.c:1204-1238) raises `LJ_ERR_CYIELD` when `!cframe_canyield`; `lua_isyieldable` = `cframe_canyield(L->cframe)`
(lj_api.c:1199-1202). Note: on Windows a hook yield is itself delivered by `lj_err_throw(L, LUA_YIELD)` (RaiseException).

### RT-6 — native C-stack overflow (confirmed, nuance)

`python -I runx.py V\v.exe f <case> <KB>` (thread created with `STACK_SIZE_PARAM_IS_A_RESERVATION`), `V\res_f.txt`:

| case | 256 KB | 1 MB | 16 MB | 256 MB |
|---|---|---|---|---|
| Lua recursion, `__index` recursion | stack overflow (caught) | same | same | same |
| `__tostring` → `string.format('%s')` recursion | **0xC00000FD** | **0xC00000FD** | caught | caught |
| gsub callback recursion | **0xC00000FD** | **0xC00000FD** | caught | caught |
| sort comparator recursion | **0xC00000FD** | **0xC00000FD** | caught | caught |
| coroutine.wrap nesting / coroutine.resume nesting | **0xC00000FD** | **0xC00000FD** | **0xC00000FD** | **0xC00000FD** |

Parser nesting (`(`, `{`, `do`, 1e5 deep): "chunk has too many syntax levels". LuaJIT has no C-call counter (grep finds no
`nCcalls`; only `LUAI_MAXSTACK` 65500 slots, luaconf.h:92, and `LJ_MAX_XLEVEL` 200, lj_def.h:65).

JVM (`V\res_jni.txt`): gsub recursion and coroutine nesting on a `new Thread()` with `-Xss1m` → process exit 0xC00000FD, no
hs_err file. Linux (`V\res_linux.txt`): gsub and coroutine nesting on the 8 MB main thread → SIGSEGV (rc 139). CC:T creates computer
threads with `new Thread(group, x)` (ThreadUtils.java:78), i.e. default stack size.

Nuance (`V\coronest.c`, `V\res_coronest.txt`): coroutine nesting under a capped allocator stops with catchable "not enough memory"
when the stack is big enough: 16 MB stack + 32 MB cap → OOM at depth 62,500; 16 MB + 8 MB → 15,500; 64 MB + 64 MB → 125,100.
So ≈512 B heap and ≤ 268 B native stack per level; a cap bounds nesting iff stack ≳ 0.55 × cap. With 1 MB stacks it crashed at caps
of 8 MB, 32 MB and uncapped. The claim's "only the native stack bounds it" is true for the uncapped state it tested.

### RT-7 — unprotected errors (confirmed)

Source: lj_err.c:377-390 `err_raise_ext` → `RaiseException`; panic is called only if that returns (lj_err.c:772-783), then `exit`.
`python -I runx.py V\v.exe d error|lerror|call` (panic installed via `lua_atpanic` + longjmp): all three
`EXIT=0xE24C4A02`, panic function never printed. JVM (`java -Xss1m -cp . VJ vj.dll unprot`, `V\jni\unprot_full.txt`): exit 1,
"A fatal error has been detected by the Java Runtime Environment: Internal Error (0xe24c4a02)", problematic frame
`KERNELBASE.dll+0xc408a`, `V\jni\hs_err_pid*.log` written; `catch (Throwable)` on the Java side did not help. Linux
(`V\res_linux.txt`): "PANIC fn called: boom", rc=1 (exit). Linux-in-JVM not tested (inferred: exit ends the JVM).

Unprotected GC at the cap (`V\mem2.exe unprotgc [strings]`, `V\res_mem2.txt`): fresh state → `lua_gc` returned (no allocation
needed); after a string-heavy workload (string table grown) → **EXIT=0xE24C4A04**. So the crash is state-dependent but real.

Mod: `handleEvent` calls `to_lua_values` (main.cpp:802) directly from JNI; `to_lua_value` raises `luaL_error` for unrecognised Java
values (main.cpp:308-323) — confirmed by reading. See M-2 for more such paths.

### RT-8 — bytecode (confirmed)

`V\luajit\src\luajit.exe V\scripts\g1.lua`:
```
load(dump)          true 21
loadstring(dump)    true 21
load(dump,'x','b')  true 21
load(dump,'x','t')  nil attempt to load chunk with wrong mode
load(reader)        true 21
load(reader,'t')    nil attempt to load chunk with wrong mode
after string.dump=nil: abab X 5 bc
```
Fuzz (`python -I V\scripts\fuzzdrv.py V\luajit\src\luajit.exe V\scripts\fuzz1.lua 1000`, header bytes kept, 3 random bytes
mutated, each case its own process, 5 s timeout; `V\res_gfuzz.txt`): LUAERR 315, **CRASH 0xC0000005 245**, RAN 232, REFUSED 195,
**HANG 11**, **CRASH 0x80000003 2**. (The investigator saw 94/1000 with a different function/mutation; same conclusion.)

Mod: main.cpp:707-711 opens base/math/string/table/bit; only `collectgarbage`, `gcinfo`, `newproxy` are nil'd (main.cpp:730-735);
`load`, `loadstring`, `loadfile`, `dofile`, `string.dump` remain; bios is loaded with `luaL_loadbuffer` (no mode, main.cpp:789).
CC:T 1.121 bios.lua: no wrapper of `load`; `loadfile` (bios.lua:423-439) passes the caller's `mode` straight to `load`, so
`loadfile(path, "b")` also loads bytecode from the computer's FS. LuaJIT's native loadfile/dofile (lib_base.c:380, 446) read the
host FS, but the bios replaces both globals before user code runs, and `package` is not opened by the mod, so the host-FS part is
not user-reachable in the mod's configuration (matches the claim's "when those libraries are opened" wording).

### RT-9 — `__gc` / `jit.attach` (partially confirmed)

Source: `gc_call_finalizer` → `hook_entergc` (lj_gc.c:512) sets `HOOK_ACTIVE|HOOK_GC`; `callhook` skips when `hook_active`
(lj_dispatch.c:362); lua_close runs finalizers after `hook_enter` (lj_state.c:336-347); vmevent callbacks run under
`hook_vmevent` (lj_vmevent.c:45); finalizer errors go to the ERRFIN vmevent (lj_gc.c:524-529).

`V\gcv.exe <case> <jit>` with a `mark()` C function proving the callback was entered (`V\res_gcv.txt`):

```
collect   jit=0/1  callback_entered=1  HUNG hookcalls=0
alloc2    jit=0/1  callback_entered=1  HUNG hookcalls=0     (finalizer triggered by normal allocation)
close     jit=0/1  callback_entered=1  HUNG                 (lua_close never returns)
finite    jit=0/1  callback_entered=1  returned, interrupt latency 688 / 695 ms (1 s finalizer)
attach    jit=1    callback_entered=1  HUNG ; attachctl (finite callback) returned 'done'
tablegc   ran=0    (__gc on tables ignored, 5.1 semantics) ; gcerr: error swallowed (only "ERROR in finalizer" on stderr)
```

Why only partial: the hazard requires user access to a `__gc`-bearing object or to `jit`. In the mod, `newproxy` is removed
(main.cpp:734-735), `jit`/`ffi`/`debug` are never opened, tables cannot carry `__gc`, and the only native userdata (JavaFN) is hidden in a
C-closure upvalue. So with the mod's library surface the vector is closed; it becomes critical only if the port exposes newproxy,
ffi, jit, or debug (M-3 shows how debug re-opens it). As a design rule ("keep these closed, protect native metatables") it is
correct. Also note an incidental pitfall when testing: a stale copy of the proxy in an unused stack slot can keep it alive (my first
`gc-alloc` variant never ran the finalizer for that reason).

### RT-10 — memory (confirmed; wedge part medium)

`V\v.exe e` (`V\res_e.txt`): `lua_newstate(custom)` non-NULL on GC64, 35,616 B after openlibs; 32 MB cap: table-of-tables,
`string.rep(2^30)`, string doubling, 1e8 unique strings → caught error "not enough memory" (luaL_dostring's `||` macro reports 1 = failure), peak 31 MB, reusable after GC. Wedge: OOM inside `lua_resume` (status 4), then `lua_cpcall(gc)` twice → status 4
("FAIL alloc o=0 n=48" = the cpcall C closure), `luaL_dostring("return 1")` fails, still fails after unref of the coroutine;
cap +1 MB → GC succeeded, used 32,768 KB → 545 KB, state reusable; `lua_close` → 0 bytes.
Uncapped: `string.rep('x', 2^31-1024)` succeeded in 1.08 s, `collectgarbage('count')` 4,194,343 KB at that point;
`string.rep('x', 2^31)` returns "" (length 0).
Allocators (`V\mem2.exe 0|1|2 300`, 300 create/run/close cycles; `V\res_mem2.txt`): lj_alloc 7.7–8.0 s; lj_alloc behind a
counting/capping wrapper (restored before `lua_close`, lj_state.c:232-233) 7.85–8.4 s (+2–5 %), +0.2 MB; msvcrt realloc
12.3–12.7 s (+59 %), +12.5…+13.6 MB private bytes. The claim's exact ratios (+35 % / 3.3×) are benchmark-specific; direction holds.

### RT-11 — MinGW C++ (confirmed)

`V\vcpp.exe raii|throw|innercatch|catchall` (`V\res_cpp.txt`) vs `vcpp_lnx` in WSL (`V\res_linux.txt`):

| case | Windows MinGW g++ 16.2 | Linux g++ 15.2 |
|---|---|---|
| luaL_error through a C++ frame with RAII guard, under pcall | pcall status 2, **destructor_ran=0** | destructor ran |
| C++ `throw` in lua_CFunction under pcall | "C++ exception", state usable | same |
| try/catch(...) around `lua_call` of an erroring chunk inside a C function | catch skipped, error reached outer pcall, destructor not run | **catch(...) caught it** (pcall status 0) |
| catch(...) around unprotected `lua_error` | **EXIT 0xE24C4A02** | caught, destructor ran |

Extra implication: on Linux a `catch(...)` in glue code will silently swallow Lua errors, including the interrupt error.

## 3. Missed issues

**M-1 (high): the mod never enables LuaJIT's JIT compiler.** `jit_init` (lib_jit.c:712-717, called only from `luaopen_jit`,
lib_jit.c:721-724) is the only place that sets `JIT_F_ON` and the JIT parameters; `lua_newstate` zeroes the whole `GG_State`
(lj_state.c:264). The mod opens only base/math/string/table/bit (1.20.1_CCT main.cpp:707-711; 1.14.4_CCT main.cpp:701-705) and has no
`luaopen_jit`, `luaL_openlibs` or `luaJIT_setmode` call. Verified (`V\modlibs.exe`, `V\res_modlibs.txt`): mod-style state runs a 3e8
numeric loop in 836 ms and the async abort of `while true do end` works; the same state plus `luaopen_jit` runs it in 366 ms and the
abort HUNG. So the "last working" 1.14.4 build ran LuaJIT as an interpreter only (this also explains why it never hit RT-1). A port
that wants JIT speed must call `luaopen_jit` (then hide the `jit` global) — which immediately makes RT-1 apply, so CHECKHOOK (or
equivalent) is mandatory together with it.

**M-2 (medium): more unprotected `luaL_error` paths in handleEvent.** After `lua_resume` returns, `handleEvent` converts yielded or
returned values and the error object with `to_java_values`/`to_java_value` (main.cpp:810, 817) outside any protected frame;
`to_java_value` calls `luaL_error` for functions, threads, userdata and light userdata (main.cpp:368-381). On Windows that is the
RT-7 JVM fatal error. Reachability from user code is unproven (CC:T's bios and `parallel` normally keep such values inside the main
routine), but it is the same pattern as RT-7 and must be removed in the port (do conversions under `lua_cpcall`, or type-check and
convert without raising). `assert(false)` for `IDynamicLuaObject` (main.cpp:291) is either a JVM `abort()` (assert on) or a silent
missing push (NDEBUG).

**M-3 (high if `debug` is exposed): LuaJIT's debug library reaches the mod's native internals.** CC:T exposes a full debug library
(Cobalt `CoreLibraries.debugGlobals` → DebugLib with getupvalue/setupvalue/getregistry/setmetatable/sethook/upvaluejoin …), so a
compatible port will be tempted to open LuaJIT's. With it (`V\dbgup.c`, which reproduces main.cpp:414-415/489-515, `V\res_dbgup.txt`):
`debug.getupvalue(apifn, 1)` returns the JavaFN userdata and its `__gc`, `debug.getregistry().JavaFN` returns the metatable
(re-opening RT-9), and `debug.setupvalue(apifn, 1, 'not a userdata'); apifn()` → `invoke_java_fn` dereferences the NULL from
`lua_touserdata` → **EXIT 0xC0000005** (JVM crash). The investigator's recommendation only mentions wrapping
sethook/gethook/setmetatable; getupvalue/setupvalue/upvaluejoin/getregistry (and getlocal/setlocal on C frames) must also be
filtered, and `invoke_java_fn` should use `luaL_checkudata`-style validation.

**M-4 (medium, Java glue on 1.14.4): abort is latched.** `LuaJITMachine.abort()` returns if `aborted` is already true and never
resets it (1.14.4 LuaJITMachine.java:139-142); `handleEvent` calls it on soft abort before entering Lua (line 196) and the coremod
routes the hard abort (`ComputerExecutor.abort`) to the same method (cclj_txfm.js "ComputerExecutor::abort"). Combined with RT-3, a
machine whose first abort was swallowed by `pcall` can never be aborted again.

## 4. Artifacts

* Sources: `V\v.c`, `V\gcv.c`, `V\modlibs.c`, `V\ymod.c`, `V\coronest.c`, `V\mem2.c`, `V\vcpp.cpp`, `V\lnx.c`, `V\dbgup.c`,
  `V\jni\vj.c`, `V\jni\VJ.java`, `V\scripts\g1.lua`, `V\scripts\fuzz1.lua`, `V\scripts\fuzzdrv.py`, `V\runx.py`.
* Results: `V\res_a.txt` (+`V\out_a1\`), `V\res_opt.txt`, `V\res_rt3.txt`, `V\res_i.txt`, `V\res_b.txt`, `V\res_ymod.txt`,
  `V\res_f.txt`, `V\res_coronest.txt`, `V\res_jni.txt`, `V\jni\unprot_full.txt`, `V\jni\hs_err_pid*.log`, `V\res_linux.txt`,
  `V\res_gfuzz.txt`, `V\res_gcv.txt` (`V\res_j.txt` is an earlier, partly ambiguous run superseded by res_gcv), `V\res_e.txt`,
  `V\res_mem2.txt`, `V\res_cpp.txt`, `V\res_modlibs.txt`, `V\res_dbgup.txt`.
* Builds: `V\luajit` (Windows static), `V\luajit-linux` (WSL static), logs `V\build-static.log`, `V\build-linux.log`.

## 5. Limits

Not re-tested: the CHECKHOOK build (not among my claims), the investigator's 22-shape list item by item (I used 17 shapes incl. 2 new
ones), Linux inside a JVM, arm64/macOS. Benchmarks ran on a shared machine (±30 % noise on absolute numbers).
