# Pause token protocol prototype (P10 / WP F)

This prototype is a C and Lua implementation of transparent time-slicing for LuaJIT under CC:T's `MachineResult.PAUSE` contract. A pause requested at any coroutine depth yields all the way up to the host and later resumes exactly where it stopped. It works for programs running inside `parallel` / `multishell` coroutines. The full report is in [`../../reports/proto-pause.md`](../../reports/proto-pause.md).

| File | What it is |
|---|---|
| `cclj_co.c`, `cclj_co.h` | About 450 lines of C plus a 12-line Lua continuation. It replaces the sandbox's `coroutine.resume` and `coroutine.wrap` with a C `raw_resume`. That function checks status the way `ffh_resume` does, enforces the nesting limit (count 200 plus a stack-pointer floor), and propagates pauses. It also contains the count hook (PAUSE / SOFT / HARD) and the request side used by other threads. |
| `pausehost.c`, `plat.h` | A host that behaves like CC:T. Lua runs on a thread with a 1 MB stack, a second thread requests pauses every 0.3–1.5 ms, and events are delivered the way `CobaltLuaMachine.handleEvent` delivers them. |
| `tests/t1..t7*.lua`, `tests/lib/cc.lua` | The 7 tests: ROM `parallel`, deep nesting, semantics, C-call boundaries, aborts, nesting limit, performance. `cc.lua` holds the test helpers and loads the CC:T code the tests drive from an extracted ROM (see Build notes). |
| `build.sh`, `run_tests.sh`, `linux/` | Build and test scripts for Windows (MinGW) and Linux (WSL). `logs/run_tests.out` and `linux/run-linux.out` hold the results from 2026-11-07: all PASS except `jitoff-t3_semantics` `pingpong_under_pauses` (`logs/run_tests.out:115`, unexplained; it did not recur in the 2026-11-08 re-runs below). |
| `microbench/resume_rt.c` | Microbenchmark of resume round-trip cost. |

## Build notes
- **CC:T ROM.** The tests run CC:T 1.121.0's own `os.pullEvent`/`os.pullEventRaw` (from `bios.lua`) and `parallel` API, with the real `cc.expect` and `cc.internal.exception`. That code is CCPL / MPL-2.0, so this MIT repository does not contain it. `tests/lib/cc.lua` reads it at run time from the directory in `CCT_ROM`, and stops unless that ROM is 1.121.0. `run_tests.sh` and `linux/run-linux.sh` default `CCT_ROM` to the `cct-rom/` next to `extract_cct_rom.py`, which git ignores, take a relative `CCT_ROM` from the directory they are started in, and exit with 125 if it has no `rom/apis/parallel.lua`. Fill it once from the CC:T core jar (in the Gradle cache after a dev build). From this directory:

  ```bash
  python -I ../extract_cct_rom.py <cc-tweaked-1.21.1-core-1.121.0.jar> ../cct-rom
  ```

  Until 2026-11-08, `cc.lua` held a verbatim copy of those functions, with a stand-in for `expect` and two verbatim lines of `cc.internal.exception`. The logs from 2026-11-07 come from that copy. On 2026-11-08 the full `run_tests.sh` matrix (Windows, four times) and `linux/run-linux.sh` (WSL Ubuntu, five times) were re-run with the ROM loaded at run time, on LuaJIT `047efd3b` + 01–08. Every run passed except the naive control, which must fail, and once `t5_abort-rep1` `soft_uncaught`: its soft-abort error was attributed to the `[cclj coroutine]` continuation instead of `tests/t5_abort.lua:123`. That case calls no ROM code, and 120 more `t5_abort` runs with the old and the new `cc.lua` did not repeat it, so it is a timing flake of the prototype. The old and the new `cc.lua`, on the same binary, printed the same `result:` lines and the same `[ref]` counters (okyields, events, dropped, resumes) for t1–t7, including the t1 result with `resumes=13883`. Only `stack_hw` and t6's floor-limited depth differ, and they vary as much between two runs of the same `cc.lua`. Those re-run logs are not stored.
- `cclj_co.c` includes LuaJIT's internal `lj_obj.h` so that it can check coroutine status exactly like `ffh_resume`. It must therefore be compiled with the **same feature flags** as `libluajit.a`. That is natural for `libcclj`, which links LuaJIT statically. `build.sh` expects a patched LuaJIT tree in `./luajit` (and `./luajit-assert` for the assert build). Build them with `../luajit-patches/build-patched-luajit.sh` and add the same `-D` flags here.
- The prototype was developed on patches 01–06. On 2026-11-07 it was re-run against LuaJIT `3c4f9fe2` + 01–08 with the final flags. `build.sh` now carries them (`-DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT` were added); `linux/build-linux.sh` still uses the 01–06 flags. `run_tests.sh quick` passed all 7 suites (46 sub-tests). The output is in `logs/run_tests-3c4f9fe2-01-08.out`.

## Key results (Windows x64 and Linux x64, JIT on and off, plus an assert build)
- **Transparent:** 2,719 pauses during the ROM `parallel` test (61,575 at a 20–60 µs pause rate) gave byte-identical results, with no lost or extra events. A naive hook-yield fails the same test within 8 ms.
- **C boundaries:** pauses are deferred inside non-yieldable C callbacks (C `sort` comparator, `gsub` callback, `xpcall` handler, `format("%s")`→`__tostring`, `load` reader). "attempt to yield across C-call boundary" never reaches user code. In adversarial tests, deferral delayed a pause by up to about 0.5 s; the optional checkpoint wrappers cut that to 6–17 ms.
- **Aborts:** hard abort takes at most 42 µs, including through `pcall` loops and a looping `xpcall` handler. Soft abort stays catchable and has a `chunk:line:` prefix.
- **Nesting limit:** recursion ends with a clean Lua error. Without the limit, a 1 MB thread crashes at about 12,600 native levels or 4,500 wrapper levels.
- **Cost:** about 13–15 ns more per resume (1.3–1.7x on pure ping-pong microbenchmarks, about 0% on real workloads), and about 1–2.5 µs per pause.
- **Design lesson:** do **not** re-arm the interrupt from the hook while a pause is deferred. That made JIT code inside sort/gsub callbacks 4–7x slower. Instead, a per-process ticker re-pokes machines that have a pending pause every ~250 µs.
