# CCLuaJIT LuaJIT patch stack

> **Superseded for building (M0, 2026-11-07).** The canonical patches and build script are in [`natives/luajit-patches/`](../../../../natives/luajit-patches/), and the harnesses CI runs are in [`natives/tests/`](../../../../natives/tests/). This directory is the evaluation record. Do not build from here inside a git work tree: this copy of `build-patched-luajit.sh` then silently skips patches 01, 07 and 08 and still exits 0 (details in `natives/luajit-patches/README.md`).

These patches go on top of LuaJIT v2.1. The recommended base is `cclj/v2.1`, which is upstream `14d8a7a2` plus 17 upstream fixes; see "Base revision" below and `BACKPORTS.md`. On 2026-11-07 the full stack 01–08 was tested with `verify-patch-stack.sh` on `19878ec0`, `3c4f9fe2`, `14d8a7a2` and `cclj/v2.1`. On every base the patches applied together, built, and passed every harness.

| # | Patch | Gate (build flag) | What it does | Origin |
|---|---|---|---|---|
| 01 | `01-interrupt.patch` | `-DLUAJIT_ENABLE_INTERRUPT` | Adds `luaJIT_interrupt(L)`, a per-VM flag that can be set from any thread (two plain stores). JIT-compiled traces check it at every trace entry and every loop iteration. When it is set, the trace exits to the interpreter, and the installed count hook runs at the next instruction. | preempt-patch lane |
| 02 | `02-xpcall-handler-in-hook.patch` | always on | `lj_err_run`: an error raised while a hook is active skips the `xpcall` message handler. Otherwise `xpcall(loop, function() while true do end end)` escapes the abort, because the handler runs with hooks disabled. | verify-preempt-patch |
| 03 | `03-no-bytecode.patch` | `-DLUAJIT_DISABLE_BCLOAD` | Every loader (`load`, `loadstring`, reader functions, `loadfile`) refuses binary chunks, whatever `mode` is passed. LuaJIT has no bytecode verifier, so this is a sandbox-escape fix. | rom-compat lane |
| 04 | `04-pattern-nul.patch` | always on | Pattern matching honours NUL bytes inside patterns, as Lua 5.2 and Cobalt do. Without it, `textutils.serialiseJSON` and `help` break, and some patterns silently mis-match. | rom-compat lane |
| 05 | `05-numfmt-roundtrip.patch` | always on | `tostring(number)` uses `%.15g`, falling back to `%.17g` when needed to round-trip, as Cobalt does. Stock LuaJIT uses `%.14g`, which loses precision in `textutils.serialise`, `settings` and JSON. **Cost:** `tostring(float)` is about 2.6x slower, and the CC "strings" benchmark is 22–25% slower. Integer `tostring` is unchanged. | rom-compat lane |
| 06 | `06-goto-contextual.patch` | always on | With `LUAJIT_ENABLE_LUA52COMPAT`, `goto` stays usable as an identifier (`t.goto(...)`, `function goto()`), as in Cobalt. This is common in turtle code. | rom-compat lane |
| 07 | `07-interruptible-patterns.patch` | `-DLUAJIT_ENABLE_INTERRUPT` (the Two-Way `find` part is always on) | The pattern matcher (every `match()` step, plus the `max_expand` and `%b` scans) and `table.sort`'s partition loops poll the interrupt flag. They then run the count hook in a protected, non-yieldable C frame: errors work there, yields are refused. Plain `string.find` (`lj_str_find`, which JIT traces also call) switches to the linear-time Two-Way algorithm after a work budget. **Results:** 17/17 pathological cases stop in 2–50 µs, where 01–06 hangs on all 17. A plain-find worst case drops from ~90 s to 10–21 ms. Semantics were identical across a 2.18M-result differential fuzz. Overhead: +1.8% geomean on pattern microbenchmarks, worst +8.7% (greedy `.*`). | proto-patterns-cstack |
| 08 | `08-cstack-guard.patch` | `-DLUAJIT_ENABLE_CSTACKGUARD` | `luaJIT_setcstacklimit(L, lo, hi)` sets a red zone on the C stack. Every C→VM re-entry raises a catchable "C stack overflow" inside that zone. That covers `lua_call`/`pcall`/`cpcall`/`resume`, C-API metamethods, `luaL_callmeta`, parser nesting, and the x64 fast path of `coroutine.resume`/`wrap`. **Results:** 31 recursion shapes on 1 MB stacks produced 0 crashes on Windows threads, Linux pthreads and JDK 21 Java threads. Without the patch, 21/29 crashed on Windows and 22/31 on Linux, and the JVM died silently. Overhead: about +0.5%. `global_State` grows by exactly 64 bytes; an unpadded 16-byte growth had cost +5% on coroutine switches. | proto-patterns-cstack |

Recommended build flags (used by `build-patched-luajit.sh`):

```
BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD -DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT"
```

For test builds, add `-DLUA_USE_ASSERT -DLUA_USE_APICHECK`. `DISABLE_VMEVENT` causes three harmless "unused variable" warnings (`lj_gc.c`, `lj_trace.c`).

Usage:

```
# in a LuaJIT clone (core.autocrlf=false): git checkout -b cclj/v2.1 14d8a7a2 && git am <this dir>/backports/0*.patch
LUAJIT_REV=cclj/v2.1 ./build-patched-luajit.sh <luajit-clone> build/luajit-cc
LUAJIT_REV=cclj/v2.1 ./verify-patch-stack.sh   <luajit-clone> build/verify    # Windows/MinGW
```

## Verified on 2026-11-07
Windows x64, MinGW gcc 16.2, all eight patches, the flags above, `verify-patch-stack.sh`. Identical results on `19878ec0`, `3c4f9fe2`, `14d8a7a2` and `cclj/v2.1`; logs for the first two and the last one are in `results/`:
- **Preemption harness** (`../harnesses/preempt`, method `intr`): 25/25 Lua-level loop shapes interrupted, with latency 0.7–22 µs on `19878ec0` and 0.8–15 µs on `3c4f9fe2`. Logs: `results/<base>/`. `X_string_find_backtrack` was interrupted too. Hook-yield, nested and 3×2 hard-abort cases all OK.
  - Two cases still hang, as expected, because the sandbox removes what they rely on: `X_finalizer_loop` (`newproxy`) and `X_lua_hook_loop` (`debug.sethook`).
- **Pattern harness** (`../harnesses/patterns-cstack`): 74/74 OK. That is 17 endless cases × `error`/`pcall`/`hard`/`hardyield`, plus 6 finite cases under `pause`. The patch author's own run also included the `yieldbad` and `gcstress` modes and reached 97/97.
- **C-stack harness:** 31/31 shapes ended without a crash: 25 with a catchable error, 6 as expected successes.
- **`compat_check.lua`:**
  - bytecode is refused (`load(string.dump(f))` → nil);
  - NUL patterns work;
  - `tostring(0.1+0.2) == "0.30000000000000004"`;
  - `goto` works as a field name and as a statement;
  - `table.pack`/`unpack` exist;
  - `ffi` is absent;
  - the JIT is on.

Earlier runs by the patch authors also covered Linux x64 (WSL: preemption 25/25 via `win32_on_posix.h`, pattern interrupts from a second pthread, C-stack 31/31) and a JDK 21 JVM (C-stack with JNI upcalls). **Not tested yet: arm64 (Linux or macOS), macOS x64.**

## Base revision
- **Recommended base (2026-11-07): `cclj/v2.1` = upstream `14d8a7a2` (2026-07-18) + 17 cherry-picked upstream fixes.** The fixes are in `backports/`, and the reasons for each are in `BACKPORTS.md`.
  - `14d8a7a2` is the last v2.1 commit before upstream merged LuaJIT 3.0 syntax extensions into v2.1 (`a2ce8114`, 2026-07-20): `continue`, `?.`, `??`, `+=`, bit operators, `const`, short functions and more. Cobalt and CC:T reject all of these, and upstream has no switch to turn them off.
  - `14d8a7a2` is `3c4f9fe2` + one x64 backend fix, with identical harness results.
  - The 17 picks fix 5 JIT/VM bugs that sandboxed code can reach and that were confirmed on the base, on Windows and Linux x64:
    - `#t` after a rehash;
    - stale `math.floor()`/`ceil()` arguments;
    - ±0 for-loop steps;
    - `ldexp` folding;
    - `tonumber(s, 16)` truncated on Windows.

    The rest are OOM, stitching, `next()` and register-allocation fixes, plus ARM64/macOS fixes that are compiled out on x64 for now. 9 performance commits are deferred until they are benchmarked.
  - Verified (`results/cclj-v2.1/`): patches 01–08 apply cleanly; preempt 33 OK + 2 expected hangs, patterns 74/74, C-stack 31/0, compat identical, syntax probe 45/45 (`tools/syntax_probe.lua`), JIT-vs-interpreter checks 6/6 (`tools/fixcheck.lua`). This holds on Windows x64 and Linux x64.
- Build it with `git checkout -b cclj/v2.1 14d8a7a2 && git am backports/0*.patch` (in a LuaJIT clone with `core.autocrlf=false`), then `LUAJIT_REV=cclj/v2.1 ./build-patched-luajit.sh <clone> <out>`.
- The stack 01–08 also applies to `19878ec0` (the old vendored copy) and `3c4f9fe2`. Results for both are in `results/`.
- Upstream HEAD (`ea9eb7ed`, 2026-10-30) also works, but only with `head-kit/`: patch 06 ported to HEAD, and a syntax gate that rejects the v3.0 syntax. It is not used for now (`../../reports/pin-head-gate.md`).

## Still to do
- **09** `LUAI_MAXCSTACK 65000` in `luaconf.h`. It lets `unpack` and `string.byte(s, 1, -1)` return roughly 30k–64k values instead of 8,000. It was tested in a probe build; it still needs to be added here and re-verified. Cobalt allows at least 2^19 values, so a gap remains.
- **10** `tostring`/`%p` without heap addresses: print a keyed hash, Cobalt-style.
- **11** Lexer interrupt poll every 64 KiB of input.
- Port 08's 4-instruction coroutine fast-path check to `vm_arm64.dasc`, or rely on the C coroutine wrapper (DESIGN §3.3), which goes through the guarded `lua_resume`.
- `msvcbuild.bat` does not pass the DynASM `CSTACKGUARD` flag. That is irrelevant while builds use MinGW/gcc/clang.
