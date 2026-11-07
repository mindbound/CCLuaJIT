# Pause token protocol prototype (P10 / WP F)

This prototype is a C and Lua implementation of transparent time-slicing for LuaJIT under CC:T's `MachineResult.PAUSE` contract. A pause requested at any coroutine depth yields all the way up to the host and later resumes exactly where it stopped. It works for programs running inside `parallel` / `multishell` coroutines. The full report is in [`../../reports/proto-pause.md`](../../reports/proto-pause.md).

| File | What it is |
|---|---|
| `cclj_co.c`, `cclj_co.h` | About 450 lines of C plus a 12-line Lua continuation. It replaces the sandbox's `coroutine.resume` and `coroutine.wrap` with a C `raw_resume`. That function checks status the way `ffh_resume` does, enforces the nesting limit (count 200 plus a stack-pointer floor), and propagates pauses. It also contains the count hook (PAUSE / SOFT / HARD) and the request side used by other threads. |
| `pausehost.c`, `plat.h` | A host that behaves like CC:T. Lua runs on a thread with a 1 MB stack, a second thread requests pauses every 0.3–1.5 ms, and events are delivered the way `CobaltLuaMachine.handleEvent` delivers them. |
| `tests/t1..t7*.lua`, `tests/lib/cc.lua` | The 7 tests: ROM `parallel`, deep nesting, semantics, C-call boundaries, aborts, nesting limit, performance. |
| `build.sh`, `run_tests.sh`, `linux/` | Build and test scripts for Windows (MinGW) and Linux (WSL). `logs/run_tests.out` and `linux/run-linux.out` hold the results from 2026-11-07: all PASS. |
| `microbench/resume_rt.c` | Microbenchmark of resume round-trip cost. |

## Build notes
- `cclj_co.c` includes LuaJIT's internal `lj_obj.h` so that it can check coroutine status exactly like `ffh_resume`. It must therefore be compiled with the **same feature flags** as `libluajit.a`. That is natural for `libcclj`, which links LuaJIT statically. `build.sh` expects a patched LuaJIT tree in `./luajit` (and `./luajit-assert` for the assert build). Build them with `../luajit-patches/build-patched-luajit.sh` and add the same `-D` flags here.
- The prototype was developed on patches 01–06. On 2026-11-07 it was re-run against LuaJIT `3c4f9fe2` + 01–08 with the final flags. `build.sh` now carries them (`-DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT` were added); `linux/build-linux.sh` still uses the 01–06 flags. `run_tests.sh quick` passed all 7 suites (46 sub-tests). The output is in `logs/run_tests-3c4f9fe2-01-08.out`.

## Key results (Windows x64 and Linux x64, JIT on and off, plus an assert build)
- **Transparent:** 2,719 pauses during the ROM `parallel` test (61,575 at a 20–60 µs pause rate) gave byte-identical results, with no lost or extra events. A naive hook-yield fails the same test within 8 ms.
- **C boundaries:** pauses are deferred inside non-yieldable C callbacks (C `sort` comparator, `gsub` callback, `xpcall` handler, `format("%s")`→`__tostring`, `load` reader). "attempt to yield across C-call boundary" never reaches user code. In adversarial tests, deferral delayed a pause by up to about 0.5 s; the optional checkpoint wrappers cut that to 6–17 ms.
- **Aborts:** hard abort takes at most 42 µs, including through `pcall` loops and a looping `xpcall` handler. Soft abort stays catchable and has a `chunk:line:` prefix.
- **Nesting limit:** recursion ends with a clean Lua error. Without the limit, a 1 MB thread crashes at about 12,600 native levels or 4,500 wrapper levels.
- **Cost:** about 13–15 ns more per resume (1.3–1.7x on pure ping-pong microbenchmarks, about 0% on real workloads), and about 1–2.5 µs per pause.
- **Design lesson:** do **not** re-arm the interrupt from the hook while a pause is deferred. That made JIT code inside sort/gsub callbacks 4–7x slower. Instead, a per-process ticker re-pokes machines that have a pending pause every ~250 µs.
