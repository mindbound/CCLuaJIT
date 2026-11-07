# LuaJIT base alternative: upstream HEAD + syntax gate

*Written on 2026-11-07 by an investigation agent (decision 5/6 in PLAN.md). Reusable outputs were copied to `../artifacts/luajit-patches/head-kit/` (the HEAD port of 06, `gate-v3-syntax.patch` (named 09-syntax-gate.patch in this report), difftest/, logs/). Paths under `scratchpad/work/...` refer to the session scratch workspace and no longer exist.*

## Summary

Tracking upstream v2.1 HEAD with a syntax gate works. HEAD is ea9eb7edb5eac4a9d175d74523db34fac0fa5180 (2026-10-30), which is 53 commits (46 non-merge) after 3c4f9fe2. Rebasing the stack onto it, only patch 06 conflicted (5 of 6 hunks), because upstream folded every goto-as-name check into a new lex_isname() helper. The port is 3 small hunks. Patches 01–05, 07 and 08 apply with `git apply`; some hunks needed line offsets. The new patch 09-syntax-gate.patch is enabled with -DLUAJIT_DISABLE_V3_SYNTAX. It touches lj_lex.c, lj_lex.h and lj_parse.c in 20 hunks (+61/−24 lines). I tested it against 3c4f9fe2+01-08 on 113,872 inputs:
- the CC:T ROM (104 rom/*.lua plus bios.lua, which is every .lua in the core jar);
- 302 handwritten cases (112 new constructs, 190 Lua 5.2-isms and old syntax);
- 113,465 fuzz inputs.

Results are identical on both sides: same compile success, same error text, and byte-identical deterministic string.dump output for every input that compiles. This holds on Windows (release and assert builds) and on WSL Linux. Of the 112 new-construct cases, 110 fail on both builds with identical messages. The other 2 are valid old code that the line-number directive would change (`print(\n#5)` and a first line starting with `#`); both builds accept them with the old meaning.

The first, token-only version of the gate was not enough. The test found two old-syntax regressions in upstream's parser rewrite:
- an uninitialized `fs.minline` read, so `function f(<201 params>)` reported "function at line 12714177" or "main function";
- changed debug line numbers when a `.`/`:` access starts a new line, so a runtime error in `local x = a.b\n  .c` reports line 3 instead of line 2.

Patch 09 fixes both. With it, verify-patch-stack.sh on HEAD gives the reference numbers: 9 patches applied, preemption 33 OK + 2 expected hangs, pattern 74 OK, C-stack 31/0, and compat_check identical. The Linux POSIX harnesses also pass.

The gate has costs:
- It lives entirely in the code upstream is changing most right now: 4 lexer/parser commits in the 3.5 months since the pin, against about 3 per year in 2021–2024.
- When I applied it to earlier states in that period, 6 of its 20 hunks failed at a2ce8114 and 3 failed at 0218af7e and 634daf24.
- Upstream's jit.* tool modules now use the new syntax, so `-jv`, `-jdump` and `-bl` don't load in a gated build.

My recommendation: do not switch to continuous HEAD tracking yet. Keep the 3c4f9fe2 pin plus backports, and keep 06-port/09/difftest as a ready kit for a later pin bump.

## Recommendation

Do not switch to continuous HEAD tracking yet. Keep the DESIGN.md plan of pinning 3c4f9fe2 (or 14d8a7a2) plus backports, and check the ported 06, 09-syntax-gate.patch and the difftest tooling into luajit-patches as a ready, verified kit for a later pin bump.

Reasons:
1. The gate is correct today. It showed zero differences on 113,872 inputs, including bytecode identity, and harness parity on Windows and Linux. But all of it sits in the code upstream is actively rewriting: 4 lexer/parser commits in 3.5 months, against about 3 per year before. Applied to earlier states in that period, it would have broken on 2 of the 3 commits after the merge, and upstream lists more features that could still be backported.
2. The obvious token-level gate was not enough. The difftest caught two non-token regressions in upstream's parser rewrite: uninitialized `minline`, and the line attribution of `.` chains split across lines. Every pin bump will need the same careful rerun and possibly new fixes. That is recurring parser work for a personal project on 6–8 h/week.
3. The relevant fixes are cheap to get the other way: 21 of the 22 Windows/Linux x64 fixes cherry-pick cleanly onto 3c4f9fe2, though they are not yet built or tested.
4. Tracking HEAD breaks the jit.* diagnostics (`-jv`, `-jdump`, `-bl`) in the shipping build.

Re-evaluate when upstream has had about 3 months without lexer/parser commits, or when backports onto 3c4f9fe2 start conflicting regularly. At that point, move the pin to a newer HEAD in one step: rebase 01–08 (only 06 has needed porting so far), port 09, then run verify-patch-stack.sh and `difftest/compare.py` (about 15 minutes of machine time).

Costs and risks of this recommendation:
- The monthly triage stays (0.5–1 day/month already budgeted), and cherry-picks will get harder as files diverge (a2ce8114 changed vm_*.dasc and lj_record.c).
- The 22 backports are verified only at the git level.
- The pre-existing gaps against Cobalt (0b literals, BOM, high-byte identifiers, the 60-upvalue limit, `\q`, `goto goto`) remain whichever base you choose and deserve their own triage.
- If you choose HEAD anyway:
  - use 09 as is;
  - add an ungated tools build for -jv/-jdump;
  - report the minline undefined-behaviour bug upstream (it also affects ungated builds);
  - renumber 09, because the README reserves 09 for LUAI_MAXCSTACK.

## Details

All paths are under W = <scratch>\work\pin-head-gate. Nothing in <repo> was modified. Its scripts were only run, with output written to W.

## 1. Clone and HEAD
`git clone --reference <repo>/vendor/luajit --dissociate https://github.com/LuaJIT/LuaJIT W/luajit`
- origin/v2.1 = ea9eb7edb5eac4a9d175d74523db34fac0fa5180, 2026-10-30 19:07:02 +0100, "Don't FOLD ldexp with non-integer exponent".
- Pin 3c4f9fe2052b8d08a917ac0d5f38563f0297b5a3 is dated 2026-07-11.
- `git rev-list --count 3c4f9fe2..ea9eb7ed` = 53; with `--no-merges` = 46.
- I set `core.autocrlf=false` in the clone. The global setting is true and checks out CRLF; the builds use `git archive`, which is unaffected.

## 2. Language changes after 3c4f9fe2
`git log 3c4f9fe2..ea9eb7ed -- src/lj_lex.c src/lj_lex.h src/lj_parse.c` lists exactly 4 commits: a2ce8114, 0218af7e, 634daf24 and c41d727d. 5f627e41 and 2460b3ff change docs only. The full list with files is in logs/40-commits-since-pin.txt.

**a2ce8114 (2026-07-20), "Backport some v3.0 syntax extensions":** 28 files, +1510/−234.
- New tokens in lj_lex.h TKDEF: `?.` (TK_nav), `??` (TK_coal), `<<`, `>>`, `~>>`, `&&`, `||`, `!=`, `->`.
- New reserved words `const` and `continue`. They are "soft": lex_isname() still accepts them as names.
- Single-character tokens that gained a meaning: `!` (not), `~` (unary BNOT and binary BXOR), `&`, `|` (bit operators; `|` also opens short-function parameters), `?` (ternary).
- Grammar:
  - bit operators `& | ~ << >> ~>>` and unary `~`;
  - `! && || !=`;
  - right-associative ternary `c ? a : b`;
  - safe navigation `a?.b`, `a?.[k]`, `a?.(x)`, `a:m?.()`, including as an assignment target;
  - `??`;
  - compound assignment `+= -= *= /= %= &= |= ~= <<= >>= ~>>= ..=` (no `^=`);
  - `continue`, as a statement only when followed by end/else/elseif/until/EOF or `;`;
  - `const` declarations (`const a, b = ...`, `const function f`), with compile errors on assignment or re-declaration;
  - short functions `|a,b| -> e`, `|| -> e`, `x -> e`, `-> do ... end`;
  - underscores in number literals (`1_000`, `0x_ff`, `0_x10`, `1e1_0`).
- New precedence table: the old priority values are remapped monotonically, so the old operators keep their relative order.
- 7 new bytecodes BNOT, BAND, BOR, BXOR, BSHL, BSHR, BSAR, inserted after JMP. They come with PROTO_BITOP, BCDUMP_F_BITOP, lj_meta_bitop and recording in lj_record.c.
- New error messages: XCONT, XCSCOPE, XCONSTA, XCONSTR.
- Internal parser rewrite: hashed variable lookup (vhash), proto_begin/proto_finish, VCALLNAV.

**0218af7e (2026-10-13):** semicolons allowed anywhere. `case ';'` and parse_label's `;` lose their `LJ_52` guard. parse_chunk eats any number of `;`, so under LUA52COMPAT the only change is `return;;`.

**634daf24 (2026-10-13):** fixes `?.` parsing.

**c41d727d (2026-10-20):** line number directive `#<n> ["name"]` at the start of a line (lex_linedir), plus a new error XLINEDIR. A `#` first line is now skipped only if it is `#!` and buffered; otherwise it is parsed as a directive or a `#` token. linedefined is replaced by minline/maxline.

**Library side:** no new Lua library functions. f30aabe8 rewrote the jit.* modules (bc, bcsave, dump, v, p, dis_*) in the new syntax.

**Existing defines:** none of these is gated by any existing define. LUA52COMPAT does not affect them, and 0218af7e removed a former LJ_52 condition.

## 3. Rebasing 01-08 onto ea9eb7ed
Logs: logs/01-apply-check-head.txt, logs/02-apply-sequential-head.txt, logs/03-patch-offsets-head.txt. Each patch was applied in sequence with `git apply -p1`, falling back to `patch --dry-run`.
- 01–05, 07 and 08: `git apply` clean. `patch` reports offsets only, for example 08's lj_parse.c synlevel_begin hunk at +233 lines.
- 06 conflicts: "Hunk #2..#6 FAILED, 5 out of 6 hunks FAILED". Upstream folded the four `TK_name || (!LJ_52 && TK_goto)` checks (lex_str, expr_table, parse_params, expr_primary) into `lex_isname()` and changed the goto statement test to `LJ_52 || lex_isname(lj_lex_lookahead(ls))`.
- Resolution (W/artifacts/luajit-patches/06-goto-contextual.patch, 3 hunks):
  - keep the `LJ_GOTO_RESERVED` define;
  - `!LJ_52` becomes `!LJ_GOTO_RESERVED` in lex_isname();
  - `LJ_52` becomes `LJ_GOTO_RESERVED` in parse_stmt case TK_goto.
- Side effect without the gate: upstream's soft-keyword lookahead also accepts `goto goto`. 3c4f9fe2+06 rejects it with "'=' expected near 'goto'"; Cobalt 0.9.9 accepts it. The gate restores the 3c4f9fe2 rule.

## 4. Gate patch: W/artifacts/luajit-patches/09-syntax-gate.patch
3 files, 20 hunks (lj_lex.c 7, lj_lex.h 1, lj_parse.c 12), `git diff --numstat` = 21/14, 9/0, 31/10. `LJ_V3SYNTAX` appears 27 times.
- **lj_lex.h:** `LJ_V3SYNTAX` is 0 under `-DLUAJIT_DISABLE_V3_SYNTAX`.
- **lj_lex.c:**
  - no `_` skipping in lex_number;
  - no directive after a newline;
  - lj_lex_setup skips any `#` first line, as before;
  - `->`, `<<`, `>>`, `~>>`, `!=`, `?.`, `??`, `&&` and `||` are never produced as tokens;
  - lj_lex_init does not mark `const` or `continue` as reserved, so they lex as plain names.
- **lj_parse.c:**
  - `#if LJ_V3SYNTAX` around the `|` short function and the `& | ~` binary operators;
  - `!` and `~` unary operators gated;
  - ternary `?` gated;
  - parse_compound returns 0;
  - `;` handling restored: `lex_opt` in parse_chunk, `#if LJ_52||LJ_V3SYNTAX` on `case ';'` and in parse_label, so non-52 builds are exact too;
  - goto lookahead is `== TK_name`;
  - new `lex_optfield()`: under the gate it discharges the prefix before skipping `.`/`:` (3 call sites, in expr_primary_nav and parse_func), restoring the pre-a2ce8114 line attribution;
  - **unconditional:** `fs.minline = line;` in parse_body before parse_params(). Upstream reads it uninitialized in err_limit(); this is undefined behaviour and worth reporting upstream.
- Effect on unreachable code: the bitop VM/JIT code stays compiled in, but nothing can emit those bytecodes. The parser never does, and patch 03 refuses bytecode loading.

## 5. Build and verify
- Build-script copy: W/artifacts/luajit-patches/build-patched-luajit.sh adds `-DLUAJIT_DISABLE_V3_SYNTAX`. The harnesses and lua/ directories were copied to W/artifacts.
- Builds (logs/10..14): `LUAJIT_REV=ea9eb7ed W/artifacts/luajit-patches/build-patched-luajit.sh W/luajit W/builds/head-gated` gives 9 patches and 0 warnings. The 3c4f9fe2 reference build, made with the original script, prints 99 -Wswitch warnings from lj_opt_fold.c:372.
- Verify command (from W): `LUAJIT_REV=ea9eb7edb5eac4a9d175d74523db34fac0fa5180 artifacts/luajit-patches/verify-patch-stack.sh $PWD/luajit $PWD/verify-head`. Output in logs/21-verify-patch-stack-head-final.txt and W/verify-head/results/:
  - build: OK (9 patches applied);
  - preempt harness: 33 OK, 2 hang (X_finalizer_loop, X_lua_hook_loop);
  - pattern harness: 74 OK, 0;
  - cstack harness: 31 completed, 0 crashed;
  - compat_check identical to results/3c4f9fe2/compat.txt once paths are masked.
- Comparison with the reference results:
  - the RESULT histograms of preempt.txt and pharness.txt match exactly;
  - the cstack shape and outcome list matches;
  - preemption latency is min 0.7 / median 3.5 / max 11.4 µs here, against 0.8 / 3.6 / 15.4 µs for the 3c4f9fe2 reference.
- An earlier run with the draft gate had the same numbers (logs/20-…-WIPgate.txt).
- Linux (WSL Ubuntu, gcc 15.2), command `W/linux-check.sh`, log logs/50-linux-check.txt:
  - both builds OK, 0 warnings;
  - differential parse: 0 differences;
  - harness_posix intr: 26 OK, 2 expected hangs;
  - plinux: 5/5 interrupted at 2.4–3.2 µs;
  - cstack: 31/0;
  - compat_check OK.

## 6. Differential parse test (W/difftest)
**Corpus**
- `python -I extract_rom.py <core jar> corpus/rom` extracted 105 files. The jar contains exactly 105 .lua files: 104 under rom/ plus bios.lua.
- `python -I gen_corpus.py corpus/rom packs` (seed 20261107) writes 6 packs:
  - hand.pack: 302 cases, 112 `new.*` plus 190 `l52.*` (goto and labels, goto/continue/const as fields, locals, params, functions and labels, long strings and comments, about 30 numeric edge cases, escapes, `;`, `#` lines, limits, assorted syntax errors);
  - rom.pack: 105;
  - soup.pack: 60,000 token-soup inputs;
  - gram.pack: 25,000 grammar-generated inputs with mutations;
  - rommut.pack: 3,150 ROM mutants;
  - nlsplit.pack: 25,315 inputs with spaces turned into newlines.

**Method**
- Driver: `<build>/src/luajit.exe parsediff.lua packs/<p>.pack out/<build>/<p>.txt [dumpdir]` calls loadstring(src, "="..id). It records the error text, or the length and FNV-1a of `string.dump(f,"d")`.
- The dump must use deterministic mode: the default dump of a table template varied between runs of the same binary.
- Compare: `python -I compare.py out/ref-3c4f9fe2 out/head-gated out/head-nogate`. For rom it also compares the full .bc files byte by byte.

**Results**
- logs/30-difftest-ref-vs-gated.txt: every pack shows 0 differences, and all 105 ROM dumps are byte-identical. OK/ERR counts per pack: hand 105/197, rom 105/0, soup 3069/56931, gram 8842/16158, rommut 953/2197, nlsplit 8830/16485. TOTAL DIFFERENCES: 0.
- The same holds for the LUA_USE_ASSERT/APICHECK builds (logs/31…). No assertion fired in any build.
- The ungated HEAD build (ea9eb7ed+01-08) accepts 95 of the 112 new.* cases, which confirms the corpus really uses new syntax. It rejects the other 17 with new-syntax messages such as "no loop to continue" or "malformed line number directive".
- Table of every new construct with the messages of 3c4f9fe2+01-08, gated HEAD, ungated HEAD and Cobalt 0.9.9: logs/32-new-constructs-table.txt. Cobalt is run through `java -cp cobalt-0.9.9.jar CobaltParse.java`.
  - Examples of identical messages: `x = a & b` gives "unexpected symbol near '&'"; `x += 1` gives "'=' expected near '+'"; `x = 1_000` gives "malformed number near '1_000'"; `return;;` gives "'<eof>' expected near ';'"; `for i=1,2 do continue end` gives "'=' expected near 'end'".
  - Cobalt rejects all 110 as well; its wording sometimes differs, for example "syntax error near '+'".
- **Differences found and fixed:**
  - the minline undefined behaviour: 3c4f9fe2 reports "function at line 3 has more than 200 local variables", ungated HEAD reports "function at line 12714177…";
  - gram.528 bytecode, caused by `.` line attribution.
  - The first gate draft run is in logs/29-…txt. Runtime effect: logs/34 shows `local x = a.b\n  .c` reporting line 2 on 3c4f9fe2 and the gated build, and line 3 on ungated HEAD.
- What ungated HEAD changes for old programs: logs/36-ungated-head-old-syntax-changes.txt.
  - files whose first line is `# comment`, or `#` followed by a BOM, stop compiling ("unexpected symbol near '#'");
  - `local n =\n#5` stops compiling;
  - `goto goto`, `const x = 1` and `x ~= 1` now compile.
  - The gated build matches 3c4f9fe2 on every one of these cases.
- Linux and Windows outputs differ in 2 cases (gram.2525 and nl:gram.2525), identically for both 3c4f9fe2 and gated HEAD. The cause is that pow() constant-folding `0x1F ^ 0x1F` differs by 1 ulp: 0x1.7eb441346ffep+153 with MinGW/msvcrt, 0x1.7eb441346ffe1p+153 with glibc (logs/50).
- ROM compile time, best of 5 × 20 reps (logs/35): 3c4f9fe2 5.35–5.40 ms, gated 5.20–5.25 ms, ungated 5.20–5.25 ms. The gate has no measurable cost.

## 7. Maintenance
- **Invasiveness:** 20 hunks in the lexer's main scanner, lex_number, lj_lex_setup and lj_lex_init, and in 11 parser functions. All of them are in code that the v3.0 backport series changed; none touches VM, JIT or 01–08 territory.
- **Upstream churn:** lexer/parser commits per year before the pin were 2021: 3, 2022: 3, 2023: 3, 2024: 3, 2025: 7 (including a revert pair and a copyright bump). After the pin there were 4 in 3.5 months, all syntax work. None of the other 42 non-merge commits touched lj_lex.* or lj_parse.c.
- **Applying the gate to earlier upstream states** (logs/41): 6 of 20 hunks fail at a2ce8114, 3 of 20 at 0218af7e and 634daf24, 0 at c41d727d. So each of the two feature commits after the merge would have broken the gate textually and also needed new gate logic.
- **More syntax backports are possible:** upstream's documentation lists features still not backported: bit-operator metamethods, `//`, compound-assignment metamethods and `...name`. Each would need more gate work plus a rerun of difftest.
- **Tooling:** in a gated build, `luajit -bl` fails with "jit/bcsave.lua:115: 'end' expected ... near '?'" and `-jv` with "jit/v.lua:110: 'then' expected near '!'" (logs/34). The project's preempt-patch report relied on `-jdump=im` and `-jv`. Work around it with an ungated tools build of the same tree.
- **Non-syntax fixes gained:** 39 commits (46 non-merge minus 4 syntax, 2 docs and the f30aabe8 jit.* module rewrite).
  - 9 x64 backend: 14d8a7a2, 346ab587, afa81a57, 1edc3e52, 849bbbe7, b310887b, 55853647, 592f9e3f, 3a658cac.
  - 13 portable VM/JIT, Windows lib: 59447257, 5ed524c0, 52cdb12e, c5569108, 27f169c6, 21ecb361, 8c23d6aa, 1ee778a4, bc9261a3, 95f931cb, a4c06453, ea9eb7ed, 2a8e017e.
  - 4 ARM64: 2c9f5cf6, ce8ec8dc, a9981441, 8ca76297.
  - 7 FFI-only, irrelevant because FFI is disabled.
  - 2 x86-32 only: faaf6633, acf8fcbd.
  - 4 other platforms, including 716f74a6 (macOS).
  - So about 22 are relevant to the Windows/Linux x64 build.
- **Cherry-picking those 22 onto 3c4f9fe2 instead** (logs/42; git-level only, not built or tested): 21 apply clean; 1 conflicts (55853647, lj_asm_x86.h).
- **Pre-existing CC:T portability gaps** (present in 3c4f9fe2 too; seen with the Cobalt oracle, logs/33):
  - LuaJIT accepts `0b101` literals, a UTF-8 BOM and high-byte identifiers; Cobalt rejects them;
  - LuaJIT rejects `"\q"` and 61 upvalues; Cobalt accepts them;
  - `goto goto`, as noted in §3.
- **Numbering clash:** the README already reserves "09" for LUAI_MAXCSTACK. Renumber the gate when integrating it.

## Files (scratch workspace)

- `work/pin-head-gate/artifacts/luajit-patches/09-syntax-gate.patch`
- `work/pin-head-gate/artifacts/luajit-patches/06-goto-contextual.patch`
- `work/pin-head-gate/artifacts/luajit-patches/build-patched-luajit.sh`
- `work/pin-head-gate/logs/01-apply-check-head.txt`
- `work/pin-head-gate/logs/02-apply-sequential-head.txt`
- `work/pin-head-gate/logs/03-patch-offsets-head.txt`
- `work/pin-head-gate/logs/10-build-ref-3c4f9fe2.txt`
- `work/pin-head-gate/logs/11-build-head-gated.txt`
- `work/pin-head-gate/logs/21-verify-patch-stack-head-final.txt`
- `work/pin-head-gate/logs/29-difftest-first-gate-draft-nondeterministic-dumps.txt`
- `work/pin-head-gate/logs/30-difftest-ref-vs-gated.txt`
- `work/pin-head-gate/logs/31-difftest-ref-vs-gated-assertbuilds.txt`
- `work/pin-head-gate/logs/32-new-constructs-table.txt`
- `work/pin-head-gate/logs/33-l52-cases-table.txt`
- `work/pin-head-gate/logs/34-runtime-lineinfo-and-jit-tools.txt`
- `work/pin-head-gate/logs/35-rom-compile-time.txt`
- `work/pin-head-gate/logs/36-ungated-head-old-syntax-changes.txt`
- `work/pin-head-gate/logs/40-commits-since-pin.txt`
- `work/pin-head-gate/logs/41-gate-replay-on-older-revs.txt`
- `work/pin-head-gate/logs/42-cherry-pick-relevant-onto-3c4f9fe2.txt`
- `work/pin-head-gate/logs/50-linux-check.txt`
- `work/pin-head-gate/verify-head/results/preempt.txt`
- `work/pin-head-gate/verify-head/results/pharness.txt`
- `work/pin-head-gate/verify-head/results/cstack.txt`
- `work/pin-head-gate/verify-head/results/compat.txt`
- `work/pin-head-gate/difftest/gen_corpus.py`
- `work/pin-head-gate/difftest/parsediff.lua`
- `work/pin-head-gate/difftest/compare.py`
- `work/pin-head-gate/difftest/extract_rom.py`
- `work/pin-head-gate/difftest/table.py`
- `work/pin-head-gate/difftest/CobaltParse.java`
- `work/pin-head-gate/difftest/linedump.lua`
- `work/pin-head-gate/difftest/compilebench.lua`
- `work/pin-head-gate/linux-check.sh`
