# Harnesses for patch 07 (interruptible patterns) and patch 08 (C-stack guard)

The full report is [`../../../reports/proto-patterns-cstack.md`](../../../reports/proto-patterns-cstack.md). To run everything against a fresh 01–08 build, use [`../../luajit-patches/verify-patch-stack.sh`](../../luajit-patches/verify-patch-stack.sh).

| File | What it tests |
|---|---|
| `harness/pharness.c` | 17 endless or pathological C-library cases: `find`/`match`/`gmatch`/`gsub` backtracking, 15 captures, `.-.-.-.-b`, O(n²) `gsub`/`gmatch`, `%b()`, a 200 MB `.*` scan, frontier, anchored, a 100k "sort killer", and `gsub` with a function replacement. Each case runs in the modes `error`, `pcall`, `hard`, `hardyield` and `yieldbad`. There are also 6 finite cases under `pause` and `gcstress`. Windows only (Win32 threads). Set `PH_DIR` to a folder containing `lua/killer100k.txt` and `lua/killer32k.txt`, which `lua/genkiller.lua` generates. |
| `harness/cstack.c`, `harness/run_cstack.py` | 31 recursion shapes on 1 MB threads: `gsub` callback, `sort` comparator/`__lt`, `format %s`, `load` reader, `xpcall` handler, coroutine chains, host `lua_call`/`pcall`/`cpcall`/metamethod/`resume`, parser depth, and a mixed chain. Each must finish without a crash and leave the state usable. Most end in a catchable `C stack overflow` or `stack overflow`; a few complete normally (on 01–08, 25 ended in an error and 6 completed). Builds on Windows and Linux. Usage: `python -I run_cstack.py cstack.exe <stack_kb> <margin_kb>`. |
| `harness/strfind_test.c`, `strfind_twoway.h` | Standalone test of the Two-Way plain-find fallback: 3M random cases plus forced mid-search hand-over, compared with the naive search. |
| `harness/runlua.c` | Runs a Lua file with a background interrupter, used by the differential fuzz under interrupt stress (`STRESS_MS`). |
| `jni/` | `CStackJni.java` and `cstackjni.c` test the C-stack guard inside a real JDK 21 JVM, including JNI upcalls at every recursion level. |
| `linux/` | Linux variants (`plinux.c` interrupts patterns from a second pthread). |
| `lua/patfuzz.lua`, `lua/chunkfuzz.lua` | Differential pattern fuzzers. The 01–06 and 01–08 builds gave identical results on about 2.18M results. |
| `lua/patbench.lua`, `lua/cobench.lua` | Overhead benchmarks. |
| `final_verify.sh`, `build.sh`, `benchrun.py` | The scripts used on 2026-11-07. They contain absolute scratch paths, so they are reference only. |
| `results/` | Results from 2026-11-07: preempt, pharness, cstack (1 MB/128 KB and 256 KB/64 KB), fuzz, benchmarks, JNI, Linux. |
