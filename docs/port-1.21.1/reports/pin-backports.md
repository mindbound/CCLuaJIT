# LuaJIT base: pinned 14d8a7a2 + upstream backports

*Written on 2026-11-07 by an investigation agent (decision 5/6 in PLAN.md). Reusable outputs were copied to `../artifacts/luajit-patches/` (BACKPORTS.md, backports/, backports-full/, tools/, results/cclj-v2.1/). Paths under `scratchpad/work/...` refer to the session scratch workspace and no longer exist.*

## Summary

Pinning 14d8a7a2 and cherry-picking upstream fixes onto it works, and it costs little. On Windows, verify-patch-stack.sh on 14d8a7a2 gave exactly the same per-case results as the 3c4f9fe2 reference: build OK, preempt 33 OK plus the 2 expected hangs, patterns 74/74, C-stack 31 completed and 0 crashed, compat identical. The normalized outcome files match byte for byte.

Upstream v2.1 HEAD on 2026-11-07 is ea9eb7ed (2026-10-30). There are 45 non-merge commits after 14d8a7a2, 8 of them from master. I classified them as follows:
- 10 fixes that matter for the x64 Windows/Linux build now;
- 5 ARM64/macOS fixes, compiled out on x64;
- 2 sync-only commits that change no behaviour;
- 9 performance changes to defer, each together with the fixes for bugs it introduced;
- 19 to skip: 4 syntax, 7 FFI-only, 5 unsupported platforms, 2 docs, 1 jit.* tooling.

The branch cclj/backports-14d8a7a2 is 14d8a7a2 plus 17 picks: 16 applied cleanly. One (55853647, a master-side commit) had a trivial conflict, which I resolved exactly as upstream's own merge 47fc4e39 did. No pick ran into the syntax-backport code. Patches 01-08 apply strictly with git apply.

On the branch, every harness gives results identical to the reference on Windows. A WSL Linux build also passes: POSIX preempt harness 33 OK + 2 expected hangs, plinux 5/5, C-stack 31/0. A 45-case syntax probe passes 45/45 on the branch; the same probe on unpatched upstream HEAD accepts all 35 v3.0 constructs, so the probe does detect them.

A behaviour check (JIT vs interpreter, -Ohotloop=1) shows real bugs in the 14d8a7a2 base that the branch fixes, on both Windows and Linux:
- #t is wrong after a rehash (JIT 0, interpreter 3);
- pcall(math.floor) returns a stale stack value (11);
- 12 of 96 for-loops with a +-0 step differ between JIT and interpreter;
- math.ldexp with non-integer exponents is folded to 0 instead of inf/nan;
- on Windows only, tonumber('ffffffffff', 16) is truncated to 32 bits.

Since the pin there have been about 2.7 relevant fixes a month (4.1 including ARM64/macOS) out of about 12 upstream commits a month. They arrive in bursts: none in July or September, 8 in October. My first-time triage of the 45 commits took about 6 minutes of wall-clock time, and the whole lane about 25 minutes. A Windows verify run takes about 2 minutes and the WSL run about 35 seconds. The draft BACKPORTS.md, the classification table and the verify results are in the deliverables folder.

## Recommendation

Change the pin from 3c4f9fe2 to 14d8a7a2. Create cclj/v2.1 from the bundle; in a clone of upstream, run:
`git fetch <W>\deliverables\cclj-backports.bundle refs/heads/cclj/backports-14d8a7a2:refs/heads/cclj/v2.1`
This gives 14d8a7a2 plus the 17 APPLY picks.

Why:
- 14d8a7a2 gives the same results as 3c4f9fe2 on every harness.
- The picks fix 5 confirmed JIT/VM bugs reachable from sandboxed code, on both Windows and Linux x64, plus the Windows tonumber truncation.
- They keep the new syntax rejected.
- They needed only one trivial conflict resolution, which matches upstream's own merge.
- The ARM64/macOS picks are inert on x64 and are already in place for later targets.

Then:
1. Commit deliverables/BACKPORTS.md as luajit-patches/BACKPORTS.md.
2. Update the README/DESIGN §2 references from `LUAJIT_REV=3c4f9fe2` to `LUAJIT_REV=cclj/v2.1`.
3. Add tools/syntax_probe.lua and tools/fixcheck.lua to the verification scripts, so every future pick is checked for syntax leakage and for JIT/interpreter mismatches.

Keep the 9 DEFER commits off the branch until the bench lane has measured them on CC workloads. They apply cleanly as a group and pass all harnesses (cclj/backports-full-14d8a7a2), so adopting them later is cheap.

Costs and risks:
- This is a maintained fork. Expect about 12 upstream commits a month to triage and about 3-4 picks, in bursts.
- The real risk is a future fix whose context depends on a2ce8114 and has to be ported by hand. None has appeared so far.
- The ARM64/macOS picks and patch 01 on arm64 remain untested until CI or hardware exists.
- a4c06453 was not reproduced by my probe; upstream issue #1535 has the original report.
- On the open version-string question: the fork's jit.version would read "LuaJIT 2.1.<tip commit timestamp>" (2.1.1794068125 for a8f03353), but DESIGN §3.5 removes `jit` from the sandbox. Using it to tell CCLuaJIT apart would mean keeping `jit.version` (or a copy) visible.

## Details

W = <scratch>\work\pin-backports
ART = <repo>\docs\port-1.21.1\artifacts (read-only; the verify scripts were run in place and wrote only to their work-dir).

## Step 1: upstream
- Clone: `git clone --reference <repo>/vendor/luajit --dissociate https://github.com/LuaJIT/LuaJIT $W/luajit`, then `git fetch origin`.
- `git ls-remote` confirms the heads:
  - v2.1 = ea9eb7edb5eac4a9d175d74523db34fac0fa5180, 2026-10-30 19:07:02 +0100, "Don't FOLD ldexp with non-integer exponent.";
  - master = 592f9e3f (2026-10-29), already merged into v2.1.
- 14d8a7a2..origin/v2.1 holds 52 commits: 45 non-merge and 7 merges.
- 3c4f9fe2..14d8a7a2 is exactly one commit (lj_asm_x86.h, +7/-4).

## Step 2: verify on 14d8a7a2
Commands:
- `LUAJIT_REV=14d8a7a2 $ART/luajit-patches/verify-patch-stack.sh $W/luajit $W/verify-14d8a7a2` (2m05s)
- Same-session control: `LUAJIT_REV=3c4f9fe2 ... $W/verify-3c4f9fe2-control` (1m59.8s)

Results, identical for 14d8a7a2 and the control:
- build OK, 8 patches applied, no patch/fuzz fallback;
- preempt: 33 OK, 2 hang (X_finalizer_loop, X_lua_hook_loop);
- patterns: 74 OK, 0;
- C-stack: 31 completed (25 error + 6 ok), 0 crashed;
- compat identical;
- the same 99 gcc -Wswitch warnings, all at lj_opt_fold.c:372.

`tools/outcomes.sh <results-dir>` strips timings, line numbers and paths, leaving 145 lines per run: every case + RESULT, the C-stack verdicts and compat. The reference results/3c4f9fe2, the control, 14d8a7a2, the backport branch and the full branch all give byte-identical files (md5 1fd1f6c23687 for all five).

Single-sample latency maximums were 63 / 66.8 / 78 µs on different cases, including the control run on the old pin. Repeating 3 cases 10 times per build (`results/latency-repeat.txt`): while_true 6-13 µs, pairs_loop 0.5-4.6 µs, gsub_callback 0.4-2.4 µs on every build. The maximums are noise.

## Step 3: classification
Full table: deliverables/classification.md (generated from classification.tsv). Each row gives hash, date, subject, files, category, relevance, syntax dependency and decision. Hashes, dates and subjects were checked against `git log`.

| Hash | Decision | Short reason |
|---|---|---|
| 346ab587 | APPLY (sync) | DynASM movd/movq; lj_vm.o byte-identical (cmp) |
| c5569108 | APPLY | OOM during trace stitching |
| 27f169c6 | APPLY | next() after ITERN despecialization |
| a9981441, 8ca76297 | APPLY (ARM64, inert) | DynASM ARM64 buildvm UB |
| 2c9f5cf6, ce8ec8dc | APPLY (ARM64, inert) | ARM64 VM: JFORI; getmetatable compared key instead of value |
| 9451be00 | APPLY | string.buffer recorder; not actually FFI-gated |
| 849bbbe7 | APPLY | x64 math.floor()/ceil() with no argument read a stale slot |
| bc9261a3 | APPLY | +-0 for-step recording |
| 2a8e017e | APPLY | Windows tonumber(s, base) truncated to 32 bit |
| 716f74a6 | APPLY (macOS, inert) | mcode vs xzone malloc |
| 95f931cb | APPLY | ALEN CSE across a NEWREF rehash; issue #1534 case 1 confirms the bug predates 14d8a7a2 |
| a4c06453 | APPLY | select() guard overflow |
| 55853647 | APPLY | x64 register-allocation fix; trivial conflict |
| 592f9e3f | APPLY (sync) | no-op unless -O-dce |
| ea9eb7ed | APPLY | ldexp folding |
| afa81a57+1edc3e52, 59447257, 52cdb12e+21ecb361+8c23d6aa, 1ee778a4, b310887b+3a658cac | DEFER | performance changes, plus the fixes for bugs they introduced |
| a2ce8114, 0218af7e, 634daf24, c41d727d | SKIP | syntax |
| e4d80516, 4886b676, 5ed524c0, 71797fa7, 3000f7cd, 2410b57f, dd3b2436 | SKIP | FFI-only (checked reachability: CONV num.u32, unsigned DIV, LJ_POST_FIXCONST, CNEWI and LJ_HASFFI blocks exist only with FFI) |
| faaf6633, acf8fcbd, c6ffc141, 3f549af5, e7e9a328 | SKIP | x86-32, PPC, DragonFly, iOS |
| 5f627e41, 2460b3ff | SKIP | docs |
| f30aabe8 | SKIP | jit.* modules |

Syntax dependency: only the 4 syntax commits, 5f627e41 (documents a2ce8114) and part of faaf6633 depend on a2ce8114.

## Step 4: backport branch
cclj/backports-14d8a7a2, tip a8f033531355daa7eae9ca3c4b455e53863ff14b, 17 `git cherry-pick -x` picks (`results/cherry-pick-log.txt`):
- 16 clean;
- 55853647 conflicted in lj_asm_x86.h. `tools/resolve_55853647.py` resolved it; asm_tobit() and asm_conv() are identical to upstream merge 47fc4e39.

Files now identical to upstream HEAD: dasm_arm64.h, dasm_x86.lua, lj_asm.c, lj_mcode.c.

Patch 01-08 check: `git apply --check` then `git apply` for each patch in sequence, on a `git -c core.autocrlf=false archive` export: all 8 ok.

Verify: `LUAJIT_REV=cclj/backports-14d8a7a2 ...verify-patch-stack.sh $W/luajit $W/verify-backports` (2m06s): build OK (8), 33 OK + 2 hang, 74 OK, 31/0, compat identical, outcomes identical to the reference.

Syntax probe (`tools/syntax_probe.lua`): 35 v3.0 snippets must be rejected and 10 Lua 5.2/Cobalt controls accepted.

| Build | Result |
|---|---|
| backport branch | ok=45 bad=0 |
| full branch | ok=45 bad=0 |
| 14d8a7a2 | ok=45 bad=0 |
| 3c4f9fe2 | ok=45 bad=0 |
| plain upstream ea9eb7ed (`mingw32-make BUILDMODE=static XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI"`) | ok=9 bad=36 (all 35 v3.0 snippets accepted; `t.goto` rejected without patch 06) |

Behaviour checks (`tools/fixcheck.lua`, `luajit -Ohotloop=1`): 14d8a7a2 fails 5 of 6 on Windows and 4 of 6 on Linux (the tonumber bug is Windows-only). The branch passes 6/6 on both. a4c06453 was not reproduced on the base by my probe.

WSL Linux x64 run:
- Export: `git -c core.autocrlf=false archive ... | tar -x`.
- Command: `MSYS_NO_PATHCONV=1 wsl -e bash .../tools/linux-sanity.sh <src> <out> <ART> <probe>`. The script uses build-patched-luajit.sh, harness_posix.c + win32_on_posix.h, plinux.c, linux/cstack.c via run_cstack.py 1024 128, compat and the syntax probe.
- Results, the same on base and branch: build OK (8), preempt 33 OK + 2 hang (same cases, X_string_find_backtrack OK), plinux 5/5 fired and 5/5 usable afterwards, C-stack 31 completed / 0 crashed, compat OK, probe 45/0. About 35 s per run.

Extra:
- cclj/backports-full-14d8a7a2 (tip 67261ece) = the 17 picks + the 9 DEFER commits. It had the same single conflict and also passes everything (verify 1m52s, outcomes identical, probe 45/0, fixcheck 6/6). It makes lj_asm_x86.h, lj_opt_mem.c and lib_base.c identical to HEAD.
- 346ab587 codegen check: plain builds of lj_vm.o from 14d8a7a2 and 14d8a7a2+346ab587 compare identical with `cmp`.

## Step 5: ongoing cost
Window: 2026-07-18 to 2026-11-07, 112 days (about 3.7 months).
- Upstream commits per month: Jul 6, Aug 13, Sep 2, Oct 24, Nov 0.
- Fixes for the current targets per month: Jul 0, Aug 2, Sep 0, Oct 8, i.e. 10 in total (about 2.7 a month). Adding ARM64/macOS fixes gives 15 (about 4.1 a month).
- Conflicts: 1 of 17 picks, caused by master/v2.1 divergence, none by a2ce8114.

My time, from file timestamps:
- triage of the 45 commits: about 6 min wall-clock (18:08 diff dump to 18:14 first cherry-pick), including one GitHub issue lookup (#1534);
- whole lane: 18:07 to 18:32.

A human maintainer has not been measured. My estimate: 1-2 h a month, which fits DESIGN's 0.5-1 day.

## Step 6
deliverables/BACKPORTS.md contains the base and the reason for it, the policy, all 45 commits with decision, reason and pick result, the deferred groups, a triage procedure with exact commands and tests, the cost estimate, and open items.

Side findings:
- LuaJIT skips a first line that starts with `#` in every load(), not only in files, unlike PUC 5.2. This predates v3.0; it should be checked against Cobalt.
- LuaJIT treats a +0 for-step as ascending: `for i=1,3,0` loops forever. Cobalt's behaviour should be compared (rom-compat lane).

## Files (scratch workspace)

- `work/pin-backports/deliverables/BACKPORTS.md`
- `work/pin-backports/deliverables/classification.md`
- `work/pin-backports/deliverables/classification.tsv`
- `work/pin-backports/deliverables/verify-results.md`
- `work/pin-backports/deliverables/cclj-backports.bundle`
- `work/pin-backports/deliverables/backports-series`
- `work/pin-backports/deliverables/backports-full-series`
- `work/pin-backports/deliverables/results`
- `work/pin-backports/deliverables/results/cherry-pick-log.txt`
- `work/pin-backports/deliverables/results/fixcheck-windows.txt`
- `work/pin-backports/deliverables/results/fixcheck-linux.txt`
- `work/pin-backports/deliverables/results/syntax-probe-controls.txt`
- `work/pin-backports/deliverables/results/latency-repeat.txt`
- `work/pin-backports/deliverables/tools/syntax_probe.lua`
- `work/pin-backports/deliverables/tools/fixcheck.lua`
- `work/pin-backports/deliverables/tools/linux-sanity.sh`
- `work/pin-backports/deliverables/tools/outcomes.sh`
- `work/pin-backports/deliverables/tools/resolve_55853647.py`
- `work/pin-backports/deliverables/tools/gen_backports_table.py`
- `work/pin-backports/luajit`
