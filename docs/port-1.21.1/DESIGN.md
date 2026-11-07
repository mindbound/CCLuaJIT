# CCLuaJIT 1.21.1: technical design

*Companion to [PLAN.md](PLAN.md), version 2 (2026-11-07). It contains the review corrections and the results of the pause, patch 07 and patch 08 prototypes. P-numbers refer to the problem table in PLAN.md §3. "Prototyped" means the code exists in `artifacts/` and was tested outside CC:T. "Design" means nothing has been built yet.*

```
 CC:T ComputerThread worker ──handleEvent()──► LuaJITMachine (Java, common module)
                                                   │  lean JNI: cached IDs, Push/PopLocalFrame per call,
                                                   │  Latin-1 strings via byte[], lazy IArguments, handle table
                                                   ▼
                              libcclj (one static native library per platform, plain C)
                              ├─ host core: state + counting allocator (per-computer and global caps),
                              │  curated stdlib/debug/registry, coroutine wrapper (nesting limit, pause),
                              │  interrupt/abort state machine, protected boundary, conversions,
                              │  method tables, callback trampoline, dead-handle ring
                              └─ LuaJIT v2.1 cclj/v2.1 (14d8a7a2 + 17 backports) + patches 01–08 (+09–11 planned) (static)
 CC:T Monitor / server thread ──TimeoutState listener──► cclj_request(PAUSE|SOFT|HARD)
                                                          └─► luaJIT_interrupt(L)  (thread-safe, two stores)
 per-process ticker (250 µs) ──► re-poke machines whose pause is deferred
```

---

## 1. Mod / loader layer

- **Multiloader Gradle project.** Modules: `common` (plain Java with all the machine code and no Minecraft dependency), `neoforge`, and later `fabric`. Decision 1 makes NeoForge the only loader until M4. Keep the `common`/`neoforge` split from day one, so that Fabric can be added later at almost no cost. Java package `io.github.mindbound.cclj`, Maven group `io.github.mindbound`, mod ID `ccluajit` (decision 10). Each loader module is about 10 lines and sets `ServerContext.luaMachine` at init. No Mixin is needed, because CC:T reads the field at server start, after all mod init. A compiling scaffold is in `artifacts/scaffold-multiloader`.
- **Versions (proven to build).**
  - NeoForge: ModDevGradle 2.0.148, NeoForge ≥ 21.1.265 (CC:T's requirement). CC:T's `cc-tweaked-1.21.1-forge` Maven artifact *is* the NeoForge build.
  - Fabric: Loom 1.19.4 + Gradle 9.8.2 on **JDK 25**, compiling with `--release 21`. JDK 25 is needed only because CC:T 1.121.0's Fabric jar was built with Loom 1.18. A JDK-21-only fallback is Loom 1.17 with CC:T 1.120.2 as the dev-runtime mod.
- **Server-side only.** CCLuaJIT registers nothing and only replaces the server's machine factory, so clients do not need it. NeoForge 21.1 no longer has a `displayTest` key (FML 4.0.x does not read it). The mod is client-optional simply because it registers no blocks, items or network payloads. M0 checks this in game: a client without CCLuaJIT joins a server that has it. Fabric (later): environment `*` with no client entrypoint. Single-player uses the mod through the integrated server.
- **Factory wrapper with fallback.** The installed factory is a wrapper that decides on every `create()`. It uses LuaJIT if the natives loaded, `enabled = true`, and the computer is not in `cobalt_computers` or quarantined. Otherwise it delegates to *whatever factory was installed before ours* (normally Cobalt). This makes the config switch and the admin kill-switch take effect without a restart: the next computer boot follows the new setting. The computer ID comes from finding `OSAPI` in `env.apis()`.
- **CC:T compatibility policy.** `ILuaMachine` and `ServerContext.luaMachine` are not public API. Note that 1.115 → 1.121 already changed `IArguments.escapes()` in a binary-incompatible way.
  - Declare `computercraft >= 1.121.0` with **no hard upper bound**, so a modpack update of CC:T does not stop the server from starting.
  - At mod init, run a reflective **contract probe**: the `ILuaMachine` / `Factory` signatures, the `MachineEnvironment` record components, `IArguments.escapes()` returning `void`, `ServerContext.luaMachine` being public static and non-final, `LuaMethod.apply`, and the `MethodResult` accessors. On any mismatch, log an ERROR naming the CC:T version and leave Cobalt in place.
  - On the first native load, compile (not run) every ROM `.lua` file with the LuaJIT parser. Any failure means CC:T added syntax we don't support, so fall back to Cobalt with a clear log message.
  - At `SERVER_STARTED`, check that `ServerContext.luaMachine` is our factory. If it is not, log WARN "another mod replaced the Lua machine factory (<class>); CCLuaJIT inactive". Known cases: Minecraft-Modding-Libs' test harness, and CC:T's own gametests.
  - If mods that mixin into `CobaltLuaMachine` are loaded (CCGraphics, cride), log WARN that they will not see LuaJIT computers, and list them in the documentation.
  - A weekly CI canary runs the headless test suite against the newest CC:T release for 1.21.1.
  - (An alternative is a tight range `[1.121.0,1.122)` on both loaders. It is safer against silent breakage, but every CC:T update then blocks server start until CCLuaJIT ships an update. The probe + fallback approach is recommended.)
- **Native loading.** This is what `artifacts/scaffold-multiloader/.../NativeLoader.java` does; it was tested with 12 concurrent JVMs and with a locked DLL on Windows.
  - Extract to `<gamedir>/.ccluajit/natives/<first 16 hex of sha256>/<lib>`: write a private temp file, publish it with `ATOMIC_MOVE`, and re-hash an existing file before reusing it.
  - Delete other hash directories best-effort, and ignore failures such as a DLL that is still loaded on Windows.
  - Additions:
    - **musl detection** (Alpine images): `/proc/self/maps` contains `ld-musl`, or `/lib/ld-musl-*.so.1` exists. If so, skip the natives and log INFO.
    - On `noexec` or "failed to map segment" errors, log a hint to move the game directory or set `-Dccluajit.nativeDir`.
    - If the extracted file disappeared or is locked, log "native library may have been quarantined by antivirus; see <docs>; falling back to Cobalt". LuaJIT-based malware loaders are a real 2024–2026 campaign family, so antivirus false positives are likely. See PLAN §6.
    - The README documents the cache path `<gamedir>/.ccluajit/natives/`, so that admins can add an antivirus exclusion.

## 2. Native layer: one static library per platform

- LuaJIT is statically linked into `cclj`. On Windows the build uses `-static-libgcc -static-libstdc++ -static`, and the result imports only `KERNEL32` and `msvcrt`. This is proven on Windows and Linux.
- **Write the glue in plain C, not C++.** On MinGW, Lua errors skip C++ destructors and `catch(...)`. On Linux g++, `catch(...)` *swallows* Lua errors, including the abort. Never rely on RAII or `catch` across Lua calls.
- **Build flags:**
  `BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT"`.
  `DISABLE_PROFILE` and `DISABLE_VMEVENT` are defence in depth: the sandbox removes `jit` anyway, and this also avoids SIGPROF clashes with spark/async-profiler. For test builds add `-DLUA_USE_ASSERT -DLUA_USE_APICHECK`. Keep the `LUAJIT_SECURITY_*` defaults, and at state creation assert that `jit.security()` reports prng, strhash and mcode all on, before `jit` is removed. `natives/luajit-patches/build-patched-luajit.sh` builds all of this, `natives/build-natives.sh` links `libcclj`, and `natives/tests/run-harnesses.sh` runs every harness (M0). The copies under `artifacts/luajit-patches/` are the evaluation record: their build script silently skips patches 01, 07 and 08 when its output directory is inside a git work tree (see `natives/luajit-patches/README.md`).
- **Licence notices** ship next to each library (`natives/<platform>/*.txt` in the jar and in the CI artifacts): LuaJIT's `COPYRIGHT` (LuaJIT, Lua and dlmalloc), and for the Windows DLL the mingw-w64 runtime notices, because `-static` links mingw-w64 code into it (`gdtoa`, math and startup files; seen in the link map). The texts are in `LICENSES/`.
- **Patch stack** (canonical copy in `natives/luajit-patches/`; details in `artifacts/luajit-patches/README.md`):

  | # | Patch | Status |
  |---|---|---|
  | 01 | `luaJIT_interrupt()`: thread-safe flag that traces check | done, tested (Windows and Linux x64) |
  | 02 | errors raised from hooks skip `xpcall` handlers | done, tested |
  | 03 | every loader refuses bytecode | done, tested |
  | 04 | NUL bytes in patterns | done, tested |
  | 05 | `%.15g` / `%.17g` round-trip number formatting | done, tested; costs `tostring(float)` 2.6x (§PLAN 2) |
  | 06 | `goto` usable as an identifier | done, tested |
  | 07 | interruptible pattern matching and `table.sort`; linear-time plain `find` | done, tested |
  | 08 | C-stack guard: `luaJIT_setcstacklimit()`, catchable "C stack overflow" | done, tested |
  | 09 | `LUAI_MAXCSTACK 65000`: lets `unpack` / `string.byte` return up to ~30k–64k values instead of 8,000 | one line; tested in a probe build, not yet in the stack |
  | 10 | `tostring` / `%p` print a hashed ID, not a heap address | design |
  | 11 | lexer polls the interrupt every 64 KiB of input (an unpolled `load` took 1.0 s for 20 MB and 1.5 s for 38 MB) | design, small |
  | 12 | lexer parity with Cobalt: accept unknown escapes as Cobalt does; reject `0b` literals, a UTF-8 BOM and identifiers with bytes above 0x7F (PLAN P13) | design, small; check against Cobalt with `head-kit/difftest` |

- **LuaJIT base: `cclj/v2.1` = upstream `14d8a7a2` (2026-07-18) + 17 cherry-picked upstream fixes** (decision 5, evaluated 2026-11-07; `reports/pin-backports.md`, `artifacts/luajit-patches/BACKPORTS.md`).
  - `14d8a7a2` is the last v2.1 commit before upstream's v3.0-syntax backport `a2ce8114` (2026-07-20), which adds `continue`, `?.`, `??`, `+=`, bit operators, `const` and short functions. CC:T rejects all of these, and upstream has no switch to disable them.
  - `14d8a7a2` is `3c4f9fe2` + one x64 backend fix, with identical harness results.
  - **The 17 picks** come from the 45 upstream commits up to `ea9eb7ed` (2026-10-30):
    - 10 fixes that matter for x64 now: OOM during trace stitching, `next()` despecialisation, `#t` after a rehash, stale `math.floor()`/`ceil()` arguments, ±0 for-steps, `ldexp` folding, Windows `tonumber(s, 16)`, register allocation, `select()`, and the string.buffer recorder;
    - 5 ARM64/macOS fixes, inert on x64;
    - 2 sync commits.

    A JIT-vs-interpreter check (`tools/fixcheck.lua`) confirms 5 of these bugs on the base, on Windows and Linux; the branch passes 6/6.
  - 9 performance commits are **deferred** until they are benchmarked on CC workloads. They apply cleanly as a group (`backports-full/`).
  - Skipped: syntax, FFI-only and unsupported-platform commits.
  - Recreate the branch with `git checkout -b cclj/v2.1 14d8a7a2 && git am backports/0*.patch`; that gives a tree identical to the evaluated tip `a8f03353`. Patches 01–08 apply on top, and every harness gives the reference results. The new syntax is still rejected (`tools/syntax_probe.lua`, 45/45).
- **LuaJIT is a maintained fork.**
  - Host `cclj/v2.1` in a LuaJIT fork under your GitHub account and add it to the mod repo as the `vendor/luajit` submodule. Patches 01–08 stay in the mod repo and are applied at build time.
  - Triage upstream v2.1 commits monthly with the procedure in `BACKPORTS.md`, and immediately when upstream touches the files patches 01–08 edit. Measured rate since the pin: about 12 upstream commits a month, of which about 2.7 are relevant fixes (plus about 1.4 for ARM64/macOS). They arrive in bursts. Budget 0.5–1 day per month.
  - **Not chosen for now:** tracking upstream HEAD with a lexer/parser "syntax gate" (`artifacts/luajit-patches/head-kit/`, `reports/pin-head-gate.md`). It works today: on `ea9eb7ed` + ported 06 + gate it showed 0 differences from the pinned build over 113,872 parse inputs, with harness parity. But it lives in code upstream is actively rewriting (4 lexer/parser commits in 3.5 months, against about 3 a year before). A token-level gate also missed two parser regressions that the differential test caught. Keep the kit for a one-step pin bump once upstream's parser settles.
- **Linux natives** must be built in a `manylinux_2_28` container (glibc 2.28 baseline). Built on a current distro, they need glibc 2.38 and fail on Debian 12, Ubuntu 22.04 and RHEL 8/9. Locally, the WSL **AlmaLinux 8** distro (glibc 2.28) is the same base as the `manylinux_2_28` image. Build there, then run the result on WSL **Ubuntu 26.04** (glibc 2.43): that covers both the oldest and the newest glibc. The CI image compiles with a `gcc-toolset` GCC, while Alma's system compiler is GCC 8.5; install `gcc-toolset-14` there to match CI.
- **macOS:** build with and without `-DLUAJIT_ENABLE_OSX_HRT` (MAP_JIT under the hardened runtime; present in the base but not in `19878ec0`). Ship the variant that passes gate G5. The smoke test must check that the JIT is actually on.
- **CI:** GitHub Actions native runners. Required until M4 (decision 4): windows and ubuntu x64 (manylinux). Informational only until M4: ubuntu arm64, macos-15-intel and macos-15 arm64. The workflow is `.github/workflows/build.yml` (M0, replacing the draft in `artifacts/scaffold-multiloader/`). In week 1, run a harness-only job (no JNI) on all four Unix and Windows targets. The POSIX shim `harnesses/preempt/win32_on_posix.h` already makes the preemption harness pass 25/25 on Linux x64. Patch 01 has **never run on arm64**, so CI is the first place it will.

## 3. Host core: the safety-critical part

### 3.1 Threads, locks and lifecycle (P4)
- CC:T calls `close()` and `printExecutionState()` from the **Computer-Monitor thread** while `handleEvent` runs on a worker, and calls `close()` repeatedly (about 30 times after one hard abort, measured). The first hard abort in M1 will hit this, so the lifecycle design belongs in M1.
- **Lock order:** `TimeoutState` monitor → `machine.lock`, never the reverse. CC:T runs `TimeoutState` listeners while holding the `TimeoutState` monitor (`refresh`/`reset`/`startTimer` are `synchronized` and call `updateListeners`), and `removeListener` needs that monitor too. Holding `machine.lock` while calling `removeListener` can therefore deadlock the Monitor thread. After that, no computer on the server can be timed out again.
- **`close()`:** idempotent. Steps:
  1. Call `env.timeout().removeListener(listener)` **without** holding `machine.lock`. When it returns, no listener call is in flight.
  2. Take `machine.lock`, set CLOSED, and request a HARD interrupt.
  3. Return. The **owning worker thread** frees the `lua_State` when `handleEvent` unwinds, or immediately if no call is running. `CobaltLuaMachine.close()` is the model: set a flag, interrupt, remove the listener.
- **Listener body:** takes only `machine.lock` (or does a CAS on the native-pointer field), performs the two stores of `luaJIT_interrupt`, and never calls back into CC:T.
- **`printExecutionState()`:** never touches the VM. The count hook stores the last `chunk:line` in an atomic per machine, and `printExecutionState()` prints that, sanitised.
- Test: a stress harness calls `close()` from 4 threads against `refresh()`/`reset()` at 1 kHz for 10 minutes, under the real `ManagedTimeoutState`. A watchdog asserts that the Monitor thread keeps ticking.

### 3.2 Interrupt protocol (P1, P2, P3)
- One count hook stays installed per VM (count 10⁴). It is set on the VM thread. **Nobody calls `lua_sethook` from another thread.**
- The `TimeoutState` listener (any thread) does `req |= what; luaJIT_interrupt(L)` (§3.1).
- On the next hook run:
  - **SOFT:** clear the bit and raise a catchable `luaL_where(L, 0) .. "Too long without yielding"`. In Lua code that gives `chunk:line: Too long without yielding`, which CC:T's `timeout_spec` requires for loops. When the hook runs inside a C function (patch 07, see below), `luaL_where` is empty. That gives exactly `Too long without yielding`, which `timeout_spec` requires for `string.find`/`match` (verify in the suite).
  - **HARD:** set `aborting`, re-arm at count 1, then `lua_yield` if yieldable, otherwise raise an error. `pcall` cannot catch it: wrappers that see `aborting` yield or raise immediately. The host reports `TIMEOUT` and frees the state. Measured latency: at most 42 µs, including `pcall` loops and a looping `xpcall` handler.
  - **PAUSE:** §3.3.
- **Hook calls from C (patch 07).** The pattern matcher and `table.sort` poll the flag and run the count hook inside a protected, non-yieldable C frame (`lua_isyieldable() == 0`). Errors work there, but yields are refused. SOFT and HARD work unchanged. A PAUSE stays pending (§3.3).
- **C stack (patch 08).** Before every `lua_resume` on a CC:T worker, call `luaJIT_setcstacklimit(L, lo, lo + margin)`. Get `lo` from `GetCurrentThreadStackLimits` (Windows), `pthread_getattr_np` (Linux) or `pthread_get_stackaddr_np − pthread_get_stacksize_np` (macOS). Cache it per thread, because CC:T moves computers between workers.
  - Margin: **128 KiB**, configurable. Measured on x64 with 4 KiB pages: 32 KiB is enough natively, and 64, 128 and 256 KiB all worked inside a JDK 21 JVM with JNI upcalls at every level.
  - HotSpot's guard and shadow zones scale with page size: 16 KiB on macOS arm64, 64 KiB on some aarch64 kernels. The margin therefore has to be **measured on those systems** (CI gate G0) before shipping. A page-size formula like max(256 KiB, 32 × page size), as one reviewer suggested, cannot work with 64 KiB pages: it would reserve 2 MiB on a ~1 MB stack.
  - On Linux, each nested `gsub` callback costs ~9 KiB of C stack (glibc's 8 KiB `luaL_Buffer`). A 1 MB thread therefore allows only ~100 nested `gsub` callbacks. That is fine; the ROM nests ~6–10 deep.
- **Hook-disabled contexts (P3).** `__gc` finalizers and user `debug.sethook` functions run with hooks off. Fix: no `newproxy`, and no native `debug.sethook` (§3.5). `xpcall` handlers are handled by patch 02.

### 3.3 Coroutine wrapper and pause token protocol (P8, P10) — prototyped
The prototype is `artifacts/pause-prototype/cclj_co.c`. It was tested on Windows and Linux, with the JIT on and off, and with an assert build.
- **F1, nesting limit (needed even without pause).** `coroutine.resume` and `coroutine.wrap` are replaced by a C `raw_resume` with a 12-line Lua continuation. `create`, `yield`, `status`, `running` and `isyieldable` stay native.
  - `raw_resume` checks status exactly like LuaJIT's `ffh_resume` and refuses the main thread. Its messages use Cobalt's wording ("cannot resume dead coroutine").
  - It enforces a nesting **count limit of 200** and a **stack-pointer floor** (margin + 128 KiB). On Linux the floor, not the count, is what stops `gsub`-mixed nesting.
  - `wrap` re-raises child errors unchanged, as Cobalt does.
  - Cost: +13–15 ns per resume (1.3–1.7x on coroutine ping-pong microbenchmarks, ~0% on real workloads).
- **F2, pause:**
  1. Hook PAUSE: if no non-yieldable resume is active (`nonyield == 0`) and `lua_isyieldable(L)`, clear the bit, set `pausing`, and `lua_yield(L, 0)` (a hook-yield, transparent on resume).
  2. Otherwise **defer**: leave the bit pending and do **not** re-arm the interrupt. Re-arming made JIT code inside `sort`/`gsub` callbacks 4–7x slower.
  3. A wrapper whose child returns `LUA_YIELD` while `pausing` is set does `settop(child, 0)` and `return lua_yield(L, 0)`. This repeats up to `lua_resume(top)`, which becomes `MachineResult.PAUSE`. The event filter is kept.
  4. `handleEvent(null, null)` clears PAUSE and `pausing` and resumes the top with zero values. Each wrapper's continuation sees zero values (a real resume always returns at least `true`/`false`) and re-resumes its child, down to the hook-yielded coroutine.
  5. A per-process ticker re-pokes (`luaJIT_interrupt`) running machines that have a pending deferred pause, every 250 µs. Neither the ticker nor the listener may write shared memory in a tight loop: a spinning writer on another core made the VM 3–5x slower on Linux. Keep the shared fields on their own cache line.
  6. Optional checkpoint wrappers around sandbox `xpcall` and `load(reader)` call a C `checkpoint()` right after the native call. This cuts worst-case pause deferral from ~0.5 s to 6–17 ms.
  7. HARD while paused → `TIMEOUT` without resuming.
- **Measured:**
  - The ROM `parallel` scheduler ran through 2,719 pauses (61,575 at 20–60 µs intervals) with byte-identical results and no lost or extra events. 7 nesting levels all paused. A naive hook-yield fails the same test in 8 ms.
  - Each pause costs ~1–2.5 µs.
  - Pauses inside non-yieldable C callbacks are deferred; that is correct, but costs fairness only. `coroutine.isyieldable()` returns false inside C callbacks (Cobalt returns true).
- **Integration check:** the prototype was developed on patches 01–06. It was then re-run on LuaJIT `3c4f9fe2` + 01–08 with the final flags, and all 7 suites (46 sub-tests) passed (`artifacts/pause-prototype/logs/run_tests-3c4f9fe2-01-08.out`). With patch 07, the hook can also run inside C pattern matching; there `lua_isyieldable()` is 0, so a PAUSE is deferred under the same rule. Patch 07's own notes suggest re-arming deferred pauses with `luaJIT_interrupt`. The pause prototype measured that this slows JIT code inside callbacks 4–7x, so use the ticker instead.

### 3.4 Protected boundary (P5)
- Every native entry point runs Lua work under `lua_cpcall` / `lua_resume`. Nothing can raise outside a protected frame, because on Windows that is a fatal SEH `0xE24C4A02` and on Linux it is `PANIC` + `exit(1)`, which skips Java shutdown hooks.
- **JNI phase rule:**
  1. Gather Lua values into C buffers under protection.
  2. Make all JNI calls with no Lua API calls in between.
  3. Release arrays and pop the local frame.
  4. Push the results under protection.

  Never hold `Get*Critical` across any call. Never `longjmp` across a JVM frame. Check `ExceptionCheck` after every JNI call. A pending Java exception becomes a Lua error only after all JNI work is done.
- Conversions are iterative, with depth limits and identity-preserving cycle maps (§4.3).

### 3.5 Curated environment (P6, P7, P15)
- **Libraries:**
  - base without `dofile`/`loadfile`/`collectgarbage`/`newproxy`/`print`/`__inext`;
  - `coroutine` (wrapper, §3.3);
  - `string` without `dump`;
  - `table` with `pack`/`unpack`/`move`;
  - `math`;
  - C `bit32` (Cobalt semantics);
  - `utf8` and `string.pack`/`unpack`/`packsize`;
  - curated `debug`;
  - `_CC_DEFAULT_SETTINGS`;
  - **identity (decision 6, accepted; `reports/version-survey.md`):**
    - `_VERSION = "Lua 5.2"` exactly, as on stock CC:T.
    - `_HOST` = `env.hostString()` + ` (CCLuaJIT <version>; LuaJIT <jit.version number>)`, plus `; JIT off` when the config disables the JIT. Only the LuaJIT machine adds the tag, so computers on the Cobalt fallback keep the stock string.
    - A plain `jit` table holding only `version` and `version_num`, created per state before the bios runs. The same table is `_LOADED.jit`, so `require "jit"` works. It has no functions, no `os`/`arch`, and nothing from the real `jit` library.
    - LuaJIT's BitOp library as `_LOADED.bit` only, so `require "bit"` works. The global `bit` stays the CC ROM's stub, and `_G` gains no new name;
  - `_ENV` emulation.

  Not present: `io`, `os`, `package`, `ffi`, the real `jit` library, and LuaJIT's global `bit`.
  - The JIT is switched on through `luaopen_jit`. Then the real `jit` is removed from `_G`, `_LOADED`, `_PRELOAD` and `_VMEVENTS`, and replaced by the two-field identity table above.
  - Why the identity is designed this way:
    - CC programs detect LuaJIT with `if jit` (AUKit, sanjuuni's video player). Seeing it, they switch to chunk sizes of 7,680–8,000 values, which fit LuaJIT's result limit (PLAN P13).
    - No CC program looks for LuaJIT in `_VERSION`. Every distinct-but-compatible `_VERSION` string tried changes at least one real check (`== "Lua 5.2"`, `:sub(-3)` in compat53, `tonumber(_VERSION:sub(5))`).
    - A separate trailing `(…)` tag in `_HOST` parses the same as the existing UnBIOS suffix in all 20 real-world parsers that were checked.
    - Egor Skriptunoff's widely vendored `sha2.lua` detects LuaJIT by a VM quirk and then calls `require "bit"`, so it needs `_LOADED.bit`.
- **`bit32`, `utf8` and `string.pack` must be C.** The pure-Lua `bit32` in `artifacts/lua/cclj_prebios.lua` is *slower than Cobalt* on full 32-bit values, because its loops abort JIT traces. With it, SHA-256 is only ~1.5x Cobalt instead of 22x. Start from CraftOS-PC's LuaJIT fork (`MCJack123/craftos2-luajit`, MIT):
  - `lib_utf8.c` (commits `de36407`, `aa5e292`);
  - `string.pack`/`unpack`/`packsize` from PUC 5.3 (`5cd001e`);
  - PUC 5.2 `bit32` (`5cd001e`).

  Watch for its `\u{...}` lexer bug (`c - 'F' + 10`). Credit it in `META-INF/licenses`.
- **Host-owned functions are opaque.** At load time, every C function and every function compiled from the host prelude chunk is recorded in a native weak set. The curated `debug` and base functions then enforce:
  - `debug.getinfo(...)`: option `'f'` returns nil for host frames, and `func` is never returned for them. `'S'`, `'l'`, `'u'` and `'n'` stay allowed, because the ROM needs them.
  - `debug.getlocal`/`setlocal` (thread or level form) refuse host frames. Without this, the parent coroutine can read the trampoline's private native resume function and handle out of a suspended coroutine. The probe did exactly that, and called it with a forged handle.
  - `debug.getupvalue`/`setupvalue`/`upvalueid`/`upvaluejoin` refuse host-owned functions. Without this, `debug.getupvalue(load, n)` returns the real, unguarded `load`.
  - `debug.getmetatable` returns nil (or the `__metatable` value) for every host-owned metatable. `debug.setmetatable` refuses host userdata.
  - `getfenv(f)` returns the sandbox `_G` for host-owned functions and for levels that land on a host frame. `setfenv` refuses them. This is the Luanti CVE-2026-41196 class (CVSS 10, "only exploitable when using LuaJIT").
  - At state creation, set the environment of every C and prelude function to the single sandbox `_G`. Never keep a richer "real" globals table around.
  - `debug.getregistry` returns a fake, sanitised registry.
  - There is no `debug.sethook`. User hooks would replace the host hook, and user hook functions cannot be interrupted. Making `sethook` virtual needs a design in which the user hook runs outside hook context; that is decision 7 in PLAN.
  - Every native entry point validates its arguments as if an attacker supplied them (handle generation, owning machine, current coroutine), because reachability alone is not a security boundary.
- **Randomness (P15).** LuaJIT seeds `math.random` with a fixed value, so every computer and every reboot gets the same sequence. Rednet uses `math.random(1, 2147483647)` as its message ID and drops IDs it has already seen. Computers that boot together therefore silently lose rednet messages.
  - Fix: before the bios runs, seed the VM PRNG from Java `SecureRandom` (a native `cclj_seed` with two 64-bit words).
  - Match Cobalt's argument errors (`math.random(0)`, `math.random(3,1)` → "interval is empty").
- **No address disclosure (P15, patch 10).** `tostring(table|thread|userdata|function)` prints `<type>: %08x`, a 32-bit hash of (address XOR a per-state SecureRandom key). That is Cobalt's format; it also replaces `function: builtin#N`. `string.format('%p')` raises "invalid conversion", as in Cobalt. Leaked heap addresses would turn any memory-safety bug into an exploit.

### 3.6 Memory budget (P9, P16)
- **Counting allocator wrapper** around `lj_alloc` (`lua_getallocf` / `lua_setallocf`, +2–5% cost; the prototype is `artifacts/harnesses/runtime/allocwrap.c`). It enforces two caps:
  - `per_computer_memory`, default 64 MiB;
  - `global_native_memory` across all LuaJIT states, default min(1 GiB, 25% of `Runtime.maxMemory()`), kept in one process-wide atomic counter.
- An allocation that would exceed either cap returns NULL, which becomes a catchable "not enough memory" in that computer. Keep a 1 MiB per-state reserve so that the GC can run (it is exempt from the cap but counted), and force a full GC after an OOM.
- **JIT machine code** is mmap'd outside the allocator, up to `maxmcode` (512 KiB per state by default). Set `jit.opt.start("maxmcode=256")` at state creation, and add `maxmcode × live states` to the global figure as an estimate.
- Keep the per-computer default small enough that any single linear C operation stays well under 0.5 s. Measured: a full GC of a 368 MB heap took 0.19–0.25 s, and `("x"):rep(2^30)` took 0.53 s.
- Document for container hosts (Pterodactyl, Docker): container limit ≥ `-Xmx` + JVM overhead + `global_native_memory` + `maxmcode × computers`. Exceeding the container limit means a SIGKILL of the whole server.

## 4. JNI bridge and Java machine

### 4.1 Contract
- Implements `ILuaMachine` + `ILuaMachine.Factory(MachineEnvironment, InputStream)`. The contract is unchanged between CC:T 1.115.0 and 1.121.0 apart from nullness annotations and `IArguments.escapes()` → `void`. The checklist is `reports/cct-contract.md` §11.
- API tables come from `env.luaMethods().forEachMethod(...)` (plus `IDynamicLuaObject`), with modules registered in `_LOADED`. Calls go through `LuaMethod.apply(target, env.context(), IArguments)`.

### 4.2 Calls, errors and callbacks (P12)
- `MethodResult` with an `ILuaCallback` (turtle commands, main-thread peripheral methods, `peripheral.call`) uses a **Lua trampoline**. The native code returns a marker plus a handle, and Lua yields and then calls the native resume, as CC:T's `ResultInterpreterFunction` does.
  - Prototyped in `reports/verify-cct-contract.md` (CT-4, `vtramp.c`): a callback that yielded 3 times completed.
  - Still to do: adjust error levels for the extra Lua frame, and implement `getErrorAdjust()`.
  - The trampoline is a host-owned function, so its locals are protected by §3.5.
- `LuaException` levels are honoured. Other `Throwable`s become `"Java Exception Thrown: …"`, logged rate-limited (§5).
- **Error values that escape the bios are stringified raw:** a string as-is, anything else as `<type>: <id>`, truncated to 64 KiB. Host code never runs metamethods (`__tostring`, `__index`) on them. Cobalt uses `rawToString`.

### 4.3 Conversions
- **Strings:** Latin-1, mapping bytes 0–255 1:1 to chars 0–255 (`'?'` for chars > 255 going Java→Lua). `byte[]` is copied verbatim. A `ByteBuffer` copies `remaining()` bytes from `position` (heap buffers via array + `arrayOffset`; direct buffers, which fs reads use, via `GetDirectBufferAddress`) and advances the position, as Cobalt does.
- **Numbers:** `Double` in both directions. `Map`/`Collection`/`Object[]`/method objects follow the rules below.
- **Lua→Java:** iterative and identity-preserving, so cyclic tables work. It is raw-only (`lua_rawget`/`lua_next`, never metamethods). Each call has a budget of `max_conversion_values` nodes (default 1,000,000); beyond it, raise `LuaException("table too large to convert")`. The `IArguments` contract explicitly allows that. Without a budget, 256 queued events × a big table can exhaust `-Xmx`.
- **Java→Lua:** iterative, with depth ≥ 512 (Minecraft NBT depth). The memory cap bounds the size under protection. A Java `Map` with a `null` or NaN key, or an unknown type, raises a normal Lua error. These used to be JVM-killers.
- **`IArguments`:** lazy over a native argument snapshot, like CC:T's `VarargArguments`, so a table is converted only when `get`/`getTable`/`escapes` asks for it.
  - Override `getType` (honours `__name` via a raw lookup on the metatable), `getStringCoerced`/`getBytesCoerced` (Lua-style numbers and `table: <id>`, never Java `hashCode()`), and `getTableUnsafe`. `escapes()` converts eagerly in place.
  - This avoids the `term.write(cyclicTable)` → `StackOverflowError` → stack-trace-per-call log flood that eager conversion causes.

### 4.4 Java object lifetime (P14)
CC:T relies on Java GC to clean up after programs that forget `close()`. `FileSystem.openFiles` uses `WeakReference`s against `maximum_open_files` (default 128), and websockets use `GlobalCleaner`. LuaJIT tables have no `__gc`, and pinning handles until `close()` would leak them for the machine's lifetime.
- Each Java object converted to Lua (method table, `ILuaFunction`, `IDynamicLuaObject`) gets **one host userdata**. It holds a generation-tagged integer handle into a per-machine Java `Object[]` / `Int2ObjectMap`, and every method closure of that object captures it as an upvalue.
- There are no per-object JNI global refs and no per-machine global refs: pass `this` (a local ref) down each native call. After `close()`, nothing is left to leak.
- The userdata `__gc` is a C function. It only pushes the handle ID onto a native "dead handles" ring, which is preallocated outside the Lua allocator. It never calls JNI, never allocates and never raises.
- After every `handleEvent` and in `close()`, Java drains the ring and drops those references. CC:T's `WeakReference`/`Cleaner` logic then closes leaked files and websockets, as it does under Cobalt.
- A stale or forged handle (generation mismatch, wrong machine) raises a Lua error. Host metatables carry `__metatable`, and §3.5 keeps `debug.getmetatable` away from them. That blocks the `getmetatable(ud).__gc(ud)` use-after-free that the code audit reproduced.
- Tests:
  - open 1,000 files without `close()`, let them become garbage, and check that "Too many files open" never appears;
  - churn 10,000 http/websocket handles and check that the handle table stays bounded;
  - `debug.getmetatable(handle_ud) == nil`;
  - a forced handle reuse errors cleanly.

### 4.5 `MachineResult` mapping
- `OK` with the string event filter (the first yielded value, if it is a string);
- `PAUSE` (§3.3; keep the filter; the next call is `handleEvent(null, null)`);
- `TIMEOUT` after a hard abort;
- `error(msg)`;
- `GENERIC_ERROR` when the bios returns.

Never return `null`.

## 5. Observability, configuration and admin controls (WP K)

- **Metrics** (registered at mod init, before `ServerContext` / `ComputerMBean` exist):
  - `Metric.Event("luajit_memory", "bytes", Metric::formatBytes)`, the peak per `handleEvent`;
  - counters `luajit_soft_aborts`, `luajit_hard_aborts`, `luajit_oom`, `luajit_cstack_limit`, `luajit_coroutine_limit`, `luajit_abandoned`.

  Report them per computer through `env.metrics().observe(...)`, so that they show up in `/computercraft track` and over JMX.
- **Logging.** Every log line a player can trigger:
  - uses CC:T's `Logging.COMPUTER_ERROR` / `VM_ERROR` markers, so `log_computer_errors = false` silences it;
  - wraps untrusted text in `SanitisedError`-style escaping (200 chars, no control or ANSI characters);
  - includes the computer ID and label;
  - is rate-limited per computer (`log_rate_limit_seconds`).

  At startup, log a banner with: the CCLuaJIT version, the LuaJIT commit plus patch list, the `jit.security()` string, the platform, and the caps. Ship split debug symbols per release, so native frames in `hs_err` logs can be symbolised.
- **Config** (`config/ccluajit-server.toml`, server side; NeoForge `ModConfigSpec` SERVER type, plain TOML on Fabric):

  | Key | Default | Notes |
  |---|---|---|
  | `enabled` | `true` | live; applies at the next computer boot |
  | `jit` | `true` | live; applies at the next boot. `false` = interpreter only, still ~3.7x Cobalt. A safe fallback if a JIT miscompile is ever suspected. |
  | `per_computer_memory` | 64 MiB | |
  | `global_native_memory` | min(1024 MiB, 25% of `-Xmx`) | |
  | `max_coroutine_depth` | 200 | |
  | `max_conversion_values` | 1,000,000 | |
  | `jit_maxmcode_kb` | 256 | |
  | `cobalt_computers` | `[]` | computer IDs that always use Cobalt |
  | `auto_quarantine_after` | 1 | after N hard aborts that failed to stop within 1.5 s, that computer uses Cobalt until restart |
  | `log_rate_limit_seconds` | 60 | |

- **Admin command** `/ccluajit` (permission level 3):
  - `status`: natives, platform, security string, live states, global bytes;
  - `top [memory|aborts]`;
  - `dump <id>`;
  - `quarantine <id>` / `unquarantine <id>`;
  - `disable`: live; new boots use Cobalt. Optionally reboots LuaJIT computers.
