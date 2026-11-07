# LuaJIT base and upstream backports

This file records which upstream LuaJIT commits the CCLuaJIT fork contains on top of its base, and why every other upstream commit was left out. Patches 01–08 (see `README.md`) are a separate layer: they are applied at build time and are not listed here.

## Base

| | |
|---|---|
| Upstream | `https://github.com/LuaJIT/LuaJIT`, branch `v2.1` |
| Base | `14d8a7a27dc8c626ab9e7c7e9e50b6df6def4f03` (2026-07-18), "x64/LJ_GC64: Avoid store-to-load forwarding stall after stack restore." |
| Why this base | It is the last v2.1 commit before `a2ce8114` (2026-07-20, "Backport some v3.0 syntax extensions"). That commit adds `continue`, `?.`, `??`, `?:`, `+=` and friends, bit operators, `!`/`&&`/`\|\|`/`!=`, `const`, short functions and `1_000`. CC:T and Cobalt reject all of them, and upstream has no switch to turn them off. `14d8a7a2` = `3c4f9fe2` + one x64 backend change, and gives identical harness results. |
| Fork branch | `cclj/v2.1` = base + the **APPLY** commits below, cherry-picked with `git cherry-pick -x` in upstream order. Mike Pall stays the author of each commit, and every message ends with `(cherry picked from commit …)`. |
| Recreate it | `git checkout -b cclj/v2.1 14d8a7a2 && git am backports/0*.patch` in a LuaJIT clone with `core.autocrlf=false`. On 2026-11-07 this gave a tree identical to the tip below. `backports-full/` holds the same 17 commits plus the 9 **DEFER** commits, in upstream order. |
| Build | `LUAJIT_REV=cclj/v2.1 ./build-patched-luajit.sh <luajit-clone> <out>`; `verify-patch-stack.sh` takes the same `LUAJIT_REV`. |
| Last upstream commit triaged | `ea9eb7edb5eac4a9d175d74523db34fac0fa5180` (2026-10-30), v2.1 HEAD on 2026-11-07 |
| Branch tip as built | `a8f033531355daa7eae9ca3c4b455e53863ff14b` (17 picks). `luajit -v` prints `LuaJIT 2.1.1794068125`: the number is the committer timestamp of the tip, so it changes with every pick. The sandbox removes `jit`, so Lua code cannot see this string. |
| Verified | 2026-11-07, Windows x64 (MinGW gcc 16.2) and Linux x64 (WSL Ubuntu, gcc 15.2), patches 01–08 with the recommended flags. Results are identical to `results/3c4f9fe2`: preempt 33 OK + 2 expected hangs, patterns 74/74, C-stack 31/0, compat OK, syntax probe 45/45. |

## What we take

1. **APPLY**: correctness fixes in code that is compiled into our build and reachable from it. This includes fixes for platforms on the roadmap (ARM64 Linux/macOS, x64 macOS): they are compiled out on x64, so they cost nothing now and are already in place when those targets ship.
2. **APPLY (sync)**: no-behaviour-change commits, taken only to keep a file identical to upstream, so that later fixes in it pick cleanly. Each must be shown to be a no-op, for example with a byte-identical `lj_vm.o`.
3. **DEFER**: performance changes, and fixes for bugs that a deferred performance change introduced. Take such a group whole or not at all, and only after measuring it; never take the fix without its cause.
4. **SKIP**:
   - v3.0 syntax, and anything that fixes or depends on it;
   - FFI-only code (`LUAJIT_DISABLE_FFI`): `lj_c*.c`, code under `#if LJ_HASFFI`, or IR that only FFI emits (`CNEW`/`CNEWI`, u32/u64 arithmetic);
   - unsupported platforms: x86 32-bit, PPC, MIPS, DragonFly, iOS, consoles;
   - docs;
   - `src/jit/*.lua` tooling.

## Upstream v2.1 commits after the base

All 45 non-merge commits in `14d8a7a2..ea9eb7ed`, including the 8 that came in through merges of `master`. Detailed reasons, files and categories: `backports/classification.md`.

| # | Upstream | Date | Subject | Decision | Why | Pick |
|---|---|---|---|---|---|---|
| 1 | `a2ce8114` | 2026-07-20 | Backport some v3.0 syntax extensions. | SKIP | the v3.0 syntax backport itself | - |
| 2 | `5f627e41` | 2026-07-20 | Document bit operator bytecode compatibility. | SKIP | docs for a2ce8114 | - |
| 3 | `2460b3ff` | 2026-07-20 | Fix documentation about Lua 5.2 extensions/compatibility. | SKIP | docs only | - |
| 4 | `346ab587` | 2026-07-24 | DynASM/x86: Fix movd/movq and vmovd/vmovq operand sizes. | APPLY (sync) | DynASM; lj_vm.o byte-identical; keeps vm_x64.dasc in sync | clean |
| 5 | `e4d80516` | 2026-07-25 | FFI: Fix widening semantics for 64 bit arithmetic. | SKIP | FFI-only (needs CONV num.u32, FFI-only IR) | - |
| 6 | `faaf6633` | 2026-07-28 | x86: Conditionally use SSE3 for 64 bit conversions. | SKIP | x86 32-bit only; lj_meta.c hunk needs a2ce8114 | - |
| 7 | `afa81a57` | 2026-08-01 | x64/LJ_GC64: Enable XLOAD/STRREF fusion. | DEFER (pair with 1edc3e52) | x64 perf; pair with 1edc3e52 | - |
| 8 | `59447257` | 2026-08-01 | Optimize common 1-char case of string.sub/string.byte. | DEFER | recorder perf | - |
| 9 | `4886b676` | 2026-08-01 | FFI: Set cur_L in FFI callback. | SKIP | FFI-only | - |
| 10 | `5ed524c0` | 2026-08-01 | Don't fold -a / -b for unsigned operands. | SKIP | unsigned DIV IR exists only with FFI | - |
| 11 | `f30aabe8` | 2026-08-03 | Modernize jit.* Lua modules. | SKIP | jit.* Lua modules, not shipped | - |
| 12 | `1edc3e52` | 2026-08-03 | x64/LJ_GC64: Fix XLOAD fusion. | DEFER (pair with afa81a57) | fixes afa81a57 only | - |
| 13 | `52cdb12e` | 2026-08-18 | Compile unpack(). | DEFER (group with 21ecb361, 8c23d6aa) | new recorder feature (perf); 2 follow-up fixes | - |
| 14 | `71797fa7` | 2026-08-18 | FFI: Mark cts->L in atomic phase. | SKIP | FFI-only (#if LJ_HASFFI) | - |
| 15 | `c5569108` | 2026-08-18 | Handle OOM error during trace stitching. | APPLY | OOM during stitching; memory cap makes OOM routine | clean |
| 16 | `27f169c6` | 2026-08-18 | Fix next() recording immediately after ITERN despecialization. | APPLY | next()/pairs recording | clean |
| 17 | `21ecb361` | 2026-08-19 | Fix unpack() recording. | DEFER (with 52cdb12e) | fixes 52cdb12e only | - |
| 18 | `8c23d6aa` | 2026-08-19 | Fix unpack() recording. | DEFER (with 52cdb12e) | fixes 52cdb12e only | - |
| 19 | `1ee778a4` | 2026-08-19 | Add FOLD rule for ALEN of TNEW/TDUP. | DEFER | optimizer perf | - |
| 20 | `3000f7cd` | 2026-09-03 | FFI: Adjust J->maxslot after ffi.offsetof. | SKIP | LJ_POST_FIXCONST is FFI-only | - |
| 21 | `c6ffc141` | 2026-09-08 | PPC: Fix string.* fast functions with P64 set (testing only). | SKIP | PPC only | - |
| 22 | `0218af7e` | 2026-10-13 | Allow empty statements (semicolons) anywhere. v3.0 backport. | SKIP | v3.0 syntax (`return;;`) | - |
| 23 | `a9981441` | 2026-10-13 | DynASM/ARM64: Prevent sanitizer warning in dasm_imm13(). | APPLY (ARM64, inert on x64) | ARM64 buildvm UB; inert on x64 | clean |
| 24 | `3f549af5` | 2026-10-13 | Fix concurrent allocation issue on DragonFly (unsupported). | SKIP | DragonFly only | - |
| 25 | `2c9f5cf6` | 2026-10-13 | ARM64: Fix BC_JFORI on empty FP range. | APPLY (ARM64, inert on x64) | ARM64 VM; inert on x64 | clean |
| 26 | `ce8ec8dc` | 2026-10-13 | ARM64: Fix getmetatable() for cleared __metatable field. | APPLY (ARM64, inert on x64) | ARM64 VM; inert on x64 | clean |
| 27 | `9451be00` | 2026-10-13 | FFI: Fix recording of string buffer put() method. | APPLY (compiled-in code) | string.buffer recorder (compiled in; unreachable in sandbox) | clean |
| 28 | `acf8fcbd` | 2026-10-13 | x86: Fix math.floor()/ceil() argument check. | SKIP | x86 32-bit only | - |
| 29 | `849bbbe7` | 2026-10-13 | x64: Fix math.floor()/ceil() argument check. | APPLY | x64 VM: `math.floor()` read a stale slot (verified) | clean |
| 30 | `bc9261a3` | 2026-10-13 | Don't record for loops with +-0 step. | APPLY | JIT vs interpreter mismatch with +-0 step (verified) | clean |
| 31 | `634daf24` | 2026-10-13 | Fix parsing of safe-navigation operator. | SKIP | fixes `?.` syntax | - |
| 32 | `2a8e017e` | 2026-10-14 | Windows: Use 64 bit conversion for tonumber() with base. | APPLY | Windows tonumber(s, base) truncated to 32 bit (verified) | clean |
| 33 | `2410b57f` | 2026-10-18 | FFI: Don't drop signedness in FOLD rule for FLOAD of CNEWI. | SKIP | FFI-only (CNEWI) | - |
| 34 | `716f74a6` | 2026-10-18 | macOS: Avoid collision of mcode with xzone malloc reclaim ring. | APPLY (macOS, inert on Win/Linux) | needed for macOS; inert elsewhere | clean |
| 35 | `e7e9a328` | 2026-10-18 | iOS: Disable non-functional os.tmpname(). | SKIP | iOS only; os removed | - |
| 36 | `dd3b2436` | 2026-10-18 | FFI/ARM64e: Sign FFI callback pointers. | SKIP | FFI-only (ARM64e) | - |
| 37 | `c41d727d` | 2026-10-20 | Add line number directive. v3.0 backport. | SKIP | v3.0 syntax (line directive) | - |
| 38 | `95f931cb` | 2026-10-20 | Check for IR_NEWREF in IR_ALEN forwarding. | APPLY | ALEN CSE across rehash; bug predates base (verified) | clean |
| 39 | `a4c06453` | 2026-10-21 | Don't compile select() indexing beyond varargs. | APPLY | select() guard overflow (#1535) | clean |
| 40 | `b310887b` | 2026-10-29 | x64/LJ_GC64: Avoid s2l fwd stall after stack restore, part 2. | DEFER (pair with 3a658cac) | x64 perf; pair with 3a658cac | - |
| 41 | `55853647` | 2026-10-29 | x86/x64: Use scratch register for IR_TOBIT and IR_CONV u64.num. | APPLY | x64 register allocation; master-side | trivial conflict, resolved as upstream merge 47fc4e39 |
| 42 | `592f9e3f` | 2026-10-29 | x86/x64: Don't sink IR_CONV num.int with -O-dce. | APPLY (sync; not affected) | no-op with default flags; lj_asm.c = upstream | clean |
| 43 | `8ca76297` | 2026-10-29 | DynASM/ARM64: Prevent sanitizer warning in dasm_encode(). | APPLY (ARM64, inert on x64) | ARM64 buildvm UB; inert on x64 | clean |
| 44 | `3a658cac` | 2026-10-30 | x86/x64: Fix for change to avoid s2l fwd stall in stores. | DEFER (pair with b310887b) | fixes b310887b only | - |
| 45 | `ea9eb7ed` | 2026-10-30 | Don't FOLD ldexp with non-integer exponent. | APPLY | ldexp fold mismatch (verified) | clean |

Totals: APPLY 17 (10 fixes for the current x64 targets, 5 for ARM64/macOS, 2 sync), DEFER 9, SKIP 19.

### Deferred group (performance)

| Group | Commits | Revisit when |
|---|---|---|
| x64 GC64 XLOAD/STRREF fusion | `afa81a57` + `1edc3e52` | perf work on string-heavy code |
| 1-char `string.sub`/`byte` | `59447257` | same (it emits the XLOAD that the fusion group fuses) |
| Compile `unpack()` | `52cdb12e` + `21ecb361` + `8c23d6aa` | perf work; `unpack` is common in CC code |
| ALEN fold of TNEW/TDUP | `1ee778a4` | with the unpack group; its second bug was fixed by `95f931cb`, which we already have |
| x64 s2l stall, part 2 | `b310887b` + `3a658cac` | perf work on side-exit-heavy code (part 1 is our base) |

All 9 were also cherry-picked onto a test branch (`cclj/backports-full-14d8a7a2`): clean, apart from the same 55853647 conflict. That branch passed every harness on Windows. It makes `lj_asm_x86.h` and `lj_opt_mem.c` identical to upstream. Taking the whole group is therefore cheap; what remains is to measure its benefit (bench lane, CC benchmarks) before adopting it.

## Triage procedure

Run it monthly. Run it immediately when upstream touches `lj_record.c`, `lj_ffrecord.c`, `lj_asm*.h`, `lj_opt_*.c`, `lj_err.c`, `lj_state.c`, `lj_gc.c`, `lj_dispatch.*`, `lj_trace.c`, `lj_parse.c`, `lj_str.c`, `lib_*.c` or `vm_x64.dasc`/`vm_arm64.dasc`. Most of these are files that patches 01–08 also edit.

1. **List the new commits.** In a clone, fetch upstream and list every non-merge commit since the last triaged one:
   ```
   git fetch origin
   git log --no-merges --reverse --topo-order --format='%h %ad %s' --date=short <last-triaged>..origin/v2.1
   ```
   This includes master-side fixes. To see which merge brought each in, run `git log --merges --format=%h <last>..origin/v2.1` and, for each merge, `git log --no-merges --format=%h <merge>^1..<merge>^2`.
2. **Classify each commit** (`git show --stat <c>`, then the diff), using the rules above. Quick tests:
   - **Syntax:** "v3.0 backport" in the subject; `lj_lex.*`/`lj_parse.c` changes involving tokens added by `a2ce8114` (`TK_continue`, `TK_const`, `TK_nav`, `TK_coal`, `TK_arrow`, `TK_and_`, `TK_or_`, `TK_ne_`, `TK_shl`, `TK_shr`, `TK_sar`); or bit-op bytecodes and `lj_meta_bitop`. Also check `git apply --check` of the commit on `cclj/v2.1`: a context conflict in `lj_parse.c`/`lj_lex.c` usually means syntax code.
   - **FFI-only:** all hunks in `lj_c*.c`/`lib_ffi.c`, under `#if LJ_HASFFI`, or reachable only through FFI IR. "FFI:" in the subject is not enough: `9451be00` was string.buffer code that is compiled in.
   - **Platform:** check the file (`vm_<arch>.dasc`, `lj_asm_<arch>.h`, `lj_emit_<arch>.h`, `lj_target_<arch>.h`, `dynasm/dasm_<arch>.*`) or the `#if LJ_TARGET_*` guard.
   - **Perf vs fix:** subjects starting with "Optimize", "Enable", "Compile", "Add FOLD rule", "Avoid … stall" are performance changes. A "Fix …" commit that only touches code added by a deferred commit belongs to that commit's group.
   - **Reachability:** check the curated environment (DESIGN §3.5). `jit`, `io`, `os`, `package`, `ffi`, `bit` and `debug.sethook` are gone, and JIT options are fixed by the host.
3. **Write a reproducer** when the upstream issue has one, or when it is cheap to write. Add it to `tools/fixcheck.lua` (JIT vs interpreter, `-Ohotloop=1`), and check that it FAILS on the current tip before picking.
4. **Cherry-pick** in upstream order with `git cherry-pick -x`.
   - **Master-side commits:** on a conflict, resolve to what upstream's merge commit produced (`git show --cc <merge>`). Then check with `diff <(git show <merge>:<file> | sed -n '/^static void f/,/^}/p') <(sed -n '…' <file>)`.
   - **Hand-porting:** if a fix depends on context from `a2ce8114`, port it only if the bug exists in our tree (prove it with the reproducer). Note it as **APPLY (hand-port)** with the differences.
5. **Verify:**
   - `LUAJIT_REV=cclj/v2.1 ./verify-patch-stack.sh <clone> <work>` on Windows; expect 33 OK + 2 hang, 74 OK, 31/0, compat as before. Compare `tools/outcomes.sh <work>/results` with the previous run: it must be byte-identical.
   - `tools/linux-sanity.sh` under WSL: preempt via `harness_posix.c`, plinux, C-stack, compat, syntax probe.
   - `luajit tools/syntax_probe.lua` must print `ok=45 bad=0`.
   - `luajit -Ohotloop=1 tools/fixcheck.lua` must PASS everything.
   - Once CI exists, the macOS/ARM64 jobs cover the platform picks.
6. **Record:** add a row per commit to the table above, update "Last upstream commit triaged", "Branch tip as built" and the version string, and save the logs under `results/<tip>/`.

Expected cost, based on 14d8a7a2 → ea9eb7ed (112 days, about 3.7 months):
- about 12 upstream commits a month to read;
- about 2.7 fixes a month for the current targets, plus about 1.4 for ARM64/macOS;
- arrival is bursty: 0 in July and September, 8 in October;
- 1 trivial conflict in 17 picks, caused by master/v2.1 divergence; none so far caused by the syntax backport;
- a verify run takes about 2 min on Windows and about 35 s on WSL.

The 0.5–1 day/month budget in DESIGN §2 is enough. The risk that would exceed it is the first fix that has to be hand-ported around `a2ce8114` code.

## Open items

- Measure the deferred group on the CC benchmarks, then decide on it as a whole.
- Consider `-DLUAJIT_DISABLE_BUFFER`, defence in depth like `DISABLE_PROFILE`. `string.buffer` is compiled in but can be reached only through `package.preload`, which the sandbox removes. Untested.
- Revisit the pin strategy if upstream ever adds a switch for the v3.0 syntax, or if hand-ports start to pile up. The alternative is to track HEAD with a lexer "syntax gate" patch.
  - A verified kit for that alternative is in `head-kit/`: patch 06 ported to HEAD, `gate-v3-syntax.patch`, and the differential parse test.
  - On `ea9eb7ed` it gave 0 differences from `3c4f9fe2` + 01–08 over 113,872 inputs (ROM, handwritten cases and fuzz), plus harness parity.
  - It is not recommended yet, because upstream is actively rewriting the lexer and parser (`reports/pin-head-gate.md`).
