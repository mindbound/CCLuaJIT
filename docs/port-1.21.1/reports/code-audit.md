# CCLuaJIT code audit (lane: code-audit)

Scope: a defect and security audit of the existing CCLuaJIT code. The goal is to decide what can be reused in a 1.21.1 rewrite and which traps the rewrite must avoid.

- Branch `1.20.1_CCT` (HEAD 505b1f9): `src/main/cpp/main.cpp` (830 lines), Makefile, build.sh, `LuaJITMachine.java`, `CCLuaJIT.java`, build.gradle, gradle.properties, fabric.mod.json, mixins json.
- Branch `origin/1.14.4_CCT` (last plausibly working commit 3d27adc, "Update things to actually work.", 2024-04-25): LuaJITMachine.java, CCLuaJIT.java, OS.java, cclj_txfm.js (coremod), prebios.lua, Dockerfile, build_natives.sh, build.gradle, mods.toml, and main.cpp (diffed against 1.20.1).
- 1.12.2 and 1.7.10 were skimmed for context only.

The repository was not modified. All work happened in
`<scratch>\work\code-audit`
(abbreviated as `$W` below).

---

## 1. Bottom line

1. **The existing native glue is not safe to reuse as is.** In the unmodified 1.14.4 `main.cpp`, ordinary inputs crash or kill the whole server JVM. I verified 7 distinct triggers on Windows and confirmed the main ones on Linux:
   - an unknown Java type in an event;
   - a function value yielded to the top level;
   - a cyclic Map or table round trip;
   - a nested table about 20,000 levels deep;
   - `close()` called from another thread while Lua runs;
   - `abort()` called after `close()`;
   - Lua stack growth past 65,500 slots outside a protected call.

   None of these needs special privileges. Ordinary Lua code on any computer can trigger them, and several match normal CC:T 1.21.1 traffic exactly. For example, every `http_success` event carries a handle object, and `fs.read` returns a `ByteBuffer`.
2. **The original design never turned on the JIT.** `initMachine` opens base/math/string/table/bit by calling `luaopen_*` directly and never calls `luaopen_jit`. In LuaJIT, the compiler is enabled only inside `jit_init()`, which `luaopen_jit` runs (`lib_jit.c:712-724`). I measured the effect with CCLuaJIT's exact init sequence:
   - Count hooks fire normally (200,000 hits versus 0 with the JIT on).
   - A float/array benchmark is about 10x slower than with the JIT on (0.78 s versus 0.08 s).

   So the 1.14.4 mod, when it worked, delivered LuaJIT interpreter speed only. That is also why its hook-based abort could work at all. This is a key decision input that connects to the preemption lane.
3. **Strings are converted with the wrong encoding in both directions** (verified). Lua to Java uses US-ASCII, so bytes ≥ 0x80 become U+FFFD. Java to Lua uses JNI modified UTF-8, so U+0080..U+00FF become 2 bytes. `byte[]` is truncated at the first NUL. CC:T's rule is 1:1 Latin-1 (`LuaString.java:520-546`). This breaks CC's teletext and graphics characters 128-159, Latin-1 text, and all binary data.
4. **Security:** LuaJIT runs untrusted bytecode. A 2-byte patch to a `string.dump` result segfaults the interpreter (verified). The CC:T 1.21.1 ROM's `shell.lua:163` loads every program with `load(contents, name, nil, env)`, which uses mode "bt". The ROM also requires `debug.getregistry()` and `debug.getupvalue()`. A naive port that opens the real debug library or `luaL_openlibs` therefore hands players `registry._PRELOAD.ffi` (verified present) and the internal JavaFN metatables (verified: Lua code can run the native finalizer). There is no memory cap. A cap is feasible: `lua_newstate` with a custom allocator works on this GC64 build (verified).
5. **The 1.14.4 runtime design is obsolete and harmful.** It parks the computer thread inside `Object.wait()` during a native call. It has a lost-wakeup race and swallows InterruptedException. A `"task_completed"` versus `"task_complete"` typo makes every main-thread task deadlock. The coremod bytecode-patches ComputerExecutor. In 1.21.1, `ServerContext.luaMachine` (a public static field) and `MethodResult`/`ILuaCallback` replace all of this. Do not port it.
6. **The 1.20.1 branch does not build and does not run.**
   - `main.cpp` fails to compile: `assert` is used without `<cassert>` (verified with g++ 16.2).
   - No native methods are declared in Java.
   - `System.load` uses hard-coded paths from the original author's Linux machine.
   - The constructor is empty and `handleEvent` returns null, which causes an NPE in `ComputerExecutor.resumeMachine`.
   - The Makefile's flag order produces, on Debian or Ubuntu toolchains, a `cclj.so` with no dependency on libluajit. The JVM then dies with `undefined symbol: luaL_newstate` (verified in WSL Ubuntu).

**Salvage verdict:** reuse ideas and a few small patterns, roughly 10% of lines or less. Rewrite the native layer, the Java machine, the loader, and the build. See section 6.

---

## 2. Methodology and experiments

Tools: MinGW-w64 g++ 16.2 (WinLibs), the prebuilt LuaJIT 2.1 ROLLING (GC64, copied to `$W/lj`), Temurin JDK 21 on Windows, WSL Ubuntu with glibc 2.43, g++, and JDK 17. LuaJIT for Linux was built from the repo's `vendor/luajit` submodule (19878ec0) using `git archive` into `$W/linux/luajit`. The repo was not touched.

### 2.1 JNI harness that drives the original native code
- `$W/src/main_1144.cpp`: byte-identical copy of `origin/1.14.4_CCT:src/main/cpp/main.cpp`, built with the Makefile-equivalent command `g++ -I../lj -Ijni-headers -Ijni-headers/win32 -shared -o cclj_1144.dll main_1144.cpp -L../lj -llua51`. It builds cleanly. Its imports are only KERNEL32.dll, msvcrt.dll, and lua51.dll (checked with `objdump -p`), so there is no libstdc++ or libgcc DLL dependency.
- `$W/jni/src/...`: stub CC:T 1.86.2 API interfaces (`ILuaAPI`, `ILuaObject`, `ILuaContext`, `LuaException`). Also a stand-in `gay.vereena.cclj.computer.LuaJITMachine` with the same field names and types and the same native signatures as the 1.14.4 class, plus `harness/Main.java` holding one test per crash scenario.
- `$W/jni/run_all.sh` reruns every test in its own JVM. Output is in `$W/jni/logs/run_all.txt`, and the HotSpot crash logs are in `$W/jni/logs/hs_err_*.log`.
- `$W/src/main_1144_debug.cpp` is an audit variant that adds one line, `luaopen_debug(L);`, to simulate what a 1.21.1 port must expose.
- On Linux (WSL): `$W/linux/run.sh`, `$W/linux/cclj_makefile.so` (exact Makefile flags), and `$W/linux/cclj_fixed.so` (libraries placed after the sources, `-fPIC`).

Results from `run_all.sh` (Windows, JDK 21) and Linux:

| Test | What it does | Windows result | Linux result |
|---|---|---|---|
| strings | `"\u00A7\u0095A"` and `byte[]{65,0,66}` Java→Lua; `"\128\149\167\255"` Lua→Java | Lua sees `C2 A7 C2 95 41` (len 5) and `41` (len 1); Java sees 4×U+FFFD | identical |
| first-yield-filter | bios yields `'timer'`, then `'key'` | both filter values lost (results `[]`) | – |
| bios-error | bios raises an error, then the next event arrives | `[false,"bios.lua:1: bios died"]`, then `[false,"cannot resume non-suspended coroutine"]` on every later event | – |
| api-call | API echo of `'a\200b'`; Java exception message containing `§` | Java received `a\uFFFDb`; Lua got back 5 bytes; error text `"[C]:-1: boom from java \uFFFD\uFFFD"` | – |
| unknown-java-type | event argument is an `ArrayList` | **JVM fatal error**, unhandled SEH code `0xE24C4A02` (LuaJIT error, LUA_ERRRUN) | **`PANIC: unprotected error in call to Lua API (bios.lua:1: Attempt to convert unrecognized Java value (ArrayList)...)`**, exit code 1 |
| yield-function | main routine yields `print` | **JVM fatal error** | – |
| cyclic-map | `Map m; m.put("self", m)` as an event argument | `StackOverflowError` thrown out of the native code, with JNI calls continuing while an exception is pending | – |
| cyclic-table-roundtrip | `local t={} t.t=t` yielded to Java, then fed back | **JVM fatal error: "An unrecoverable stack overflow has occurred"** | **Segmentation fault** |
| deep-table N | `{ {...} }` nested N deep, Lua→Java then Java→Lua | N=40 OK; N=2000: StackOverflowError (Java→Lua); **N=20,000 / 50,000 / 200,000: JVM fatal stack overflow** (Lua→Java) | – |
| abort-plain | `while true do end`, then `abortMachine()` from another thread | aborted OK (the hook fires because the JIT is off) | – |
| abort-pcall | `while true do pcall(function() while true do end end) end`, then abort | **still running 3 s after abort** (one-shot hook defeated) | – |
| close-while-running | `deinitMachine()` (lua_close) from a 2nd thread while Lua runs | **3/3 EXCEPTION_ACCESS_VIOLATION** in lua51.dll | – |
| abort-after-close | `abortMachine()` after `deinitMachine()` | **ACCESS_VIOLATION reading 0x10** (`lua_sethook(NULL)`) | – |
| gc-upvalue (debug variant) | `debug.getupvalue(test.echo,1)`, then `getmetatable(ud).__gc(ud)`, then `test.echo()` | the finalizer can be called from Lua; the next call runs with deleted global refs (JNI UB; HotSpot happened to raise NPE, giving "an unknown error occurred") | – |
| `-Xcheck:jni` (api-call, gc-upvalue) | | warnings: "JNI call made with exception pending" and "without checking exceptions" (CallObjectMethodV, CallStaticObjectMethodV) | – |
| Makefile-built .so (Linux) | exact Makefile flags, `-l:libluajit-5.1.so` before `main.cpp` | – | `readelf`: only NEEDED is libc.so.6; 45 undefined `lua*` symbols; **JVM: `symbol lookup error: ... undefined symbol: luaL_newstate`, exit 127** |

### 2.2 C probes against LuaJIT (`$W/c/ljprobe.c`, `ljprobe.exe`)
`gcc -O2 -I../lj -o ljprobe.exe ljprobe.c -L../lj -llua51`
- `ljprobe hook` with CCLuaJIT's exact library set (`luaL_newstate` plus direct calls to luaopen_base/math/string/table/bit):
  - JIT off: a 1e8-iteration loop under a count hook (every 1000 instructions) hit the hook **200,000** times in 0.24 s.
  - The same state plus `luaopen_jit` via `lua_call`: hook hit **0** times in 0.04 s.
- `ljprobe bench` (array/float loop): **0.78 s** with the CCLJ init versus **0.08 s** with the JIT on.
- `ljprobe jit` (2e8 `x = x + i % 7`): 0.57 s versus 0.26 s. The CCLJ state has no global `jit`.
- `ljprobe panic`: `luaL_error` on a coroutine outside resume/pcall. On Windows the process dies with exit code `0xE24C4A02` and no PANIC text. The SEH exception is unhandled; on Linux it would reach `panic()` and `exit(EXIT_FAILURE)` (`lj_err.c:768-798`).
- `ljprobe stackgrow 1000`: OK, because LuaJIT `lua_push*` auto-grows the stack (`lj_state.h:11-12`, `incr_top`). `stackgrow 70000` raises "stack overflow" without protection, and the process dies with `0xE24C4A02`.
- `ljprobe alloc`: `lua_newstate(cap_alloc)` works on this GC64 build. A 32 MiB cap stops a memory hog with "not enough memory" (peak 28.7 MiB). The state stays usable, and usage is 0 bytes after `lua_close`.

### 2.3 Lua probes (`luajit.exe`)
- `$W/c/probe.lua` prints:
  - `table.pack nil`, `table.unpack nil`;
  - `bit.bnot(0) = -1` (bit32: 4294967295), `bit.band(0xFFFFFFFF,0xFFFFFFFF) = -1`, `bit.rshift(-1,0) = -1`, `bit.lshift(1,32) = 1` (bit32: 0);
  - `bit.extract/replace/btest/lrotate = nil`;
  - `_ENV` not honoured;
  - `debug.getupvalue(coroutine.wrap(f),1)` returns the internal thread upvalue of a C closure;
  - `load(s, n, "t", env)` works, so mode and env are supported;
  - `load(binary)` with the default mode works;
  - `utf8 = nil`.
- `$W/c/bytecode_poc.lua` and the `bc_0x*.lua` variants patch the `KSTR` operand of a dumped function. With D-high bytes 0xC0 and 0xFF, **luajit.exe segfaults (exit 139)**. With 0xFF, a fake "string" object is returned first. This shows the type confusion and out-of-bounds read that come from unverified bytecode.
- Run as `luajit.exe` (all libraries open), `debug.getregistry()._PRELOAD` contains **`ffi`**, `jit.util`, `jit.profile`, `string.buffer`, `table.new`, and `table.clear`.
- Recursion limits: plain Lua recursion gives "stack overflow" (handled), the parser gives "chunk has too many syntax levels" (200). Pure Lua cannot reach a C-stack overflow in LuaJIT. Only the glue's own recursion can.

### 2.4 Other checks
- `$W/jni/extract/ExtractTest.java` mimics `loadNatives()` from 1.14.4 (temp dir plus `deleteOnExit` plus `System.load`). After the JVM exits, **both the DLL and the dir still exist on Windows**, because a loaded DLL cannot be deleted. The test leftover was removed afterwards.
- `g++ ... main_1201.cpp` gives `error: 'assert' was not declared in this scope` at line 291. It builds once `<assert.h>` and `<stdio.h>` are added (`$W/src/main_1201.cpp` is the patched copy).

---

## 3. Findings

Severity scale: critical, high, medium, low, info. "Carry-over" says whether the risk comes back if the code or pattern is reused in 1.21.1.

### A. Process-killing failure modes

**CA-01 (critical, verified Win+Linux): Lua errors raised outside any protected call terminate the JVM.**
- Where: 1.20.1 `main.cpp:798-821` (`handleEvent`). It calls `to_lua_values` at :802 before `lua_resume`, and `to_java_value(s)` at :810/:817 after it returns, both on a coroutine that is not running. The error sources are:
  - `to_lua_value` `luaL_error` at :323 for unknown types;
  - `to_java_value` `luaL_error` at :369-381 for functions, threads, and userdata;
  - `lua_settable` with a nil key (Java `HashMap` with a null key) or a NaN key at :212;
  - stack growth beyond `LUAI_MAXSTACK` = 65500;
  - OOM.

  1.14.4 has the same code at nearly identical lines, for example :322.
- Failure scenarios in 1.21.1 terms:
  - Any event argument the converter does not know is fatal. That includes the `http_success`, `http_failure`, and `websocket_success` handle objects (plain objects with `@LuaFunction` methods), `ByteBuffer`, `List`/`Collection`, `ILuaFunction`, and enums.
  - A program can kill the server with `os.pullEventRaw = function() return coroutine.yield(print) end`. The bios's `parallel` loop then yields a function from the main routine. CC:T's `bios.lua:38-40` defines `os.pullEventRaw`, and the `os` table is writable.
- What happens: on Windows, HotSpot reports a fatal error (`EXCEPTION 0xe24c4a02`, `$W/jni/logs/hs_err_unknown-java-type.log`). On Linux, `PANIC: unprotected error in call to Lua API (...)` is followed by `exit(1)`. The server dies without a normal stop. My understanding is that libc `exit()` from native code skips Java shutdown hooks, so the world is not saved (medium confidence on the hooks detail).
- Also: `assert(false)` for `IDynamicLuaObject` (1.20.1 :291) calls `abort()` in any non-NDEBUG build.
- Carry-over: **yes**, it is a pattern. Fix:
  - Do every Lua-touching step of a JNI entry point inside `lua_pcall`/`lua_cpcall`, or a `lua_pushcfunction` trampoline plus `lua_pcall`.
  - Never call `luaL_error` in conversion code that may run outside a protected frame.
  - Map unknown or unconvertible values to nil, the same way `CobaltLuaMachine.toValue` and `toObject` do (CC:T 1.121.0 `CobaltLuaMachine.java:177-319`).

**CA-02 (critical, verified): Unbounded native recursion in the converters overflows the C stack.**
- Where: `table_to_map` (1.20.1 :221-250) and `to_java_value` (:336-386) on the Lua→Java side; `map_to_table` (:189-219) and `to_lua_value` (:261-325) on the Java→Lua side. Both directions recurse with no depth limit, on the computer worker thread, which uses the JVM default stack (about 1 MiB; `ComputerThread` uses a default `ThreadFactory`).
- Scenario: `local t={} for i=1,20000 do t={t} end os.queueEvent("x", t)`, or any API call that takes a table argument, crashes the JVM ("An unrecoverable stack overflow has occurred"). From Java, a Map nested about 2,000 deep already throws StackOverflowError, and the native code then keeps making JNI calls while that exception is pending.
- Carry-over: yes. Fix: convert iteratively with an explicit work stack, or cap the depth (for example 128) and map deeper values to nil or an error *inside a pcall*. Also call `lua_checkstack` per level.

**CA-03 (critical, verified): Java→Lua cycle detection is broken, so cyclic data recurses forever. There is also a JNI call-variant bug.**
- Where:
  - 1.20.1/1.14.4 `main.cpp:288`: `to_lua_value` calls the 4-argument `map_to_table`, which creates a **fresh** `IdentityHashMap` on every call (:216-219). The "already converted" check at :190-195 therefore never fires for nested maps.
  - The dead branch also contains `env->CallStaticIntMethod(integer_class, intvalue_id, bidx)` (:192). `intvalue_id` is the *instance* method `Number.intValue`, called with a static variant. That is undefined behavior.
  - `Object[]` (:292-307) has no cycle tracking at all.
- Scenario: a self-referencing Map, or a cyclic Lua table sent to Java and back (rednet/modem payloads, `os.queueEvent`), recurses forever. Windows reports a fatal stack overflow; Linux gets SIGSEGV.
- Carry-over: yes if the converter is reused. Fix: one identity map per top-level conversion, keyed by Java identity and mapping to a registry or stack slot. Mirror CC:T's semantics: shared objects map to the same table, and singleton maps get fresh tables.

**CA-04 (critical, verified): The machine lifecycle is not thread-safe. close or abort from another thread is use-after-free.**
- Where: 1.20.1 `deinitMachine` :750-757 (`lua_close` with no synchronization), `abortMachine` :823-826 (`lua_sethook` on `main_routine`, which may already be 0 or freed), and 1.14.4 `LuaJITMachine.java:139-143, 213-217`.
- What happens in 1.21.1: CC:T calls `ILuaMachine.close()` from the **Computer-Monitor thread** while a worker thread is still inside `handleEvent` (`ComputerThread.checkRunners` :531-532 calls `abortWithTimeout`, which calls `immediateFail` in `ComputerExecutor.java:254-256`, which calls `machine.close()`). It calls `close()` again from the worker on shutdown (:430-433).
- Evidence: in the harness, `deinitMachine()` from a second thread while Lua ran gave an **access violation 3/3 times**. `abortMachine()` after `deinitMachine()` read 0x10 and crashed.
- Carry-over: **yes, this is the most likely trap for a naive port**. Fix:
  - `close()` must only set an atomic "disposed" flag and request an interrupt.
  - The `lua_State` may be freed only on the thread that owns it, once no resume is active. For example: at the end of `handleEvent`, on the next call, or through a `Cleaner` guarded by a lock.
  - Every native entry point needs an "alive" check under the same lock or state machine.
  - `printExecutionState` (called from the monitor thread) must never touch the Lua state.

### B. Interruption, timeout and scheduling

**CA-05 (high, verified): The abort mechanism is one-shot and catchable, and it races with other hook users.**
- Where:
  - `thread_interrupt_hook` (1.20.1 :182-187) clears the hook (`lua_sethook(L,0,0,0)`) and then raises an ordinary `lua_error`.
  - `abortMachine` (:823-826) arms it from a foreign thread.
  - The yield-request hook (:396-399, :464-467) uses the **same single global hook slot**. LuaJIT hooks are per-`global_State` (`lj_dispatch.c:329-340`), so the two clobber each other.
  - Calling `lua_sethook` from another thread is a data race: it writes `g->hookf/hookcount/hookmask`, runs `lj_trace_abort(g)` (a read-modify-write of `J->state`, `lj_trace.h:45`), and rewrites the dispatch table.
  - The 1.14.4 Java `aborted` latch is set and **never reset** (`LuaJITMachine.java:139-143`), so a second runaway is never aborted. Soft abort is checked only on entry to `handleEvent` (:195-196), which means the *next* event gets killed, not the running loop.
- Scenario: `while true do pcall(function() while true do end end) end` was still running 3 s after abort (verified). The worker is then lost. In 1.21.1 the monitor gives up and spawns a new worker. The old thread keeps burning a core, and a later shutdown calls close, which crashes (CA-04).
- Carry-over: yes for the pattern. Fix:
  - Use one persistent hook that polls atomics (`softAbort`, `hardAbort`, `pause`) written by Java.
  - On hard abort, keep re-raising from the hook (do not disarm it) until control leaves `lua_resume`, so `pcall` cannot swallow the abort.
  - Only flags may be set cross-thread. Arm hooks from the VM thread itself, or keep the hook always armed.
  - Note: with the JIT on, count hooks do not fire inside compiled loops (established fact, owned by the preemption lane). The original design only worked because the JIT was off (CA-08).

**CA-06 (high): There is no pause/suspend support, and the yield-from-hook approach breaks nested coroutines.**
- CC:T's `TimeoutState.isPaused()` asks the VM to suspend and return `MachineResult.PAUSE` (Cobalt does this through `InterruptAction.SUSPEND`, `CobaltLuaMachine.java:60-68`). The old code has nothing for this, so one computer can monopolize a worker for up to 7 s per event.
- The 1.14.4 yield-request hook uses `lua_yield` from a hook. That yields whichever coroutine is *currently* running. Inside `parallel`/multishell children, the parent's `coroutine.resume` returns with zero values, and siblings later receive a synthetic zero-argument event. 1.14.4 queues `computer.queueEvent(null,null)` to resume the main routine, so `os.pullEvent()` can return nil as the event name.
- A hook-yield inside a non-yieldable C context (a `table.sort` comparator, a `string.gsub` callback, a `load` reader) throws "attempt to yield across C-call boundary" at a random point.
- Fix: suspend only at the main-routine level. Either keep track of the running coroutine, or patch or wrap `coroutine.resume/wrap` so a sentinel yield propagates up to the main routine (the CraftOS-PC approach). This needs design work together with the preemption lane.

### C. Data conversion correctness

**CA-07 (high, verified): String encoding does not match CC:T.**
- Lua→Java: `decodeString` uses `StandardCharsets.US_ASCII` (1.20.1 `LuaJITMachine.java:13-15`; 1.14.4 :34-36), so every byte ≥ 0x80 becomes U+FFFD. It also costs one JNI upcall per string (`main.cpp:357-360`).
- Java→Lua: `GetStringUTFChars` plus `lua_pushstring` (:272-274) produce modified UTF-8. U+0080..U+07FF become 2 bytes, U+0000 becomes `C0 80`, and supplementary characters become 6 bytes.
- `byte[]` goes through `lua_pushstring` (:280-284) and is truncated at the first NUL. Error messages (:440-441), API names, and the bios use the same `GetStringUTFChars` path.
- CC:T's rule (`LuaString.java:520-546`) is that each byte is a char 0-255 and chars above 255 become `?`.
- Impact: `term.write("\149")`, `term.blit`, paintutils/teletext graphics, `fs` binary data, `http` bodies, and `string.char(200)` round trips are all corrupted (verified in the strings and api-call tests).
- Carry-over: yes. Fix:
  - Lua→Java: build the `jstring` natively from a widened `jchar[]` (`NewString`).
  - Java→Lua: `GetStringRegion`/`GetStringCritical`, narrow to bytes (>255 becomes `?`), then `lua_pushlstring`.
  - `byte[]`/`ByteBuffer` (heap or direct): `lua_pushlstring` with an explicit length, using `GetDirectBufferAddress` for direct buffers.

**CA-08 (high, verified; decision input): `initMachine` never enables the JIT.**
- `main.cpp:704-711` uses `luaL_newstate` plus direct calls to `luaopen_base/math/string/table/bit`. There is no `luaopen_jit`, and LuaJIT sets `JIT_F_ON` only in `jit_init` (`lib_jit.c:712-718`).
- Measured: hook hits 200,000 versus 0; benchmark 0.78 s versus 0.08 s; simple loop 0.57 s versus 0.26 s.
- Also, `luaopen_*` is called directly instead of through `lua_call`. The Lua 5.1 manual requires `lua_call`. LuaJIT tolerates direct calls for these five libraries, but the rewrite should open libraries the documented way.
- If the rewrite enables the JIT by opening `jit` through `lua_call`, it must afterwards remove the `jit` global and `_LOADED.jit`, and the `_PRELOAD` entries for `jit.util` and `jit.profile`.

**CA-09 (high): Conversion coverage does not match the CC:T 1.20/1.21 value model.**
- Unhandled types: `ByteBuffer`, `Collection`, `ILuaFunction`, generic objects exposing `@LuaFunction` methods (resolved through `MachineEnvironment.luaMethods()`), `MethodResult` callbacks, and `IArguments`.
- `IDynamicLuaObject` hits `assert(false)` (1.20.1 :289-291).
- The 1.20.1 `callMethod` lookup (:573) uses `(ILuaContext,int,IArguments)MethodResult`, but `invoke_java_fn` (:418) passes `LuaJITMachine` (which does not implement `ILuaContext` on 1.20.1) and an `Object[]` (not `IArguments`), and casts the `MethodResult` return to `jobjectArray`. Under `-Xcheck:jni` this is a fatal argument-type mismatch; without it, it is undefined behavior. The code is dead only because `registerAPI` is commented out (:759-780).
- `wrap_lua_object` (:503-523) relies on `getMethodNames` and `callMethod`, which `ILuaAPI` no longer has in 1.20/1.21. APIs are now reflected through `MethodSupplier<LuaMethod>`.
- Lua functions, threads, and userdata inside tables raise an error. Cobalt skips them silently by returning null.
- Fix: port the *semantics* of `CobaltLuaMachine.toValue`/`toObject`, and wrap calls through `LuaMethod.apply(target, context, IArguments)`. Implement yields through a Lua-side trampoline: the C function returns a "yield + callback id" marker, the Lua wrapper calls `coroutine.yield`, then calls the native resume with the callback. This is needed because Lua 5.1/LuaJIT has no `lua_callk` continuations.
- Carry-over: n/a, this has to be new code.

**CA-10 (medium, verified): The `handleEvent` result protocol is wrong.**
- Off-by-one: `before = lua_gettop(L)` (:801) includes the bios function on the first resume. As a result, `nresults = after - before` drops the first yielded value, leaves it on the stack, and the offset persists. Every later yield then loses its first value, which is the event filter (verified).
- Success and error results are not separated: `{false, msg}` on error looks the same as a legitimate `coroutine.yield(false, ...)`.
- The 1.14.4 Java side (`LuaJITMachine.java:198-210`) maps an error to `MachineResult.OK`, so a dead bios leaves the computer silently wedged. Each later event returns "cannot resume non-suspended coroutine" (verified). A finished main routine is not treated as shutdown, unlike Cobalt (`CobaltLuaMachine.java:138-141`). By contrast, 1.12.2 handled errors correctly, so this is a regression.
- Fix: return status and values separately. Pop everything the coroutine left on its stack after each resume.

**CA-11 (medium, verified with -Xcheck:jni): JNI hygiene problems.**
- No `ExceptionCheck` after most JNI calls (OnLoad lookups, `getNames`, `entrySet/iterator/next`, `decodeString`, `valueOf`, `NewObjectArray`).
- `IsInstanceOf` is called **with an exception pending** (:425).
- On the `InterruptedException` path (:422-456), the exception is left pending and Lua keeps running and making JNI calls. The exception later escapes `handleEvent` as a sneaky checked exception.
- Local references leak inside loops (`map_to_table` :205-213, `table_to_map` :240-245, `to_java_values`). In HotSpot this means memory growth until the native frame returns.
- `JNI_FALSE` is passed as a `jboolean*` `isCopy`.
- `JNI_OnLoad` returns success even when lookups fail (leaving a pending `NoClassDefFoundError`), and `initMachine` just returns false with no diagnostic.
- What is good: `GetEnv` is called on every callback, with no cached `JNIEnv` across threads. Keep that.

**CA-12 (low, verified): Error position and level semantics differ from CC:T.**
- The `invoke_java_fn` error prefix comes from `lua_getstack(L,1)` (:428-436), which gives `"[C]:-1:"` when the call goes through `pcall`.
- `LuaException.getLevel()`/`hasLevel()` are ignored. CC:T applies them (`ResultInterpreterFunction.wrap` :97-102).
- Exception messages go through modified UTF-8.

**CA-13 (info, verified): Missing `lua_checkstack` does *not* corrupt memory on LuaJIT.**
LuaJIT `lua_push*` auto-grows the stack (`lj_state.h:11-12`). The real risk is the "stack overflow" or OOM error raised during growth when outside a protected call, which is covered by CA-01. The rewrite should still call `lua_checkstack`, so that it gets a clean, protected failure path.

### D. Sandbox and security

**CA-14 (critical, verified): Untrusted LuaJIT bytecode is accepted, which is memory-unsafe and leads to RCE on the server host.**
- `luaopen_base` provides `load`, `loadstring`, `loadfile`, and `dofile` with the default mode "bt" (`lib_base.c:380,410,441,446`). `luaopen_string` provides `string.dump` (`lib_string.c:123`). CCLuaJIT removes neither.
- The CC:T 1.21.1 ROM runs programs with `load(contents, "@/"..path, nil, env)` (`rom/programs/shell.lua:163`), which uses mode "bt". So any file on a computer's disk that starts with `\27LJ` runs as raw bytecode. That file can be written with `fs`, downloaded with `wget`, or produced locally with `string.dump`.
- LuaJIT does not verify bytecode. My 2-byte operand patch segfaults the VM (`$W/c/bc_0xC0.lua`, `bc_0xFF.lua`). Public techniques turn this class of bug into arbitrary read/write and then code execution.
- Carry-over: yes for any port. Fix:
  - Install C-level `load`/`loadstring` wrappers that force mode "t", using `luaL_loadbufferx`.
  - Remove `string.dump` (CC:T/Cobalt has no `string.dump`).
  - Remove `loadfile`/`dofile` natively. The bios overrides them (`bios.lua:423,441`), but do not rely on that.
  - Load the bios itself with mode "t".

**CA-15 (high, verified): Exposing the debug library or the registry, which the 1.21.1 ROM needs, breaks the sandbox.**
- ROM usage:
  - `debug.getregistry()` in `rom/modules/main/cc/internal/exception.lua:90,101` and `cc/require.lua:134`;
  - `debug.getupvalue` in `error_hints.lua:124`;
  - `getinfo`, `getlocal`, `getmetatable`, `traceback`.
- Risks:
  - The real registry exposes `_PRELOAD.ffi` whenever the state was opened with `luaL_openlibs` (verified). `require`-free access to FFI means arbitrary native calls.
  - It also exposes `_LOADED`, the internal metatables (`"JavaFN"`, `"jobject_ref"`), and the anchors of the main thread.
  - `debug.getupvalue` returns C-closure upvalues (verified). For the JavaFN design, user code can therefore run `getmetatable(ud).__gc(ud)`. That deletes the global refs while the closure stays callable (verified with the debug variant). `invoke_java_fn` (:414-418) never checks `jfn->obj`, which is JNI undefined behavior.
- Fix:
  - Give `debug.getregistry` a separate fake registry table.
  - Expose a curated `debug` subset: no `setupvalue` or `upvaluejoin` on C functions, no `setmetatable` on internal userdata, no `sethook`.
  - Set `__metatable` on all internal metatables.
  - Make finalizers idempotent and check validity on every call.
  - Prefer integer handles into a Java-side table over global refs inside userdata.
  - Never open `ffi`, `package`, `io`, `os`, `jit.util`, or `jit.profile`. Consider whether `string.buffer` should stay reachable.

**CA-16 (high, verified feasible fix): There is no memory cap.**
`luaL_newstate` uses LuaJIT's internal allocator with no limit (:704). One computer can allocate native memory until the host runs out, outside `-Xmx`, which risks the OOM killer taking the whole server. Fix: use `lua_newstate(custom_alloc, ud)` with a per-computer budget. This works on GC64, as verified with a 32 MiB cap. Allocation failure raises a catchable "not enough memory" and the process stays alive. Also consider removing `collectgarbage("stop")`. CCLJ removes `collectgarbage` entirely, while CC:T programs sometimes call `collectgarbage("count")`, so a safe subset would be better.

**CA-17 (info): The library baseline is mostly sound.**
Opening only base, math, string, table, and bit, with no `io`/`os`/`ffi`/`jit`/`debug`/`package`, and removing `collectgarbage`, `gcinfo`, and `newproxy` (:730-735) is the right security instinct. Keep it as the baseline, plus the CA-14 and CA-15 fixes. `getfenv`/`setfenv` exist, but CC:T's own bios provides equivalents, so they are not a privilege boundary.

### E. Lua environment (only as it concerns the existing init code; deep compatibility is another lane)

**CA-18 (high for 1.20.1, verified): The init code produces a Lua environment the 1.20/1.21 bios cannot run in.**
- `bit32` is aliased to LuaJIT `bit` (:713-714):
  - results are signed (`bnot(0)=-1`);
  - shift counts are taken mod 32 (`lshift(1,32)=1` instead of 0);
  - `extract`, `replace`, `btest`, `lrotate`, and `rrotate` are missing.

  `textutils.lua:436` calls `bit32.extract`, which breaks `textutils.serializeJSON` for non-BMP characters. The ROM's `colors.lua:250` and `bios.lua:22-30` (`bit` API) return signed values.
- `table.pack` and `table.unpack` are missing in 1.20.1, so `os.pullEvent` fails immediately (`bios.lua:42,46`). 1.14.4 polyfilled them in `prebios.lua` by concatenating it ahead of the bios, which shifts every bios line number by 5.
- `utf8` and `debug` are missing.
- `_VERSION` is "Lua 5.1".
- A stray `__inext` global (:716-717) is leaked.
- `_ENV` is not supported, and 15 ROM files use it. That gap is flagged to the compatibility lane.

### F. 1.14.4 runtime design (obsolete in 1.21.1; listed so it is not ported)

**CA-19 (high, not carry-over if redesigned): The blocking yield and special-event interception are broken.**
- Location: `LuaJITMachine.java:38-41, 168-211, 219-299` together with `cclj_txfm.js:62-85`.
- `yield()` parks the CC worker thread in `Object.wait()` inside a native `lua_resume` frame. Problems:
  - **Lost wakeup:** `containsKey` is checked outside the monitor (:231), then `wait()` runs (:239-241).
  - **InterruptedException is swallowed** (:242-244), so the thread cannot be killed.
  - **Typo deadlock:** `isSpecialEvent` matches `"task_completed"` (:40), but `taskCompleted` queues `"task_complete"` (:298) and `executeMainThreadTask` waits for `"task_complete"` (:256). Every main-thread task therefore deadlocks the computer forever.
  - The `queueEvent` coremod injection runs before ComputerExecutor's `closed`, `command`, and queue-limit checks, and dereferences `this.machine`, which is null while the computer is off. A `turtle_response` that arrives after a shutdown then throws an NPE on the server thread (medium confidence on reachability).
  - Programs that wait for `turtle_response` themselves never receive it, because the event is swallowed.
- In 1.21.1 all of this is replaced by `MethodResult.pullEvent`/`ILuaCallback` (`ResultInterpreterFunction.java:52-95`) plus `ILuaContext.issueMainThreadTask`.

**CA-20 (medium, not carry-over): The coremod is fragile.**
- It matches CC:T 1.86.2 bytecode exactly (`cclj_txfm.js:22-61, 87-121`) and throws at class load on any mismatch.
- `mods.toml:29-34` allows `computercraft` `[1.86.2,)`, which is open-ended, so newer CC:T versions crash the game.
- `COMPUTER_EXECUTOR_DESC.replace("/", ".")` replaces only the first slash. It works only because ModLauncher normalizes dots back to slashes (medium confidence).
- In 1.21.1, use `ServerContext.luaMachine` (public static, `ServerContext.java:55`, consumed at :77). No mixin or coremod is needed. Because it is internal API, pin the CC:T version range.

### G. Native loading, build and platform

**CA-21 (high, verified parts): The natives loader is broken or unsafe.**
- 1.20.1 calls `System.load` on `/run/media/vereena/...` (`LuaJITMachine.java:18-19`), which fails everywhere except the original author's machine. The resulting `ExceptionInInitializerError` is caught by `ComputerThread` (`catch LinkageError`), so the computer errors but the server survives.
- 1.14.4 (`:51-95`):
  - It creates a new temp dir per JVM run with `deleteOnExit`. On Windows the loaded DLLs and the dir stay behind on every run (verified).
  - A missing resource gives `GZIPInputStream(null)`, which throws an NPE that `catch (IOException)` (:44-48) does not catch, so the class init fails.
  - There is no CPU-architecture check: only x86-64 is built, so aarch64 Linux and Apple Silicon fail.
  - macOS natives were never built (`build.sh:11-13`, Dockerfile :55-59 are TODOs). The `-undefined dynamic_lookup` scheme also expects libluajit symbols to be globally visible, but HotSpot `dlopen`s with RTLD_LOCAL (medium confidence).
  - Two separate libraries are loaded by base name, so a different `lua51.dll` or libluajit already in the process from another mod could be picked up instead.
  - There is no fallback to Cobalt.
- Fix:
  - One native library per OS/arch with LuaJIT statically linked in (`BUILDMODE=static`, `-fvisibility=hidden`, `-Wl,--exclude-libs,ALL`).
  - Extract to a content-hashed cache dir, for example `<gamedir>/.ccluajit/<sha>/`, and reuse it.
  - Check `os.arch`.
  - If loading fails, leave `ServerContext.luaMachine` as Cobalt and log one warning.

**CA-22 (high, verified): The native build pipeline is broken.**
- 1.20.1 `main.cpp` does not compile (`assert` at :291; also no `<stdio.h>` for `snprintf`/`sprintf`).
- Makefile :13: `-l:libluajit-5.1.so -shared -fPIE` placed **before** `main.cpp`. With the Debian/Ubuntu default `--as-needed`, the result has no `DT_NEEDED` on libluajit and the JVM dies with "undefined symbol: luaL_newstate" (verified). `-fPIE` should be `-fPIC`. There is no `-O2` and no warnings.
- `OUT_DIR=/root/out` and `LUAJIT_DIR/bin/{linux,windows}` exist only inside the Docker image.
- 1.20.1 has no Dockerfile or `build_natives.sh` at all, and no Gradle hook.
- 1.14.4 Dockerfile:
  - uses `fedora:30`, which is end-of-life (2020);
  - builds osxcross with an SDK downloaded from a third-party GitHub repo, which raises an Apple SDK licensing problem;
  - does not build macOS.
- The `glibc` baseline equals the build distribution's, so binaries built on a new distribution will not load on older servers.
- 1.14.4 `build.gradle`: `jcenter()`, ForgeGradle 3.+, and old maven URLs are all dead, and `processResources` depends on Docker plus bash.
- What works: the Windows MinGW DLL has only KERNEL32, msvcrt, and lua51 imports.
- Fix: CMake or Zig-cc cross builds, or a GitHub Actions matrix (windows-x64, linux-x64 on an old glibc, linux-arm64, macos-x64, macos-arm64), with artifacts packed into the jar.

**CA-23 (low): Packaging metadata needs cleanup.**
- `fabric.mod.json`:
  - has no `computercraft` dependency, so without CC:T `onInitialize` hits NoClassDefFoundError;
  - needs a tight version range because it uses internal API;
  - still has template contact URLs from the Fabric example mod (:8-11).
- The mixin config points at a nonexistent package with an empty list.
- `splitEnvironmentSourceSets` is used with no client sources.
- Java release 17 needs to become 21 for 1.21.1.

**CA-24 (info): Licensing needs attention.**
- The project is MIT.
- Shipped LuaJIT binaries need LuaJIT's MIT notice (`vendor/luajit/COPYRIGHT`), and the jar does not include it.
- `vendor/jni-headers` is GPLv2 with the Classpath exception (JDK 8-era copies). Use `$JAVA_HOME/include` instead.
- **CC:T's `CobaltLuaMachine.java` is `LicenseRef-CCPL` ("Do not distribute without permission")**, whereas `ResultInterpreterFunction`, `VarargArguments`, and `TableImpl` are MPL-2.0. Re-implement the conversion semantics rather than copying CobaltLuaMachine code into an MIT mod. MPL files can be reused at file level under MPL.
- The Dockerfile's MacOSX SDK download has the Apple licensing problem noted in CA-22.

---

## 4. Rewrite pitfalls checklist (must-haves for 1.21.1)

1. Every JNI entry point runs its Lua work under `lua_pcall` or `lua_cpcall`. No Lua error may ever unwind through a JNI or Java frame. On Windows that is an immediate JVM fatal error.
2. Never call back into Lua from Java while a Lua error could escape (re-entry from `IArguments` getters). Either convert arguments eagerly before calling Java, or protect each re-entry.
3. Conversions must be total (unknown types become nil plus a log line), iterative or depth-capped, cycle-safe with one identity map per conversion, and Latin-1 exact, including `ByteBuffer` and `byte[]` with NULs.
4. `close()` is called from the monitor thread and more than once. It must be idempotent, only set flags, and free the state on the owning thread.
5. Preemption: one persistent hook that polls atomics. A hard abort cannot be caught by `pcall`. Pause or suspend must reach the main routine through nested coroutines. Decide on the JIT: the original ran with the JIT off.
6. Sandbox:
   - force text-only `load`/`loadstring`;
   - no `string.dump`, `loadfile`, or `dofile`;
   - a fake registry for `debug.getregistry`;
   - a curated `debug` subset;
   - `__metatable` on internal metatables;
   - never `luaL_openlibs` and never `ffi`;
   - remove `jit` from globals and the registry if the JIT is opened;
   - a per-computer memory cap with `lua_newstate`.
7. Environment: a real `bit32` (unsigned, Lua 5.2 semantics), `table.pack`/`unpack`, `utf8`, and CC:T-compatible `_VERSION`, implemented in C or in a prelude loaded as a separate chunk (not by concatenation onto the bios).
8. Results protocol: explicit status. Clear the coroutine stack after every resume. A dead main routine maps to error or shutdown, like `CobaltLuaMachine`.
9. Packaging: a single statically linked native per OS/arch, a content-hashed cache dir, an arch check, and a Cobalt fallback. CI builds for the five targets.
10. API surface: `ServerContext.luaMachine` (pin the CC:T version), `MachineEnvironment.apis()` plus `getModuleName()` (registered into `package.loaded`, as Cobalt does with `registry._LOADED`), `luaMethods.forEachMethod`, `MethodResult`/`ILuaCallback` through a Lua-side trampoline, and `LuaException` levels.

---

## 5. Severity summary

| ID | Sev | Verified | Carry-over | Title |
|---|---|---|---|---|
| CA-01 | critical | yes (Win+Linux) | yes | Unprotected Lua errors terminate the JVM |
| CA-02 | critical | yes | yes | Unbounded native recursion overflows the C stack |
| CA-03 | critical | yes | yes | Java→Lua cycle detection broken; CallStaticIntMethod misuse |
| CA-04 | critical | yes | yes (naive port) | close/abort cross-thread use-after-free |
| CA-14 | critical | yes | yes | Untrusted bytecode accepted (RCE class) |
| CA-05 | high | yes | yes | One-shot catchable abort; hook races |
| CA-06 | high | analysis | partial | No pause; hook-yield breaks nested coroutines |
| CA-07 | high | yes | yes | String encoding mismatch |
| CA-08 | high | yes | decision | JIT never enabled (interpreter-only) |
| CA-09 | high | analysis | n/a | Conversion coverage and API dispatch obsolete |
| CA-15 | high | yes | yes | debug/registry exposure, ffi reachability, user-callable finalizers |
| CA-16 | high | yes (fix) | yes | No memory cap |
| CA-18 | high | yes | yes | Init environment cannot run the 1.20/1.21 bios (bit32, table.pack...) |
| CA-19 | high | analysis | no | 1.14.4 blocking yield, lost wakeup, task_complete typo deadlock |
| CA-21 | high | partly | yes | Natives loader (paths, temp leak, arch, macOS, no fallback) |
| CA-22 | high | yes | yes | Build pipeline (doesn't compile; as-needed link breakage) |
| CA-10 | medium | yes | yes | handleEvent result protocol (off-by-one, errors become OK) |
| CA-11 | medium | yes | yes | JNI hygiene (pending exceptions, local refs) |
| CA-20 | medium | analysis | no | Coremod fragility |
| CA-12 | low | yes | yes | Error position and levels |
| CA-23 | low | analysis | yes | Mod metadata |
| CA-13 | info | yes | – | LuaJIT auto-grows the stack (corrects a common assumption) |
| CA-17 | info | – | – | Library baseline sound |
| CA-24 | info | – | – | Licensing |

---

## 6. Salvage assessment

| Piece (branch:file:lines) | Verdict | Notes |
|---|---|---|
| Idea: replace the factory via `ServerContext.luaMachine = LuaJITMachine::new` (1.20.1 `CCLuaJIT.java:13`) | **Reuse** | Correct hook for 1.21.1 (`ServerContext.java:55`). Add a native-load check, a Cobalt fallback, and a pinned CC:T range. |
| `JNI_OnLoad` caching of jclass global refs and method/field IDs (:549-671) | Reuse the pattern, rewrite the content | Keep the caching idea. Add `ExceptionCheck`, clear errors, and fail loudly. Most cached classes become unnecessary once Latin-1 conversion is native. |
| `GetEnv` per callback, no cached `JNIEnv` (:409, :476, :159) | **Reuse** | Correct thread discipline. |
| Restricted library set and removal of `collectgarbage`/`gcinfo`/`newproxy` (:707-735) | Reuse as a baseline | Open libraries through `lua_call`. Add the CA-14/15/16 hardening. Replace the `bit32` alias. Decide on `jit`. |
| Main routine via `lua_newthread`, anchored on the main state (:785-795) | Reuse the idea | Anchor it in the registry instead of a stack slot. Load the bios as raw bytes with mode "t". |
| Lua→Java table conversion with an identity table for cycles (:221-259) | Reuse the idea only | Must become iterative or depth-limited, skip unconvertible values, and produce Latin-1 strings natively. |
| `to_java_values` reverse-pop ordering (:388-394) | Minor reuse | Fine as is, once conversion cannot raise errors. |
| Hook-based abort (:182-187, :823-826) | Rewrite | Persistent flag-polling hook, uncatchable hard abort, no cross-thread `lua_sethook`. |
| `to_lua_value`/`map_to_table` (:189-219, :261-334) | **Rewrite** | Encoding, coverage, cycles, recursion, and errors are all wrong. |
| `handleEvent` (:798-821) | Rewrite | Protected, explicit status, stack cleanup. |
| `invoke_java_fn`/JavaFN/`wrap_lua_object`/`registerAPI` (:401-523, :759-780) | Rewrite | API model changed (MethodSupplier, LuaMethod, IArguments, MethodResult). Use integer handles instead of userdata global refs. Yields go through a Lua trampoline. |
| `deinitMachine`/`abortMachine` (:750-757, :823-826) | Rewrite | Thread-safe lifecycle (CA-04). |
| `__inext`, `new_jobject_ref`/`finalize_jobject_ref` | Drop | Dead or legacy code. |
| 1.14.4 `LuaJITMachine.java` blocking yield, special events, `executeMainThreadTask`, `TaskScheduler` (1.12.2) | **Drop** | Superseded by MethodResult callbacks. Defective (CA-19). |
| 1.14.4 event-filter logic (`LuaJITMachine.java:172-174`) | Re-derive from Cobalt | Same semantics as `CobaltLuaMachine.java:120-122`. |
| `cclj_txfm.js` coremod, `coremods.json` | Drop | Not needed in 1.21.1. |
| `prebios.lua` | Drop | Implement `table.pack`/`unpack` natively or in a separate prelude chunk. |
| `OS.java` | Rewrite | Add arch detection and a cache dir. |
| Natives extraction (1.14.4 `:51-95`) | Rewrite | CA-21. |
| Makefile, build.sh, Dockerfile, build_natives.sh | Rewrite | CA-22. CMake or Zig plus a CI matrix, LuaJIT statically linked. |
| `vendor/luajit` submodule (v2.1 19878ec0) | Keep, but bump | Track a current v2.1 commit. Builds cleanly in WSL with `make BUILDMODE=dynamic`; use static. |
| `vendor/jni-headers` | Drop | Use the JDK's include dir. |
| build.gradle / fabric.mod.json (1.20.1) | Rework | Fabric template leftovers. The 1.21.1 port needs Java 21, a CC:T dependency range, native packaging, and possibly a NeoForge target. |

Rough share of salvageable lines: under 10% of `main.cpp`, under 5% of the Java side. The real value of the existing code is the *architecture sketch*: replace the factory, hold one `lua_State` per computer, and drive it with a hook. It also shows clearly where the hard problems are.

---

## 7. Effort estimate (for this lane: native/JNI glue plus machine plus sandbox plus packaging; excludes `_ENV` compatibility and deep JIT-preemption R&D)

| Work item | Experienced modder with C/JNI | Modding/JNI newcomer |
|---|---|---|
| Protected, total, iterative, Latin-1 value conversion (both directions, ByteBuffer, Collections, cycles) | 2-3 d | 5-8 d |
| Machine lifecycle (thread-safe close, deferred free, atomic interrupt flags, hook, pause semantics incl. nested coroutines) | 3-5 d | 8-14 d |
| API dispatch (MethodSupplier/LuaMethod, IArguments impl, MethodResult/ILuaCallback trampoline, LuaException levels, ILuaFunction/IDynamicLuaObject, module names) | 3-4 d | 7-12 d |
| Sandbox and env (text-only load, debug subset plus fake registry, bit32/table.pack/utf8, memory-cap allocator, removal of jit/ffi paths) | 3-5 d | 6-10 d |
| Native build plus packaging (CMake/Zig, 5-target CI matrix, static LuaJIT, cache-dir extraction, arch detection, Cobalt fallback) | 3-5 d | 6-12 d |
| Testing (port the harness, run the CC:T ROM test suite under the new machine, crash/fuzz tests for the converters, -Xcheck:jni CI) | 3-5 d | 6-10 d |
| **Total** | **≈17-27 person-days** | **≈38-66 person-days** |

Reasoning: almost nothing can be lifted verbatim. The tricky parts are concurrency (close from the monitor thread), yields through C boundaries without `lua_callk`, and sandbox hardening. All of them need careful testing, and crashes in this layer take down the whole server. A newcomer additionally has to learn JNI failure modes, LuaJIT's error and unwinding model, and CC:T's internal scheduler. The `_ENV` question (15 ROM files) and the JIT-versus-preemption question can each add significant time on top and may decide feasibility. Those belong to other lanes.

---

## 8. Artifacts

- Harness: `$W/jni/src/harness/Main.java`, stubs under `$W/jni/src/dan200/...` and `$W/jni/src/gay/...`, runners `$W/jni/run.sh` and `$W/jni/run_all.sh`, outputs in `$W/jni/logs/run_all.txt`, `$W/jni/logs/hs_err_*.log`, and `$W/jni/logs/gc-upvalue-checkjni.txt`.
- Native builds: `$W/src/main_1144.cpp` (unmodified copy), `cclj_1144.dll`, `main_1144_debug.cpp`/`cclj_1144_debug.dll` (adds `luaopen_debug`), `main_1201.cpp` (1.20.1 plus the two missing `#include`s)/`cclj_1201.dll`.
- C probes: `$W/c/ljprobe.c`, `ljprobe.exe`.
- Lua probes: `$W/c/probe.lua`, `$W/c/bytecode_poc.lua`, `$W/c/bc_0x*.lua` and `.out`.
- Linux: `$W/linux/run.sh`, `$W/linux/cclj_makefile.so`, `$W/linux/cclj_fixed.so`, LuaJIT build in `$W/linux/luajit` (`build.log`).
- Native extraction test: `$W/jni/extract/ExtractTest.java`.

`$W` = `<scratch>\work\code-audit`

## 9. Open questions

- Should the rewrite enable the JIT? That gives about 10x on numeric code, but count hooks stop firing in traces and preemption needs another mechanism. With the JIT off (as in the original), hooks work, but the gain over Cobalt is only the interpreter speedup, which the performance lane should quantify.
- Is the `_ENV` gap (15 ROM files) solvable with LuaJIT? If not, the whole approach may be blocked. That is the compatibility lane's question.
- Pause/suspend across nested coroutines: patch `coroutine.resume`/`wrap` in C, or accept coarser scheduling?
- Is a per-computer memory cap acceptable to users? CC:T itself has none, but Cobalt is bounded by the JVM heap. What default would suit them?
- Which platforms should be supported (Windows x64, Linux x64/arm64, macOS x64/arm64)? macOS needs a legal build path (GitHub-hosted macOS runners) instead of osxcross with a third-party SDK.
