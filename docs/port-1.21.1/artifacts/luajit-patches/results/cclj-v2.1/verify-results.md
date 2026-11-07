# Pin-and-backport lane: verification results (2026-11-07)

`W` = `<scratch>\work\pin-backports` (Git Bash path form of the same directory).
`ART` = `<repo>\docs\port-1.21.1\artifacts`. Nothing in the CCLuaJIT tree was modified. The verify scripts were run in place; they write only to their `<work-dir>`.

Toolchains:
- Windows 11, Git Bash, gcc 16.2.0 (WinLibs MinGW-W64 x86_64-msvcrt-posix-seh), mingw32-make.
- WSL2 Ubuntu (kernel 6.18.54.1-microsoft-standard-WSL2), gcc 15.2.0, GNU Make 4.4.1, Python 3.14.4.

## 1. Upstream state

```
git clone --reference <repo>/vendor/luajit --dissociate https://github.com/LuaJIT/LuaJIT $W/luajit
git -C $W/luajit fetch origin
git ls-remote https://github.com/LuaJIT/LuaJIT refs/heads/v2.1 refs/heads/master
```

| Ref | Hash | Commit date | Subject |
|---|---|---|---|
| v2.1 HEAD | `ea9eb7edb5eac4a9d175d74523db34fac0fa5180` | 2026-10-30 19:07:02 +0100 | Don't FOLD ldexp with non-integer exponent. |
| master HEAD | `592f9e3f549c7f176a9a7a0d70189559305f2eda` | 2026-10-29 12:45:28 +0100 | x86/x64: Don't sink IR_CONV num.int with -O-dce. (already merged into v2.1) |
| 3c4f9fe2 | `3c4f9fe2052b8d08a917ac0d5f38563f0297b5a3` | 2026-07-11 14:41:15 +0200 | Merge branch 'master' into v2.1 |
| 14d8a7a2 | `14d8a7a27dc8c626ab9e7c7e9e50b6df6def4f03` | 2026-07-18 09:48:48 +0200 | x64/LJ_GC64: Avoid store-to-load forwarding stall after stack restore. |
| a2ce8114 | `a2ce8114f107464473070427e69e84f4923bd08a` | 2026-07-20 10:20:49 +0200 | Backport some v3.0 syntax extensions. |

`14d8a7a2..origin/v2.1` contains 52 commits: 45 non-merge and 7 merges of `master`. Nothing on `master` is missing from v2.1 (`git rev-list --count origin/v2.1..origin/master` = 0).

## 2. verify-patch-stack.sh on 14d8a7a2, compared with 3c4f9fe2

```
LUAJIT_REV=14d8a7a2 $ART/luajit-patches/verify-patch-stack.sh $W/luajit $W/verify-14d8a7a2      # 2m05s
LUAJIT_REV=3c4f9fe2 $ART/luajit-patches/verify-patch-stack.sh $W/luajit $W/verify-3c4f9fe2-control   # 1m59.8s, same-session control
```

| Suite | Reference `results/3c4f9fe2` | 3c4f9fe2 (my control run) | **14d8a7a2** |
|---|---|---|---|
| build | 8 patches | OK, 8 patches applied, no `patch` fallback | **OK, 8 patches applied, no `patch` fallback** |
| preempt harness | 33 OK, 2 expected hangs | 33 OK, 2 hang (X_finalizer_loop, X_lua_hook_loop) | **33 OK, 2 hang (the same 2)** |
| pattern harness | 74 OK | 74 OK, 0 timeout/fail/hang | **74 OK, 0** |
| C-stack harness | 31 completed, 0 crashed | 31 completed (25 error + 6 ok), 0 crashed | **31 completed (25 error + 6 ok), 0 crashed** |
| compat_check.lua | all expected | identical | **identical** |
| gcc warnings | — | 99, all `-Wswitch` at lj_opt_fold.c:372 | **the same 99** |

The per-case comparison is in `results/outcomes-*.txt`, produced by `tools/outcomes.sh <results-dir>`. The script strips timings, line numbers and paths, and leaves 145 lines: every preempt case + RESULT, every pharness case/mode + RESULT, the 31 C-stack verdicts, and compat. The reference, the control, 14d8a7a2, the backport branch and the full branch all give **byte-identical** files (md5 `1fd1f6c23687…` for all five).

Interrupt latency (`tools/latency.sh`, one sample per case, 32 cases):

| Run | min | median | max |
|---|---|---|---|
| reference 3c4f9fe2 | 0.8 | 3.6 | 15.4 µs |
| control 3c4f9fe2 (this session) | 0.6 | 3.7 | 66.8 µs (gsub_callback) |
| 14d8a7a2 | 0.8 | 3.7 | 63.0 µs (while_true) |
| backport branch | 0.5 | 4.0 | 78.1 µs (pairs_loop) |
| full branch | 0.9 | 3.7 | 22.6 µs |

Each single-sample maximum lands on a different case in each run, and the control run on the old pin shows one too. Repeating three cases 10 times per build (`results/latency-repeat.txt`; `harness.exe intr error <case>`) gives:
- `while_true`: 6–13 µs on every build;
- `pairs_loop`: 0.5–4.6 µs (one 19.5 µs sample on the full branch);
- `gsub_callback`: 0.4–2.4 µs.

The outliers are scheduling noise, not a regression.

**Result: 14d8a7a2 is equivalent to 3c4f9fe2 on every harness.** It can replace 3c4f9fe2 as the pin.

## 3. Backport branch `cclj/backports-14d8a7a2` (tip `a8f033531355daa7eae9ca3c4b455e53863ff14b`)

The branch is 14d8a7a2 plus 17 `git cherry-pick -x` picks, in upstream order (`results/cherry-pick-log.txt`):
- 16 clean;
- 1 trivial conflict: `55853647` in `lj_asm_x86.h`. It is a master-side commit, so its context is v2.0 code. `tools/resolve_55853647.py` resolved it exactly as upstream's merge `47fc4e39` did; afterwards `asm_tobit()` and `asm_conv()` are identical to upstream v2.1.

None of the 17 picks touched the syntax-backport code. Picks touching files that a2ce8114 also changed (lj_record.c, lj_ffrecord.c, lj_opt_fold.c, vm_x64.dasc, vm_arm64.dasc) were all clean.

Files that are now identical to upstream HEAD: `dynasm/dasm_arm64.h`, `dynasm/dasm_x86.lua`, `src/lj_asm.c`, `src/lj_mcode.c`. The remaining differences in touched files are exactly the deferred or skipped commits, for example `lj_opt_mem.c` +23 lines = 1ee778a4.

Patches 01–08 on the branch: `git apply --check` + `git apply` for each patch in sequence, on a `-c core.autocrlf=false` export, all 8 ok. `build.txt` shows no `patch` fallback and no fuzz.

```
LUAJIT_REV=cclj/backports-14d8a7a2 $ART/luajit-patches/verify-patch-stack.sh $W/luajit $W/verify-backports   # 2m06s
```
`build: OK (8 patches applied)` · `preempt harness: 33 OK, 2 hang` · `pattern harness: 74 OK, 0` · `cstack harness: 31 completed, 0 crashed` · compat identical. The per-case outcomes are identical to the reference (see §2), and the warning set is the same 99 `-Wswitch` warnings, now at lj_opt_fold.c:375 because ea9eb7ed adds 3 lines above.

**The new syntax is still rejected.** `tools/syntax_probe.lua` runs `load()` on:
- 35 v3.0 snippets, which must be rejected: every bit operator, `!`/`&&`/`||`/`!=`, `?:`, `?.`, `??`, all 12 compound assignments (including `a ~= 2` as a statement), `continue`, `const`, 3 short-function forms, `1_000`, `return;;`, and the `#10` line directive on line 2;
- 10 Lua 5.2/Cobalt controls, which must be accepted: `~=` comparison, `t.goto` (patch 06), `goto`, `continue`/`const` as variable names, `;;` between statements.

| Build | Result |
|---|---|
| backport branch (patched) | `SUMMARY ok=45 bad=0` |
| full branch (patched) | `ok=45 bad=0` |
| 14d8a7a2 (patched) | `ok=45 bad=0` |
| 3c4f9fe2 (patched) | `ok=45 bad=0` |
| **upstream HEAD ea9eb7ed, unpatched** (`mingw32-make BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI"`), positive control | `ok=9 bad=36`: all 35 v3.0 snippets accepted, and `t.goto` rejected because patch 06 is absent |

Side finding (pre-existing, not v3.0): LuaJIT's lexer skips a first line that starts with `#` in every chunk, including `load("#10\nreturn 1")`. PUC 5.2 does that only in `luaL_loadfile`. Someone should check this against Cobalt (rom-compat lane).

**Behaviour checks:** `tools/fixcheck.lua`, run as `luajit -Ohotloop=1 fixcheck.lua`. Each check compares JIT with interpreter or with the specified value.

| Check | 14d8a7a2 Win | branch Win | 14d8a7a2 Linux | branch Linux |
|---|---|---|---|---|
| 95f931cb: #t after NEWREF rehash (#1534 repro 1) | **FAIL** jit=0 interp=3 | PASS | **FAIL** | PASS |
| 849bbbe7: `pcall(math.floor)` must error | **FAIL** returns `true, 11` (stale slot) | PASS | **FAIL** | PASS |
| bc9261a3: 96 for-loops with ±0 step, JIT vs interpreter | **FAIL** 12 differ | PASS 0 differ | **FAIL** 12 differ | PASS |
| 2a8e017e: `tonumber('ffffffffff',16)` | **FAIL** 4294967295 | PASS 1099511627775 | PASS (already 64-bit) | PASS |
| ea9eb7ed: `math.ldexp(k, non-integer k)`, JIT vs interpreter | **FAIL** jit `0` vs interp `inf`/`nan` | PASS | **FAIL** | PASS |
| a4c06453: `select(2^28+k, ...)` | PASS (not reproduced) | PASS | PASS | PASS |

The full branch passes all six checks too (`results/fixcheck-windows-full.txt`).

**Codegen check for 346ab587** (DynASM movd/movq): plain builds of `lj_vm.o` from 14d8a7a2 and from 14d8a7a2+346ab587 are byte-identical (`cmp`).

## 4. Linux x64 sanity (WSL Ubuntu)

```
git -C $W/luajit -c core.autocrlf=false archive cclj/backports-14d8a7a2 | tar -xf - -C $W/linux/src-backports
MSYS_NO_PATHCONV=1 wsl -e bash /mnt/c/.../pin-backports/tools/linux-sanity.sh \
   /mnt/c/.../pin-backports/linux/src-backports /mnt/c/.../pin-backports/linux/backports \
   <repo>/docs/port-1.21.1/artifacts /mnt/c/.../pin-backports/tools/syntax_probe.lua
```
`linux-sanity.sh` does the following:
- builds with `build-patched-luajit.sh` (plain-directory mode, `make`);
- runs the preemption harness from `harnesses/preempt/harness_posix.c` + `win32_on_posix.h` (`-DHAVE_LUAJIT_INTERRUPT -lpthread`), with the same case loop as `verify-patch-stack.sh`;
- runs `patterns-cstack/linux/plinux.c` (pattern/sort interrupts from a second pthread);
- runs `patterns-cstack/linux/cstack.c` via `run_cstack.py 1024 128`;
- runs `compat_check.lua` and the syntax probe.

| | 14d8a7a2 | backport branch |
|---|---|---|
| build | OK, 8 patches, 99 warnings | OK, 8 patches, 99 warnings |
| preempt (POSIX) | 33 OK, 2 hang (X_finalizer_loop, X_lua_hook_loop) | 33 OK, 2 hang (same); X_string_find_backtrack OK |
| plinux | 5/5 fired, 5/5 usable afterwards | 5/5, 5/5 |
| C-stack, 1 MB pthreads | 31 completed, 0 crashed | 31 completed (25 error + 6 ok), 0 crashed |
| compat_check | as on Windows | as on Windows |
| syntax probe | ok=45 bad=0 | ok=45 bad=0 |

Each run took about 35 s. Linux interrupt latency on the branch was min 1.0 / median 4.5 / max 59.9 µs (single samples).

## 5. Full-sync variant `cclj/backports-full-14d8a7a2` (tip `67261ecef17f395bc44a1fff92390d637fba4cf3`)

This branch is 14d8a7a2 plus 26 picks: the 17 above plus the 9 deferred performance commits, in upstream order. It had the same single conflict (55853647), resolved the same way; after that pick, `lj_asm_x86.h` was identical to upstream `47fc4e39`.

Files that end up identical to upstream HEAD: `lj_asm_x86.h`, `lj_opt_mem.c`, `lib_base.c`, `lj_asm.c`, `lj_mcode.c`, `dasm_arm64.h`, `dasm_x86.lua`. `lj_ffrecord.c` differs only by a2ce8114's 1-line change.

Results:
- verify (1m52s): build OK, 33 OK + 2 hang, 74 OK, 31/0, compat identical, per-case outcomes identical;
- latency 0.9 / 3.7 / 22.6 µs;
- syntax probe 45/0;
- fixcheck 6/6.

This branch is not recommended yet: the performance gain of the deferred group has not been measured.
