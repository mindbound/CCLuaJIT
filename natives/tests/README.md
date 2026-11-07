# LuaJIT regression harnesses

These are the harnesses that CI runs against every patched LuaJIT build (PLAN.md §5.2, gate G0), promoted from `docs/port-1.21.1/artifacts/` into the build tree. `run-harnesses.sh` builds them against a LuaJIT tree from `natives/luajit-patches/build-patched-luajit.sh`, runs them, parses the counts and exits non-zero on any deviation.

```
bash natives/build-natives.sh windows-x86_64          # or linux-x86_64; leaves natives/build/<platform-id>/luajit
bash natives/tests/run-harnesses.sh natives/build/windows-x86_64/luajit/src
```

It needs gcc (MinGW-w64 on Windows, run from Git Bash or an MSYS2 MINGW64 shell), `timeout` and Python 3.6 or newer (`python` on Windows, `python3` on Linux). Logs go to `natives/build/<platform-id>/harnesses/results/`. A run takes about 2 minutes on Windows (generating the sort-killer inputs takes 1 of them) and 30 seconds on Linux.

macOS is supported only for the informational CI job (`harness-informational` in `.github/workflows/build.yml`), which builds the LuaJIT tree with `luajit-patches/build-patched-luajit.sh` (`MACOSX_DEPLOYMENT_TARGET` must be set). There, `gcc` is Apple clang, the cstack suite reports SKIP instead of running (`cstack.c` needs `pthread_getattr_np`), Python is not needed, and without coreutils `timeout` the script uses `gtimeout` or an equivalent in perl. The expected results in the table were measured on x86_64 only.

CI (`.github/workflows/build.yml`) runs this script on windows-x86_64 and linux-x86_64 (required) and on linux-aarch64 and macos-aarch64 (informational, never fails the workflow).

| Suite | Windows | Linux | Expected |
|---|---|---|---|
| preempt | `preempt/harness.c` | `preempt/harness_posix.c` + `win32_on_posix.h` | 33 OK, and exactly `X_finalizer_loop` and `X_lua_hook_loop` hang (35 results) |
| patterns (patch 07) | `patterns/pharness.c` (modes error, pcall, hard, hardyield, pause) | `patterns/plinux.c` | Windows: 74 OK of 74. Linux: 5/5 interrupted, 5/5 usable afterwards |
| cstack (patch 08) | `cstack/cstack.c` via `cstack/run_cstack.py`, 1 MB stacks, 128 KB red zone | same | 31 completed, 0 crashed |
| compat | `lua/compat_check.lua` | same | output equals `lua/compat_check.expected` |
| syntax | `lua/syntax_probe.lua` | same | `SUMMARY ok=45 bad=0` |
| fixcheck | `lua/fixcheck.lua` with `-Ohotloop=1` | same | 6 PASS, 0 FAIL |

The two preemption hangs are expected: those cases need `newproxy` and `debug.sethook`, which the sandbox removes. The harnesses that read `G(L)->vmstate` include LuaJIT's internal `lj_obj.h`, so the runner compiles them with the same feature flags as the library (read from `cclj-build.txt` in the LuaJIT tree).

## Provenance

Every file except `run-harnesses.sh`, `lua/compat_check.expected` and this README is a verbatim copy, made on 2026-11-07 from commit `c48cace`. The git blob ids are identical to those of the sources. The sources in `docs/` stay as the historical record. Fix the harnesses here.

| File | Copied from (`docs/port-1.21.1/artifacts/...`) | Blob |
|---|---|---|
| `preempt/harness.c` | `harnesses/preempt/harness.c` | `2101d7e8` |
| `preempt/harness_posix.c` | `harnesses/preempt/harness_posix.c` | `b6725922` |
| `preempt/win32_on_posix.h` | `harnesses/preempt/win32_on_posix.h` | `9717e681` |
| `preempt/compat_intr.h` | `harnesses/preempt/compat_intr.h` | `8b7f7c6c` |
| `patterns/pharness.c` | `harnesses/patterns-cstack/harness/pharness.c` | `f1d8cfdd` |
| `patterns/plinux.c` | `harnesses/patterns-cstack/linux/plinux.c` | `ce832c8d` |
| `patterns/genkiller.lua` | `harnesses/patterns-cstack/lua/genkiller.lua` | `af580f9e` |
| `patterns/killer.lua` | `harnesses/patterns-cstack/lua/killer.lua` | `f16db889` |
| `cstack/cstack.c` | `harnesses/patterns-cstack/harness/cstack.c` (identical to `.../linux/cstack.c`) | `de4ef7ec` |
| `cstack/run_cstack.py` | `harnesses/patterns-cstack/harness/run_cstack.py` (already Python 3.6-compatible) | `b913ee3f` |
| `lua/compat_check.lua` | `lua/compat_check.lua` | `563dd2d9` |
| `lua/syntax_probe.lua` | `luajit-patches/tools/syntax_probe.lua` | `6551eb30` |
| `lua/fixcheck.lua` | `luajit-patches/tools/fixcheck.lua` | `e0cc5684` |

`run-harnesses.sh` replaces `luajit-patches/verify-patch-stack.sh` (Windows only) and `luajit-patches/tools/linux-sanity.sh` (Linux only). It uses the same case loops and timeouts, and adds:

- exact count checks and a non-zero exit code. The old scripts only printed counts. On AlmaLinux 8, `linux-sanity.sh` printed `0 completed, 0 crashed` for the C-stack harness and carried on;
- the exact names of the two expected preemption hangs;
- a comparison of `compat_check.lua` with an expected file, where the old scripts printed the output for a human to read;
- `fixcheck.lua`.

The background for each harness is in `docs/port-1.21.1/reports/preempt-patch.md` and `docs/port-1.21.1/reports/proto-patterns-cstack.md`, and in `docs/port-1.21.1/artifacts/harnesses/patterns-cstack/README.md`.
