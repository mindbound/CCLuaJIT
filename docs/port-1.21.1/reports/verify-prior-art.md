# Adversarial verification of the "prior-art" findings (PA-1 .. PA-6)

Verifier key: `verify-prior-art`. Date: 2026-11-07.
Work dir: `<scratch>\work\verify-prior-art` (below: `$W`).
Nothing was written inside `<repo>`. I used only `git show`, `git rev-parse` and plain reads there.
I did not reuse the investigator's binaries or test programs. I wrote my own tests and rebuilt LuaJIT from a fresh copy.

## Verdict summary

| ID | Investigator severity | Verdict | My severity | One-line correction |
|---|---|---|---|---|
| PA-1 | critical | **partially confirmed** | critical | CHECKHOOK plus an async count hook reliably **aborts** JIT loops on Windows and Linux, with an overhead of about 3% or less. A naive **pause** (`lua_yield` inside the hook) works only when the hook fires in the host's top-level coroutine. In CC:T, user code always runs in nested coroutines. There the naive pause causes spurious `coroutine.resume` returns, **lost events**, and "attempt to yield across C-call boundary" errors. The claim that pause is "implementable" is overstated. |
| PA-2 | high | **confirmed, now verified by experiment** | critical (if the naive design is used) | Calling `lua_sethook` from a watchdog thread is a real data race. I reproduced LuaJIT's own assert ("active hook flag removed"), **segfaults on Linux**, a `PANIC` abort on Windows, and **permanently lost interrupts**: the hookmask stays stuck at 0x18 or 0x08, and the computer can never be interrupted again. |
| PA-3 | high | **confirmed** | critical | `string.find` and similar pattern calls cannot be interrupted, even with CHECKHOOK. Runtime grows about n^5: CC:T's own 1e4-character test case would run for decades. CC:T then abandons the worker and spawns a new one, and it also calls `machine.close()` from the monitor thread while native code is still running. With the old `close()->lua_close` design this is a use-after-free. Any player can cause a server DoS or crash this way. |
| PA-4 | critical | **confirmed, with nuances** | critical | All cited facts check out (FAQ, corsix 2015, Luanti GHSA/CVE-2026-41196 CVSS 10.0, CVE-2026-40959 9.3, CVE-2024-2517x, OpenComputers settings, CraftOS-PC load shim). I verified by experiment: on this LuaJIT build, **13.7% of random 2-byte mutations of a `string.dump` chunk segfault** the VM. The current 1.20.1 `main.cpp` exposes `load`/`loadstring` with binary chunks and `string.dump`. Nuances: CVE-2019-19391 is formally **disputed**. The getfenv vector only matters if the host keeps a richer "real" `_G` behind the user env. CVE-2026-40959's text does not name getfenv. |
| PA-5 | high | **confirmed, small correction** | medium | All cited CraftOS-PC facts verified. Correction: the `craftos2-luajit` fork is not fully dormant. It had commits on 2026-04-16 ("Fixed utf8.char") and 2026-06-09 ("Enable Lua 5.2 compat for Windows"). There has been no Accelerated release since v2.8.3-luajit (2024-08-17). As a finding this is decision input, not a defect in the mod, so I rate it medium. |
| PA-6 | high | **confirmed, and broader than stated** | high | Checked against Cobalt 0.9.9 itself, which is the real CC:T reference. Aliasing `bit32 = bit` gives the wrong sign, wrong rounding and wrong shift semantics. **`bit32.btest/extract/replace/lrotate/rrotate` are missing**, and the CC:T ROM uses `bit32.extract` at `textutils.lua:436`. `bit32.band()` and `bit32.bor()` with no arguments raise an error. |

---

## Environment and builds

* The LuaJIT sources in the shared prebuilt tree are byte-identical to the vendored submodule (`vendor/luajit` at `19878ec05c239ccaf5f3d17af27670a963e25b8b`). I compared 151 `.c/.h/.dasc` files with `cmp`: 0 differ.
* Windows (MinGW gcc, static). Each variant was built in a fresh copy:
  ```
  cp -r <scratch>/luajit-win $W/lj-plain ; cp -r ... $W/lj-ck ; cp -r ... $W/lj-ckassert
  (lj-plain/src)    mingw32-make clean; mingw32-make -j24 BUILDMODE=static
  (lj-ck/src)       mingw32-make clean; mingw32-make -j24 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK
  (lj-ckassert/src) mingw32-make clean; mingw32-make -j24 BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_CHECKHOOK -DLUA_USE_ASSERT -DLUA_USE_APICHECK"
  ```
* Linux (WSL2 Ubuntu, kernel 6.18 x86_64, gcc): `make -j24 BUILDMODE=static [XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK]` in `$W/lj-linux-{ck,plain}`.
* I confirmed that the option took effect with `luajit -jdump=i -e "local x=0 for i=1,1000 do x=x+i end"`. The CHECKHOOK build emits `XLOAD [&g->hookmask] V` / `BAND +12` / `EQ +0` both before **and after** the `LOOP` marker, so the guard runs on every loop iteration. The plain build has no such instructions.
* Test sources: `$W/src/vtest.c` (abort/pause harness, 18 Lua cases), `$W/src/race.c` and `$W/src/race-spin.c` (cross-thread `lua_sethook` race), `$W/src/pausecorrect.c`, `$W/src/linux-run.sh`, `$W/bench/kern.lua`, `$W/bit/{exprs.lua,CobaltBit.java,run-lj.lua}`, `$W/sec/{fuzz.lua,getfenv.lua,pat.lua}`.
  Build: `gcc -O2 -o bin/vtest-<v>.exe src/vtest.c -I lj-<v>/src lj-<v>/src/libluajit.a -lpthread` (add `-lm -ldl` on Linux).

---

## PA-1: CHECKHOOK plus a timer-armed count hook

### What I checked
1. Source: `vendor/luajit/src/lj_record.c:2880-2899` contains the CHECKHOOK block. Its comment is "disabled by default, since the hook checks may be quite expensive in tight loops", followed by `IR_XLOAD` of `&J2G(J)->hookmask` (volatile), `BAND (LUA_MASKLINE|LUA_MASKCOUNT)` and a guard `EQ 0`. Confirmed.
2. Pall's statements: issue #779 (2021-12-13), "run `make XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK` to enable this (unsupported). Also, use `LUA_MASKCOUNT` -- never use `LUA_MASKLINE`". Issue #723 says CHECKHOOK "will not help you to stop Lua code that's stuck inside a C function". The lua-l posts of 2011-02-21 and 2011-06-13 match the quotes; I fetched them myself into `$W/web/lual1.html` and `lual2.html`. #1488 (2026-07-22) recommends CHECKHOOK again, but the reply is from Buristan (Tarantool), not Pall.
3. Tarantool: `cmake/luajit.cmake:41` has `set(LUAJIT_ENABLE_CHECKHOOK ON CACHE BOOL ...)`. Issue #7762 (closed 2022-12-07) speaks of "neglible platform performance degradation", based on benchmarks in a **private** repo (`roadmap-internal#105`). Nuance: GitHub code search finds `lua_sethook` in Tarantool only in `extra/exports` and an export test. Tarantool enabled the option for "later" use, so it is **not** evidence of a production deployment of asynchronous interruption.
4. FNEW being NYI explains why the `pcall(closure)` loop is interpreted: `lj_record.c:2695` has `case BC_FNEW: ... LJ_TRERR_NYIBC`.

### Abort experiment (my harness, `vtest abort <case>`)
A watchdog thread arms `lua_sethook(L, hook, LUA_MASKCOUNT, 1)` 200 ms after the start. The hook raises `"Too long without yielding"` and stays armed. A case counts as NOT_INTERRUPTED if the host has no result 3 s after arming. Results are in `$W/abort-results.txt` (Windows) and `$W/linux-results.txt` (Linux).

| case | shape | plain | CHECKHOOK (latency after arming) |
|---|---|---|---|
| 0 | `while true do end` | NOT | 0.021 ms (Linux 0.007) |
| 1 | while + arithmetic | NOT | 0.024 ms |
| 2 | for + table stores | NOT | 0.015 ms |
| 3 | infinite tail recursion | NOT | 0.016 ms |
| 4 | loop calling Lua fn | NOT | 0.015 ms |
| 5 | repeat/concat/sub | NOT | 0.012 ms |
| 6 | `while true do pcall(function() while true do end end) end` | NOT | 0.023 ms (2 fires, persistent hook) |
| 7 | user coroutine running infinite loop | NOT | 0.013 ms |
| 8 | `table.sort` comparator with 1e9 loop | 230 ms (inner loop ended) | 0.013 ms |
| 9 | `string.gsub` callback infinite loop | NOT | 0.011 ms |
| 10 | `__index` metamethod loop | NOT | 0.011 ms |
| 11 | iterator function loop | NOT | 0.045 ms |
| 12 | xpcall handler loop | NOT | 0.014 ms |
| 13 | `string.find(('a'):rep(3e5), '.-.-.-.-b$')` | NOT | **NOT** |
| 14 | worker coroutine plus JIT loops (ends naturally) | 26.7 ms | 0.003 ms |
| 15 | `x = x + math.sin(i)` loop | NOT | 0.012 ms |
| 16 | `debug.sethook(function() while true do end end, '', 1)` | NOT | **NOT** (also NOT with re-arming every ~1 ms; also NOT on Linux) |
| 17 | `while true do debug.sethook() end` | NOT | NOT with a single arm; **interrupted after 292 ms** with re-arming every ~1 ms |

The abort part of PA-1 is confirmed on both OSes. Every Lua-level shape tested, 15 in all, was interrupted within 0.003 to 0.045 ms.

### Pause experiment (`vtest pause <case>`): the critical qualification
The watchdog arms every 10 ms. The hook disarms and calls `lua_yield(L, 0)`. The host resumes, up to 5 host-level yields. Results are in `$W/pause-results.txt` and `$W/linux-results.txt`.

* Top-level coroutine loops (cases 0, 1, 4, 6, 10, 11, 15): `host_yields=5`, as the investigator claimed. `pausecorrect.c` shows that state is kept across 115-118 pauses: the same result as without pauses (`x=584863 s=29741837 n=64`, `$W/pausecorrect-results.txt`).
* **Nested user coroutine (case 7):** `host_yields=0`. The hook-yield lands in the inner coroutine, so the *user's* `coroutine.resume` returns `true` with no values while `status=suspended`. The host never regains control.
* **Event loss (case 14):** a user-level scheduler sends 20 events to a worker coroutine that does about 30-60 ms of JIT work per event. Result on Windows: "sent 20 events, worker received 8, spurious resume returns 13". On Linux: received 6, 15 spurious. Values passed to `resume` on a hook-yielded coroutine are discarded.
* **C-call boundary (cases 8, 9):** in a `table.sort` comparator or `string.gsub` callback, the hook's `lua_yield` raises "attempt to yield across C-call boundary" into the *user program*.
* **xpcall handler (case 12):** the failed yield terminates the handler's loop, and the chunk "completes" normally (`st=0`).
* In CC:T every user program runs nested. `bios.lua:731` runs the shell via `pcall(parallel.waitForAny, ...)`, which creates coroutines (`rom/apis/parallel.lua:78,91`). multishell and shell add more levels (`multishell.lua:64,90`, `shell.lua:752-781`). Cobalt's pause is transparent instead: `CobaltLuaMachine.java:67` returns `InterruptAction.SUSPEND`, and `:131-133` returns `MachineResult.PAUSE` with the whole VM suspended.
* Why pause matters: `ConfigSpec.java:167` sets `computer_threads` to a **default of 1**. Without real pausing, one busy computer monopolizes the only worker until the 7 s soft abort.

Conclusion: abort is solved. Pause still needs real design work. Possible approaches: (a) the hook yields only when `L` is the machine's main coroutine and `lua_isyieldable(L)`; otherwise it records "pause pending" and re-arms. (b) Make the pause transparent through nested `coroutine.resume`/`wrap`, either with a LuaJIT patch or a sealed Lua wrapper. (c) Drop pausing and accept unfair scheduling.

### Overhead (`$W/bench/bench-results.txt`, best of 5)
| kernel | plain (s) | CHECKHOOK (s) | delta |
|---|---|---|---|
| tight int loop 3e8 | 0.131 | 0.133 | +1.5% |
| sieve 2e6 x10 | 0.195 | 0.195 | 0% |
| nbody-like 2e6 steps | 0.129 | 0.130 | +0.8% |
| Lua calls 2e8 | 0.087 | 0.089 | +2.3% |
| strings (tostring/sub/concat) | 0.434 | 0.448 | +3.2% (noisy) |
| nested 2e4 x 2e4 | 0.177 | 0.174 | -1.7% |
Confirmed: about 3% or less on these micro-benchmarks.

---

## PA-2: cross-thread `lua_sethook` is not thread-safe

### Source and statements
* `lj_dispatch.c:329-340`: `lua_sethook` does a **non-atomic read-modify-write** of `g->hookmask` (line 336). It then calls `lj_trace_abort(g)`, defined at `lj_trace.h:45` as `G2J(g)->state &= ~LJ_TRACE_ACTIVE` (an RMW on the recorder state), and `lj_dispatch_update(g)` (`:106-185`, which rewrites the dispatch table and `g->dispatchmode`).
* The Lua thread does RMWs on the same byte through `hook_enter/hook_leave` (`lj_obj.h:679-683`, used in `callhook`, `lj_dispatch.c:359-383`). It also calls `lj_dispatch_update` itself during trace recording.
* Pall, #824 (2022-03-31, fetched): "`lua_sethook()` (only when *setting* a count hook) is *signal-safe*, but *not thread-safe*". The issue's own reproducer is cross-thread re-arming plus a hook-yield, the exact pattern PA-1 recommends, and it hits `"active hook flag removed"`. #1012: XmiliaH says "The comment in the source is wrong". The `lj_record.c` comment still says "from a signal handler or another native thread". SuspendThread on Windows is the reporter's suggestion, not Pall's. #1230 and #666: hooks are global by design. `lib_debug.c:323-341`: `debug.sethook` ignores the thread argument and calls the global `lua_sethook`.

### Experiment (`$W/src/race.c`, `race-spin.c`)
The watchdog re-arms `lua_sethook(L, hook, MASKCOUNT, 1)` in a loop. The hook disarms and yields, and the host resumes immediately. Lua code mixes calls, allocation and string work, with the JIT on and off.

Windows (`$W/race-results.txt`, `$W/race-spin-results.txt`):
* Assert build, tight re-arm: `LuaJIT ASSERT lj_dispatch.c:376: callhook: active hook flag removed` (JIT on and off).
* Release build, tight re-arm, JIT off: `LOST_INTERRUPT arms=14736455 fires=23 hookmask=0x18`. HOOK_ACTIVE is stuck, so no hook can ever fire again.
* Release build, tight re-arm, JIT on: `PANIC: unprotected error in call to Lua API (1.7292297604444e-322)`, which means corrupted state. In a JVM this calls `abort()` and kills the server.
* Busy-wait intervals: at 20 us, 5 of 6 runs lost the interrupt, with `hookmask=0x08`. The count hook is set, but the dispatch table and dispatchmode are stale, so nothing fires. At 100 us, 1 of 6 runs failed (about 1 failure in 600k arms). At 500 us, 0 of 6 failed (about 120k arms).
* At usleep(1000), which is about 15 ms on Windows, all 4 runs passed. That matches the investigator's clean 9,000-cycle run.

Linux (`$W/linux-results.txt`): tight re-arm gave **`Segmentation fault`** with the JIT on and off. At 20 us: `LOST_INTERRUPT hookmask=0x08`. At 100 us: OK in a single run.

Conclusion: confirmed by experiment. Realistic arm rates (one arm per 5-50 ms slice) make each event rare, but the race is still possible. It gets likely when arms cluster: pause, then soft abort, then hard abort, or a hook delayed behind a long C function. A server runs millions of arms per day. The consequences are a process crash or a computer that can never be interrupted again. So the investigator's alternative must be used: the arming must happen on the Lua thread, or the target thread must be suspended or signalled. Signal nuance: libjsig chaining is needed only when the handler uses a signal that HotSpot already uses (for example SIGSEGV or SIGUSR2). A dedicated real-time signal avoids it. I did not test this.

---

## PA-3: C functions cannot be interrupted

* Verified on both builds and both OSes: case 13 was NOT interrupted (`string.find(('a'):rep(3e5), '.-.-.-.-b$')`).
* Scaling (`$W/sec/pat.lua`, CHECKHOOK build): n=100: 0.239 s; n=200: 7.913 s; n=400: 278.6 s, which is about x32 per doubling (about n^5). Extrapolated, CC:T's own test input (n=1e4) takes about 2.7e9 s, effectively forever.
* CC:T expects these calls to time out. `timeout_spec.lua`, fetched from both `mc-1.21.x` and `mc-1.20.x`, expects `string.find`/`string.match` to fail with "Too long without yielding", and expects loop errors to match `^.+:%d+: Too long without yielding$`. CraftOS-PC's `resources/CCT-Tests.patch` on `master` deletes the pattern cases and loosens the position match. On `luajit` it deletes the whole file (`@@ -1,21 +0,0`).
* Abandonment: `ComputerThread.java:526-546`. With `afterHardAbort >= ABORT_TIMEOUT*2` it calls `workerFinished(runner)`. `:446-471` then calls `addWorker` to spawn a replacement. Worker threads are daemon threads (`ThreadUtils` `setDaemon(true)`), so shutdown does not hang, but each stuck call keeps a native thread spinning forever. 24 such calls saturate this 24-core machine.
* **Additional hazard (missed):** at hard abort, `ComputerExecutor.immediateFail` (`ComputerExecutor.java:254-261`) calls `machine.close()` **from the monitor thread** while the worker may still be in native code. The old CCLuaJIT machine (`origin/1.14.4_CCT` `LuaJITMachine.java:214-216`) does `close() -> deinitMachine() -> lua_close(L)`. That is a use-after-free under the running thread. `close()` must only set flags and defer the free to the owning thread.
* Severity: any player who can type into a computer can do this, and it leads to server DoS or a crash. I rate it critical by the task's scale. It is fixable with a small LuaJIT patch: an interrupt-flag check inside `lib_string.c` match loops, plus size limits for rep and concat.

---

## PA-4: sandbox escape vectors

* LuaJIT FAQ (fetched): "loading untrusted bytecode is not safe! It's trivial to crash the Lua or LuaJIT VM with maliciously crafted bytecode ... no bytecode verification on purpose". It also says "the only promising approach is to sandbox Lua code at the process level". Confirmed.
* corsix (fetched): "Malicious LuaJIT bytecode", posted 2015-11-11. The sandbox in the article is `luaL_newstate()` + `luaopen_jit` + `luaL_dofile("evil.lua")` (bytecode file), and the result is native code execution. Confirmed.
* Luanti: GHSA-g596-mf82-w8c3 = CVE-2026-41196, severity "critical", published 2026-04-14: "only exploitable when using LuaJIT", workaround `getfenv = nil`. Fix commit `8a929dfb97` (2026-03-19), "Lua API: Sanitize the environment of safe functions", touches `src/script/cpp_api/s_security.cpp`. NVD gives CVSS 3.1 10.0 (Primary). CVE-2026-40959: NVD 9.3 (Secondary), status "Deferred", text "Luanti 5 before 5.15.2, when LuaJIT is used, allows a Lua sandbox escape via a crafted mod". The text does **not** name getfenv; it is probably a duplicate record of the same issue.
* CVE-2019-19391 (9.1): NVD tag **`disputed`**. "The LuaJIT project owner states that the debug libary is unsafe by definition and that this is not a vulnerability". The affected version is "through 2.0.5". This supports "never expose LuaJIT's debug library", but it is not an undisputed 2.1 vulnerability.
* CVE-2024-25176 (9.8, stack overflow in `lj_strfmt_wfnum`), -25177 (7.5, unsinking DoS), -25178 (9.1, OOB read in stack-overflow handler): all confirmed on NVD, published 2025-07-07.
* OpenComputers: `application.conf` `allowBytecode: false` ("MASSIVE SECURITY RISK ... running arbitrary code on the real server") and `allowGC: false` ("hooks are disabled while they run"). In `machine.lua:755-756`, the sandbox `load` forces `mode = "t"`. `LuaStateFactory.scala:414-431` sets `unpack`, `loadstring`, `dofile` and `loadfile` to nil. Confirmed. (The investigator cited lines 754-759; the actual lines are 754-760.)
* CraftOS-PC ROM `bios.lua:44-58`: `load(x, name, mode, env)` calls `nativeloadstring(x, name)` and ignores `mode`. Confirmed.
* **Experiments:**
  * `sec/getfenv.lua`: in LuaJIT, inside a `setfenv` sandbox, `getfenv(print) == host _G` and `getfenv(getfenv) == host _G` are both **true**. The vector is real. For CCLuaJIT it only matters if the per-computer `_G` is not the same table as the user-visible env. That holds today: libs are opened into the computer's own `_G`.
  * `sec/fuzz.lua`: dump a small function, overwrite 2 random bytes (offset >= 8), `loadstring`, `pcall`. Over 300 seeds, **41 segfaulted (rc=139), 13.7%**. A JVM process would crash outright.
  * Current code: `git show HEAD:src/main/cpp/main.cpp` lines 707-711 call `luaopen_base`, `luaopen_math`, `luaopen_string`, `luaopen_table` and `luaopen_bit`. So `load`/`loadstring` accept binary chunks and `string.dump` is present. `debug`, `jit`, `ffi`, `io`, `os` and `package` are not opened. `collectgarbage`, `gcinfo` and `newproxy` are set to nil (lines 730-735).

---

## PA-5: CraftOS-PC Accelerated

All checked through the GitHub API (`$W/web/cospc-releases.json`, `cct-tests-*.patch`, `cospc-*.cpp`, `cl_lj_api.c`, `cospc-rom-bios.lua`, `cospc-accelerated.md`, `cospc-changelog.md`):
* v2.2.1-luajit (2019-12-29) release notes: "setting the hook count to 1 ... almost 75% of the CPU time", `os.setHaltOnLongRunMode(false)`. Confirmed.
* v2.4.2-luajit (2020-09-08) and `docs/changelog.md:890`: "Removed `os.setHaltOnLongRunMode`, count hooks are no longer used". Confirmed. On the `luajit` branch, `src/runtime.cpp` (392 lines) has no `eventTimeoutEvent`, `lua_halt` or "Too long" text, unlike `master` (`runtime.cpp:184-222`). The only `lua_sethook` calls are for the debugger and Ctrl-T (`Computer.cpp:174-189`, `runtime.cpp:196`).
* `craftos2-luajit` `src/lj_api.c:1315-1317`: `lua_halt` is `// do nothing; halting LuaJIT is much more complex than PUC Lua`, added in 5cd001e. Confirmed.
* The `luajit`-branch `CCT-Tests.patch` (451 lines) deletes `timeout_spec.lua`, `coroutine_spec.lua` (334 lines), `call_spec.lua` (`__name`) and `vm_spec.lua` (2^19 unpack), and removes the `textutils.serialise` ordering asserts. Confirmed.
* 5cd001e `lib_bit.c`: `fesetround(FE_TOWARDZERO)` with the comment "The LuaJIT BitOp library uses the FPU's rounding mode ... incompatible with bit32's behavior, which is to floor (truncate)". Confirmed.
* Apple Silicon: `Computer.cpp:263-265` (`#warning JIT compilation is disabled by default on Apple Silicon`) and ROM `bios.lua:21-23`. Confirmed.
* Speed claims: "8-16x" in the v2.4.1-luajit notes and "over 30x faster ... in some scenarios" in `docs/accelerated.md:2`. Neither has a method.
* Last release is v2.8.3-luajit (2024-08-17T06:44Z), which is also the newest release of the whole repo. The `luajit` branch's last commit is 2024-08-17. **Correction:** the `craftos2-luajit` fork (default branch `v2.1`) has commits `aef3104` (2026-04-16, "Fixed `utf8.char`") and `ffce6c9` (2026-06-09, "Enable Lua 5.2 compat for Windows"). So it is not completely dormant, although no Accelerated release has used these commits.

---

## PA-6: `bit32 = bit` is semantically wrong

* `git show HEAD:src/main/cpp/main.cpp`: line 711 `luaopen_bit(L);`, lines 713-714 `lua_getglobal(L, "bit"); lua_setglobal(L, "bit32");`. This matches the investigator's citation of 711-714.
* Reference behaviour comes from **Cobalt 0.9.9 itself**, not from PUC 5.2. Harness: `$W/bit/CobaltBit.java` (`LuaState.builder().build()`, `CoreLibraries.standardGlobals`, `Bit32Lib.add`). Cobalt converts with `(int)(long)v` (`LuaDouble.java:167`), which truncates, and returns unsigned values (`Bit32Lib.java:163`). Outputs: `$W/bit/cobalt-out.txt` vs `$W/bit/luajit-alias-out.txt`.

| expression | CC:T (Cobalt) | LuaJIT with `bit32 = bit` |
|---|---|---|
| `bit32.bnot(0)` | 4294967295 | -1 |
| `bit32.band(2.7,255)` | 2 | 3 |
| `bit32.band(3.5,255)` | 3 | 4 |
| `bit32.band(-1.5,255)` | 255 | 254 |
| `bit32.bor(0x80000000,0)` | 2147483648 | -2147483648 |
| `bit32.lshift(1,32)` | 0 | 1 |
| `bit32.rshift(1,-1)` | 2 | 0 |
| `bit32.arshift(-8,1)` | 4294967292 | -4 |
| `bit32.band()` / `bor()` | 4294967295 / 0 | error "number expected" |
| `bit32.btest/extract/replace/lrotate/rrotate` | function | **nil** |
| `bit32.tobit/tohex/rol` | nil | function (extra) |
| `bit32.band(0xFFFFFFFF,0xFFFFFFFF) == 0xFFFFFFFF` | true | false |

* ROM impact: `rom/apis/textutils.lua:436` calls `bit32.extract` (JSON `unicode_strings`, non-BMP). `bios.lua:21-29` builds the legacy `bit` API from `bit32.*`, so `bit.brshift`/`bit.bnot` also become signed. User programs such as hash libraries and colour or bitmask code depend on unsigned results and on `rrotate`/`extract`. The fix: a real `bit32` implementation that matches Cobalt (truncation toward zero, mod 2^32, unsigned results, Lua 5.2 shift-range rules).

---

## Issues the investigator missed (summary)
1. **Naive hook-yield pause breaks CC:T semantics** (high, verified on Windows and Linux): spurious `coroutine.resume` returns, lost events (8 or 6 of 20 received), "attempt to yield across C-call boundary" errors in sort/gsub callbacks, and xpcall-handler termination. CC:T always nests user code (`bios.lua:731`, `parallel.lua:78,91`). `computer_threads` defaults to 1 (`ConfigSpec.java:167`), so pausing is needed for fairness.
2. **Loops inside a user debug hook cannot be interrupted** (high if `debug.sethook` is exposed, as CC:T does): `debug.sethook(function() while true do end end, '', 1)` survives CHECKHOOK plus re-arming every 1 ms on both OSes, because `callhook` skips while `HOOK_ACTIVE` is set (`lj_dispatch.c:362`). `while true do debug.sethook() end` escapes a single arm, so the watchdog must re-arm periodically.
3. **Cross-thread arming crashes and permanently loses interrupts**: shown by experiment (see PA-2). This upgrades PA-2 from "not proven" to verified.
4. **Hard-abort `close()` runs on the monitor thread** while native code may still be running (`ComputerExecutor.java:254-261`). Combined with the old `close() -> lua_close`, this is a use-after-free that would crash the JVM.
5. **Error position**: a `luaL_error` raised in the hook usually lacks the `file:line:` prefix that CC:T's `timeout_spec` requires (`^.+:%d+: Too long without yielding$`). My messages for main-chunk loops had no prefix, and cases 10 and 11 did. This is low severity; use `luaL_where(L, 0)` or an equivalent.
6. **bit32 aliasing is broader** than the investigator stated: missing functions, shift semantics, and zero-argument behaviour (see PA-6).

## Artifacts
* Results: `$W/abort-results.txt`, `$W/pause-results.txt`, `$W/pausecorrect-results.txt`, `$W/race-results.txt`, `$W/race-spin-results.txt`, `$W/linux-results.txt`, `$W/bench/bench-results.txt`, `$W/bit/cobalt-out.txt`, `$W/bit/luajit-alias-out.txt`, `$W/sec/fuzz-crashes.txt`.
* Sources: `$W/src/{vtest.c,race.c,race-spin.c,pausecorrect.c,linux-run.sh}`, `$W/bench/kern.lua`, `$W/bit/{exprs.lua,CobaltBit.java,run-lj.lua}`, `$W/sec/{fuzz.lua,getfenv.lua,pat.lua}`.
* Fetched web data: `$W/web/` (LuaJIT issues #723/#779/#824/#1012/#1230/#666/#1488 JSON plus `issues.txt`, `c824.json`, Tarantool `tt-luajit.cmake`/`tt7762*.json`, NVD JSON per CVE, `faq.html`, `lual1.html`, `lual2.html`, `corsix.html`, CraftOS-PC files, OpenComputers `oc-application.conf`/`oc-machine.lua`/`oc-LuaStateFactory.scala`, `timeout_spec-mc-1.2*.lua`).
* Builds: `$W/lj-{plain,ck,ckassert}` (Windows), `$W/lj-linux-{plain,ck}` (WSL). Binaries are in `$W/bin`, `$W/binl`.
