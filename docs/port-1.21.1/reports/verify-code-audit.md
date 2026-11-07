# Adversarial verification of the code-audit lane

Verifier key: `verify-code-audit`.
Work dir: `<scratch>\work\verify-code-audit` (abbreviated as `$V` below).
I did not modify the repository or the investigator's work dir. I read the investigator's harness only after my own runs, to explain why some of our results differed.

## 1. Summary

| ID | Verdict | Severity (mine) | One-line correction |
|---|---|---|---|
| CA-01 | confirmed (one example wrong) | critical | Verified on Windows (JVM fatal 0xE24C4A02) and Linux (`PANIC ...`, exit 1, Java shutdown hooks do **not** run). New triggers I verified: a HashMap null key and a NaN key. The exact player payload `os.pullEventRaw = function() return coroutine.yield(print) end` does **not** crash unmodified 1.14.4: handleEvent's off-by-one drops a single-value yield. `coroutine.yield(print, print)` does crash it (verified). |
| CA-02 | confirmed (threshold lower than stated) | critical | The C stack overflows at **5,000** nesting levels, not 20,000; 2,000 survives. This holds for both the API-call path and the yield path, and a `pcall` does not help. A Java Map 2,000 or 20,000 levels deep gives a StackOverflowError (not fatal), followed by 697 "JNI call made with exception pending" warnings. |
| CA-03 | confirmed, and **understated** | critical | **Lua→Java** cycle detection is broken too. `to_java_value` calls the 2-argument `table_to_map`, which creates a fresh in-progress table at every level. So a cyclic Lua table passed to *any* API function, or yielded once, kills the JVM with no round trip needed. The investigator's "round-trip" test actually died on the Lua→Java leg. A cyclic Java Map (Java→Lua) gave only a StackOverflowError in my runs. CallStaticIntMethod misuse confirmed by reading (dead code). |
| CA-04 | confirmed | critical | The 1.21.1 call chain is verified line by line. `close-while-running` killed the process 3/3 times, though here with STATUS_STACK_OVERFLOW 0xC00000FD (the investigator saw ACCESS_VIOLATION). `abort-after-close`: ACCESS_VIOLATION reading 0x10. Nuance: 1.14.4's coremod rewired the monitor's `close()` to `abort()`, so 1.14.4 mostly avoided cross-thread close. 1.21.1 has no such indirection. |
| CA-14 | confirmed | critical | In the exact CCLJ library set, `load`/`loadstring` accept `\27LJ` chunks by default. My own 2-byte KSTR patches segfault 3 of 5 variants (exit 139), and D=0x0001 already returns a type-confused "string". Mode `"t"` rejects them. RCE itself is not demonstrated; it is an inference from this memory-corruption class. |
| CA-05 | confirmed (with nuance) | high | `abort-pcall` was still running 3 s after the abort (verified). The latch and the shared hook slot are confirmed by reading. Nuance: Cobalt's *soft* abort is also one-shot and catchable by design. The real defect is that CCLJ has no uncatchable *hard* abort. LuaJIT documents `lua_sethook` as async-callable (from signals), so the cross-thread race is formally real but of limited practical impact while the JIT is off. |
| CA-06 | confirmed (now empirically) | high | C probe: a yield from the hook inside a child coroutine makes the parent's `coroutine.resume` return `true` with no values, leaving the child suspended mid-loop. Inside `table.sort` and `string.gsub` callbacks it raises "attempt to yield across C-call boundary". `CobaltLuaMachine.java:60-68` SUSPEND is confirmed. |
| CA-07 | confirmed | high | Reproduced exactly on Windows and Linux: `C2 A7 C2 95 41` (len 5), `byte[]{65,0,66}` arrives as len 1, and Lua→Java gives 4×U+FFFD. API echo of `a\200b` comes back as 5 bytes. |
| CA-08 | confirmed (all branches) | high | CCLJ init gave 200,000 hook hits in 0.235 s; with `luaopen_jit` via `lua_call`, 0 hits in 0.045 s. Benchmark 0.078 s vs 0.009 s. The 1.7.10, 1.12.2 and 1.14.4 branches all use the same init. Extra: `luaJIT_setmode(ENGINE\|ON)` alone does *not* give JIT speed, because `jit_init` never set the params. |
| CA-09 | confirmed (static) | high | Every signature and line reference matches CC:T 1.121.0 / 1.115.0 sources. |
| CA-15 | confirmed (conditional, as stated) | high | With the debug lib added: the JavaFN finalizer is callable from Lua, and the next call fails with "an unknown error occurred" plus "JNI call made with exception pending". `_PRELOAD.ffi` exists only with `luaL_openlibs`. In the CCLJ library set, `_PRELOAD` still holds `table.clear`, `string.buffer` and `table.new`. |
| CA-16 | confirmed (fix needs a caveat) | high (parity caveat) | A 1.5 GiB allocation succeeds with no cap. A custom allocator works on GC64. However, after a capped OOM the state is **not** usable: LuaJIT has no emergency GC, so every allocation keeps failing until the host calls `lua_gc(LUA_GCCOLLECT)`. CC:T/Cobalt also has no per-computer cap; the difference is the failure domain (native memory instead of the JVM heap). |
| CA-18 | confirmed (one detail wrong) | high | All environment facts reproduced in the exact CCLJ state. The prebios shifts bios line numbers by **4**, not 5 (`prebios.lua` has no trailing newline). |
| CA-19 | confirmed (static + 1.86.2 cross-check) | high | The typo, the lost wakeup and the swallowed InterruptedException are confirmed. The coremod injection precedes `if(!isOn) return;` in CC:T 1.86.2's `ComputerExecutor.queueEvent` (line 275). |
| CA-21 | partially confirmed | high | The temp-dir leak (verified), the NPE escaping `catch (IOException)` (verified), the hard-coded paths and the missing arch check all hold. **The macOS RTLD_LOCAL claim is wrong.** HotSpot BSD calls `dlopen(filename, RTLD_LAZY)`, and Apple's man page says the default is RTLD_GLOBAL, so `-undefined dynamic_lookup` would likely resolve. |
| CA-22 | confirmed (two details wrong) | high | The `assert` compile error is reproduced. With only `#include <cassert>` added it compiles: `<stdio.h>` already arrives through `jni.h:39`. The Makefile order drops DT_NEEDED only under `--as-needed` (Ubuntu gcc: verified). With `--no-as-needed`, as on the Fedora 30 / clang++ pipeline the Makefile was written for, NEEDED is present and it works. |

**Missed issues**

1. Lua→Java cycle detection is broken (critical). The details are under CA-03.
2. (high) Count-hook abort cannot interrupt long-running C library code, such as Lua-pattern backtracking in `string.find/match/gsub`. Cobalt checks interrupts inside its matcher (`StringMatch.java:513,528`). In a CCLJ state, a 0.81 s `string.find` fired **0** count hooks. The same one-liner with n=1200 ran for more than 2 minutes. That pins a Computer-Worker thread (CC:T's default `computer_threads` is 1), regardless of the JIT question.
3. (medium, design input for CA-16) LuaJIT has no emergency GC on allocation failure, so a capped state stays wedged until the host forces a full collection. CCLJ also removes `collectgarbage`.
4. (info) handleEvent's off-by-one hides some crash triggers. In practice, with CC's bios (whose first yield carries one value), the main routine's yields are never converted at all. This is why the single-value `yield(print)` example does not crash. Any fixed port will convert every yielded value, so the triggers become real.

## 2. Setup
- Windows: LuaJIT 2.1 ROLLING GC64, copied to `$V/luajit`. MinGW g++ 16.2. JDK 21.0.13. The JNI headers come from the JDK (`jdk-21/include`), not the vendored copy.
- `$V/src/main_1144.cpp` = `git show origin/1.14.4_CCT:src/main/cpp/main.cpp`, byte-identical to the investigator's copy (`cmp`). Built with `g++ -I<lj> -I<jdk>/include -I<jdk>/include/win32 -shared -Wl,--kill-at -o cclj_1144.dll main_1144.cpp -L<lj> -llua51`. Imports: KERNEL32, msvcrt, lua51.
- `$V/src/main_1144_dbg.cpp` adds one line, `luaopen_debug(L);`.
- My own harness: `$V/jni/src/vh/VMain.java`. It uses stub CC 1.86.2-style interfaces and a stand-in `LuaJITMachine` with the same natives, fields and `decodeString`. Each scenario runs in its own JVM, and handleEvent runs on a separate default-stack worker thread. Runner: `$V/jni/run.sh <dll> <test> [n]`. HotSpot logs: `$V/jni/logs/hs_err_*.log`.
- Linux: WSL Ubuntu 26.04 (glibc 2.43), g++, OpenJDK 17.0.20.1. LuaJIT was built from `git archive` of the `vendor/luajit` submodule (19878ec0) with `make BUILDMODE=dynamic`. Script: `$V/linux/build.sh`; runner: `$V/linux/run.sh`; output: `$V/linux/run_results.txt`.
- C probes: `$V/c/vprobe.c` (hook, bench, panic, stackgrow, alloc, unlimited, and a run mode that executes a Lua file in the **exact CCLJ library set** plus the `bit32 = bit` alias), `$V/c/resume_arith.c` (replicates handleEvent's stack arithmetic), `$V/c/hookyield.c` (CA-06), `$V/c/chook.c` (the C-function hook gap).
- Downloaded for cross-checks (data only): CC:T `v1.14.4-1.86.2` ComputerThread/ComputerExecutor sources and the jdk21u `os_bsd.cpp`/`os_linux.cpp`, in `$V/dl/`.

## 3. Results per claim

### CA-01: unprotected Lua errors kill the JVM
- I read main.cpp myself. 1.20.1 `handleEvent` is at :798-821: `to_lua_values` at :802 runs before `lua_resume` (:803), and `to_java_values`/`to_java_value` at :810/:817 run after it. There is no pcall/cpcall anywhere. Conversion errors come from `luaL_error` at :323 and :369-381. 1.14.4 is the same, with `luaL_error` at :322.
- Windows (`./run.sh cclj_1144.dll unknown-type`): a fatal error. `hs_err_unknown-type.log` shows `siginfo: EXCEPTION_?? (0xe24c4a02)`, native frames `KERNELBASE` then `lua51.dll+0x770a`, and Java frame `LuaJITMachine.handleEvent`.
- Linux: `PANIC: unprotected error in call to Lua API (bios.lua:1: Attempt to convert unrecognized Java value (ArrayList) to a Lua value!)`, exit=1. I added a JVM shutdown hook: it printed on a normal exit but **not** after the PANIC. So the world is not saved.
- New triggers I verified:
  - `nil-key` (HashMap with a null key): Windows fatal 0xe24c4a02; Linux `PANIC ... (table index is nil)`.
  - `nan-key` (a `Double.NaN` key): Windows fatal 0xe24c4a02.
- `vprobe panic` (`luaL_error` on a non-running coroutine) and `vprobe stackgrow 70000` both exit with **0xE24C4A02** (PowerShell `$LASTEXITCODE`). `stackgrow 1000` is fine.
- **The example payload is incorrect for the unmodified code.**
  - `resume_arith.c` shows that when the first yield carries one value, the handleEvent stack offset sticks at 1. After that, every single-value yield gives `nresults=0` and is never converted.
  - Harness `pullEventRaw-override`: a parallel-shaped main loop plus `os.pullEventRaw = function() return coroutine.yield(print) end`. The process **survived** (Windows).
  - `pullEventRaw-override2`, which yields `(print, print)`: JVM fatal 0xe24c4a02.
  - `yield-function-2` (the investigator's variant: an empty first yield, then `yield(print)`): fatal on Windows, PANIC on Linux.
  - So the class is confirmed, with a trivially adjusted payload.
- CC:T 1.21.1 `HttpRequest.java:230` queues `http_success` with an `HttpResponseHandle` (a plain `ObjectSource`), and `Websocket.java:235` queues a handle object. Both are unknown types for the old converter. `parallel.lua:103` calls the global `os.pullEventRaw()`.

### CA-02: unbounded native recursion
- Reading confirms recursion with no depth limit in `table_to_map` → `to_java_value` → `table_to_map` and in `map_to_table` → `to_lua_value` → `map_to_table`.
- `deep-lua N` (an empty first yield, then a table N levels deep is yielded): 2,000 OK; **5,000 and 20,000 give "An unrecoverable stack overflow has occurred"**.
- `deep-lua-api N` (`pcall(test.echo, t)`): 1,000 OK; **5,000 and 20,000 are fatal even inside pcall**.
- `deep-java 2000/20000`: a `StackOverflowError` thrown out of the native method; the process survives. Under `-Xcheck:jni` (`logs/deep-java-checkjni.txt`): 697× "JNI call made with exception pending", plus thousands of "without checking exceptions" warnings.
- CC:T 1.21.1 workers use `new Thread(group, x)` with the default stack (`ThreadUtils.java`, builder).

### CA-03: cycle detection
- Java→Lua: `to_lua_value` :288 calls the 4-argument `map_to_table`, which creates a new IdentityHashMap on every call (:216-219). The check at :190 can never hit. `CallStaticIntMethod(integer_class, intvalue_id, bidx)` at :192 uses `intvalue_id = GetMethodID(Number,"intValue")` (an instance method). That is UB in dead code. `Object[]` has no tracking.
- **Lua→Java (missed by the investigator):** `to_java_value`'s `LUA_TTABLE` case calls the 2-argument `table_to_map` (:367), which pushes a fresh `lua_newtable` as the in-progress table (:252-258). Nested tables therefore never see the parent's entries. Results:
  - `cyclic-lua-api` (`local t={} t.t=t pcall(test.echo,t)`): Windows fatal "unrecoverable stack overflow".
  - `cyclic-lua-yield` (yield `t` once): Windows `Internal Error (stackOverflow.cpp:239) ... not enough space to reguard`; Linux exit 139 (SIGSEGV).
  - The investigator's own `run_all.txt` for `cyclic-table-roundtrip` never printed "Lua->Java cyclic table converted to:", so their crash also happened on the first leg.
- `cyclic-java` (Map containing itself): a StackOverflowError escapes native code, and the JVM survives in my runs. `shared-java` (the same Map passed twice): `a == b` is false, so the sharing semantics differ from CC:T.

### CA-04: lifecycle and threads
- Code: 1.21.1 `ComputerThread.java:531-532` (`executor.timeout.hardAbort(); executor.worker.abortWithTimeout();`) → `ComputerExecutor.java:236-237` → `immediateFail` :254-256 → `machine.close()` on the monitor thread. Shutdown calls `machine.close()` at :430-433. Cobalt's own `close()` (CobaltLuaMachine) only sets `isDisposed` and calls `state.interrupt()`.
- `close-while-running` (`deinitMachine` from the main thread while the worker runs `while true do x=x+1 end`): the process died 3/3 times with **0xC00000FD (STATUS_STACK_OVERFLOW)**, and HotSpot wrote no log. The investigator saw ACCESS_VIOLATION; different UAF signatures are expected.
- `abort-after-close`: `EXCEPTION_ACCESS_VIOLATION reading address 0x10` in `lua51.dll+0xf754`.
- 1.14.4 context: the coremod (`cclj_txfm.js:87-121`) replaces the `machine.close()` in `ComputerExecutor.abort()` (CC:T 1.86.2 :253-256, called from the monitor at `ComputerThread.java:396`) with `LuaJITMachine.abort()`. So 1.14.4 avoided cross-thread close on that path, but still called `lua_sethook` cross-thread.

### CA-14: bytecode
- In the exact CCLJ library set (`vprobe run envprobe.lua`):
  - `string.dump` is present;
  - `load(binary)` returns "abc";
  - `loadstring(binary)` returns "abc";
  - `load(binary,'x','t')` gives "attempt to load chunk with wrong mode";
  - `loadfile`/`dofile` are present.
- KSTR opcode = 39 (from `jit.util.funcbc`). Stripped dump: `1b 4c 4a 02 0a ... 27 00 00 00 4c 00 02 00 ...`.
- My PoC `$V/c/bcpoc_tmpl.lua`, with variants `bc_<HI>_<LO>.lua`:

  | D operand | Result |
  |---|---|
  | 0x0001 | "string" type returned, then **segfault (139)** |
  | 0x0040 | nil returned |
  | 0xC000 | **segfault** |
  | 0xFFF0 | "string", then **segfault** |
  | 0x7FFF | nil |

- ROM: `shell.lua:163` calls `load(contents, "@/"..path, nil, env)`. Bios `loadfile` (:423-437) forwards `mode`. The bios defines no `load` wrapper. The program file is reconstructed as `readLine() .. "\n" .. readAll()`. In any case, user code can simply call `load(bytes)`.

### CA-05: abort
- Code: `thread_interrupt_hook` :182-187 clears the hook and raises an ordinary error. 1.14.4 Java `abort()` :139-143 is a never-reset latch, and soft abort is handled only at :195-196. Hooks live in `g->hookf` (`lj_dispatch.c:328-339`). `lj_trace_abort` is `G2J(g)->state &= ~LJ_TRACE_ACTIVE` (`lj_trace.h:45`).
- Harness results:
  - `abort-plain`: aborted (`[false, "bios.lua:1: aborted!"]`).
  - `abort-pcall`: **STILL RUNNING 3 s after abort**.
  - `abort-twice` (calling the native `abortMachine` directly twice): both work. So the second-runaway failure comes purely from the Java latch.
- Nuance:
  - The `lua_sethook` source comment says "This function can be called asynchronously (e.g. during a signal)."
  - Cobalt's soft abort is also thrown once and is catchable (`thrownSoftAbort`); only `HardAbortError` is uncatchable.
  - The defect, restated: CCLJ has no uncatchable hard abort. In 1.14.4 the monitor's repeated `executor.abort()` calls are swallowed by the latch.

### CA-06: hook-yield
`$V/c/hookyield.c`. The hook is the same as `thread_yield_request_handler_hook`, armed from a C function as `invoke_java_fn` does.
- Nested coroutine: `parent sees true` (no values), `child status suspended`; the main routine continues.
- `table.sort` comparator: `false ... attempt to yield across C-call boundary`. `string.gsub` callback: same.

### CA-07: strings
Windows and Linux are identical (`strings` test). `api-roundtrip`: Java got `a\uFFFDb`, Lua got back `a\uFFFD\uFFFD\uFFFDb` (5 bytes), and the error text is `[C]:-1: boom \uFFFD\uFFFD from java`. Cobalt `LuaString.java:520-546` maps one byte to one char, with `c<256 ? c : '?'`.

### CA-08: JIT never enabled
- `vprobe hook`:

  | Init | Hook hits | Time | `jit` global |
  |---|---|---|---|
  | CCLJ | 200,000 | 0.235 s | nil |
  | + `luaopen_jit` via `lua_call` | 0 | 0.045 s | table |
  | + `luaJIT_setmode` only | 200,000 | 0.233 s | – |

- `vprobe bench`: 0.078 s / 0.009 s / 0.078 s, in the same order.
- `JIT_F_ON` is set only in `lib_jit.c:715` (`jit_init`), apart from setmode, which leaves `J->param` zeroed.
- All branches open only base/math/string/table/bit (1.7.10 :692-696, 1.12.2 :692-696, 1.14.4 :701-705, 1.20.1 :707-711).

### CA-09: obsolete API model
- 1.21.1 `IDynamicLuaObject.java:38` is `MethodResult callMethod(ILuaContext, int, IArguments)`.
- `ILuaAPI.java` has only `getNames`/`getModuleName`/`startup`/`update`/`shutdown`.
- The 1.20.1 `LuaJITMachine` implements only `ILuaMachine`.
- `invoke_java_fn` :417-418 passes `Object[]` and casts the result to `jobjectArray`. It is dead code, because `registerAPI`'s body is commented out (:760-777).

### CA-15: debug/registry
- ROM: `exception.lua:90,101` (needs a table from `debug.getregistry()`), `require.lua:134` (tolerates nil), `error_hints.lua:124`.
- `gc-upvalue` (debug DLL): `upvalue type userdata`, `finalizer call true`, then `call after finalize false [C]:-1: an unknown error occurred`. With `-Xcheck:jni`: "JNI call made with exception pending".
- `registry-ffi` (debug DLL, CCLJ library set): `PRELOAD=table.clear,string.buffer,table.new`; `LOADED=string,debug,coroutine,bit,table,_G,math`; the JavaFN metatable is reachable.
- `luajit.exe` (`luaL_openlibs`): `_PRELOAD` = ffi, jit.profile, jit.util, string.buffer, table.clear, table.new.
- I checked a candidate escalation: LuaJIT 19878ec0 `lj_gc.c:520-529` swallows finalizer errors (`ERRFIN` vmevent), so a double finalize does *not* produce an unprotected error later.

### CA-16: memory
- `vprobe unlimited`: holding 1,536 strings of 1 MiB each succeeds with `luaL_newstate`.
- `vprobe alloc` (32 MiB cap): the hog gets "not enough memory". The 1 immediate and 3 further retries all get **"not enough memory"**. `used` was 32.0 MiB before and 1.0 MiB after `lua_gc(LUA_GCCOLLECT)`, after which the state works. After `lua_close`: 0 bytes.

### CA-18: environment
`envprobe.lua` in the CCLJ library set:
- `_VERSION` = Lua 5.1;
- `table.pack`/`table.unpack` = nil;
- `utf8`/`debug`/`jit` = nil;
- `bit32.bnot(0)` = -1, `band(0xFFFFFFFF,0xFFFFFFFF)` = -1, `lshift(1,32)` = 1, `rshift(-1,0)` = -1;
- `extract`/`replace`/`btest`/`lrotate`/`rrotate` = nil;
- `load("local _ENV = {x=42} return x")` returns nil.

ROM: `bios.lua:42,46` (identical in 1.20.1 and 1.21.1), `textutils.lua:436`, and 15 files containing `_ENV`. `prebios.lua` is 4 lines and ends without a newline, so `pre + "\n" + bios` puts bios line 1 at line 5, a shift of 4.

### CA-19: 1.14.4 blocking yield
Lines verified:
- `isSpecialEvent` :40 matches "task_completed";
- `executeMainThreadTask` :256 calls `pullEvent("task_complete")`;
- `taskCompleted` :298 queues "task_complete";
- `containsKey` is checked at :231, outside the `synchronized` block at :239-241;
- InterruptedException is swallowed at :242-244.

The coremod inserts at instruction 0 of `ComputerExecutor.queueEvent`. CC:T 1.86.2 `ComputerExecutor.java:272-287` starts with `if(!isOn) return;`. `machine` is null while the computer is off, which means an NPE on the queuing thread.

### CA-21: loader
- `$V/jni/extract/VExtract.java`: `new GZIPInputStream(null)` throws `NullPointerException`, which is not an IOException. Temp dir plus `deleteOnExit` plus `System.load`: after the JVM exits, `dir exists=true, dll exists=true` (removed by hand afterwards).
- `OS.java` checks only `os.name`.
- macOS:
  - jdk21u `os_bsd.cpp:997` calls `::dlopen(filename, RTLD_LAZY)`.
  - Apple dlopen(3) says: "If neither RTLD_GLOBAL nor RTLD_LOCAL is specified, the default is RTLD_GLOBAL."
  - So the "HotSpot RTLD_LOCAL" reasoning is wrong for macOS; it is true on Linux (`os_linux.cpp:1845`, glibc default RTLD_LOCAL).
  - macOS was never built, so this is moot except as a design note.
- Name collision with another mod's `lua51.dll`/`libluajit-5.1.so.2`: plausible, not tested.

### CA-22: build
- `g++ main_1201.cpp` gives `error: 'assert' was not declared in this scope` at :291. With only `#include <cassert>` prepended it compiles with no errors. `jni.h:39` (both the JDK copy and `vendor/jni-headers`) includes `<stdio.h>`.
- WSL Ubuntu, `readelf -d`:

  | Build | NEEDED |
  |---|---|
  | Makefile order with `-fPIE` | only `libc.so.6` |
  | Libraries after the sources, `-fPIC` | `libluajit-5.1.so.2` |
  | Makefile order with `-Wl,--no-as-needed` | `libluajit-5.1.so.2` |

  The JVM with the Makefile-order .so: `symbol lookup error: ... undefined symbol: luaL_newstate`, exit 127. The `--no-as-needed` build runs the strings test fine.
- The Makefile's Linux `CC` is `clang++` on `fedora:30`, which does not default to `--as-needed`, so the original pipeline was not affected. Ubuntu clang's default is untested (clang is not installed).
- 1.14.4 build.gradle :3 `jcenter()`, :11 ForgeGradle 3.+, :98-101 `processResources.dependsOn buildDynamicLibrary` running `./build_natives.sh` (Docker). jcenter is sunset/read-only rather than strictly dead.
- Dockerfile: `FROM fedora:30`, with the SDK from `github.com/phracker/MacOSX-SDKs`.

## 4. Missed issue detail: hooks do not fire inside C library functions
`$V/c/chook.c` (CCLJ library set, count hook every 1000 instructions):
- `string.rep('a',150):find(('a*'):rep(3)..'b')`: 0.06 s, 0 hook hits.
- n=300: 0.81 s, **0 hook hits**.
- n=1200: did not finish within 120 s (killed by `timeout`).

Cobalt checks `state.isInterrupted()` inside its pattern matcher (`StringMatch.java:513,528`). CCLJ's abort and pause both depend on hooks. A single pattern call can therefore pin a worker for minutes to hours, whatever the JIT setting. In 1.21.1 the monitor eventually abandons the thread and spawns a new worker; the old one keeps burning a core, and a later `close()` from the monitor hits CA-04. A port needs its own interrupt checks in a patched LuaJIT `lj_str`/`lib_string` matcher, or must replace `string.find/match/gmatch/gsub`.

## 5. Artifacts
- `$V/jni/src/vh/VMain.java`, `$V/jni/run.sh`, `$V/jni/logs/*` (hs_err logs, `deep-java-checkjni.txt`, `gc-upvalue-checkjni.txt`).
- `$V/src/main_1144.cpp` (unmodified), `cclj_1144.dll`, `main_1144_dbg.cpp`/`cclj_1144_dbg.dll`, `main_1201.cpp` (unmodified), `main_1201_cassert.cpp`.
- `$V/c/vprobe.c`, `envprobe.lua`, `bcpoc_tmpl.lua`, `bc_*.lua`, `findop.lua`, `resume_arith.c`, `hookyield.c`, `chook.c`.
- `$V/linux/build.sh`, `run.sh`, `run_results.txt`, `cclj_{makefile,fixed,noasneeded}.so`.
- `$V/jni/extract/VExtract.java`; `$V/dl/` (reference sources).
