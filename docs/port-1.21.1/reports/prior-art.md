# Prior-art and external research (lane: prior-art)

Date of research: 2026-11-07. Work dir: `<scratch>\work\prior-art`.
Nothing was written inside `<repo>` (only `git show` / `git log` reads).

## TL;DR

1. **The single most important prior-art fact:** LuaJIT already ships an (undocumented, "unsupported") compile option, `LUAJIT_ENABLE_CHECKHOOK`, that makes JIT-compiled traces check `g->hookmask` and exit to the interpreter when an instruction/line hook is armed. Combined with the "timer method" (keep hooks *off*, arm a count-1 hook asynchronously when the time slice/timeout expires), it lets a host abort **or pause (yield) and later resume** JIT-compiled infinite loops. I verified this here on Windows x64 (MinGW, GC64): without the option 9/10 loop shapes hang forever; with it all 9 Lua-level loop shapes are interrupted within ~0.005–0.033 ms of arming, and 9,000 async pause/resume cycles (yield from the C hook, resume later) ran without a crash. Throughput overhead in micro-benchmarks was within noise. Tarantool enables this option **by default** in its LuaJIT build for exactly this purpose. Caveats: Mike Pall says `lua_sethook()` is *signal-safe but not thread-safe* (calling it from a watchdog thread is technically a data race); C functions (e.g. `string.find` with a pathological pattern) are still uninterruptible.
2. **CraftOS-PC Accelerated** (the only other maintained-ish LuaJIT-based CC runtime) never solved preemption: v2.2.1 (2019) used a count hook of 1 ("~75% of the CPU time"), v2.4.2 (2020) removed it ("count hooks are no longer used"), and its `lua_halt()` is a stub ("halting LuaJIT is much more complex than PUC Lua"). Its CI deletes CC:T's `timeout_spec.lua`, `coroutine_spec.lua` (yield across gsub/sort/hooks/metamethods), `call_spec.lua` (`__name`) and `vm_spec.lua` (unpack 2^19 values). Its LuaJIT fork patches in `utf8`, `string.pack/unpack` (PUC 5.3 code), PUC `bit32` (LuaJIT BitOp is semantically incompatible), `\u{}` escapes, `table.pack/unpack`, `__len`. Accelerated's last release was v2.8.3-luajit (2024-08-17); the branch is effectively dormant. JIT is disabled on Apple Silicon.
3. **Security:** Mike Pall/LuaJIT FAQ: "loading untrusted bytecode is not safe" and the only robust sandbox is process-level. Real escapes exist: malicious bytecode -> native code exec (Peter Cawley/corsix, 2015), Luanti CVE-2026-41196 / CVE-2026-40959 (LuaJIT-only sandbox escape via `getfenv` on builtins, CVSS 10.0/9.3), CVE-2019-19391 (`debug.getinfo` type confusion -> arbitrary write), CVE-2024-25176/25177/25178 (2.1 memory-safety). OpenComputers (native Lua in Minecraft for years) forces `mode="t"` unless `allowBytecode`, disables user `__gc`, and kills `loadstring/dofile/loadfile`.
4. **CC:T maintainer (SquidDev) explicitly rejected native Lua** for CC:T in 2021-2023 because of safety ("Lua bugs can and do crash the game"), JNI native->Java call overhead ("any call into Java ... noticeably slower"), and persistence/suspension; measured native PUC Lua at ~150% faster than Cobalt (2.5x). The `ILuaMachine` interface is internal (core, not API); `ServerContext.luaMachine` is a `@VisibleForTesting public static` field; the interface is unchanged between CC:T 1.115.0 and 1.121.0 except nullability annotations.
5. **LuaJIT in 2026 is healthy:** 125 commits on `v2.1` in the last 12 months (184 since the vendored submodule commit 19878ec0, 2024-11-28), rolling releases, Windows x86/x64/ARM64, Linux x64/ARM64, macOS x64/ARM64 supported, GC64 default on x64 since 2019. v3.0 has a tracking issue but "no public release and no ETA"; in July-Oct 2026 some v3.0 *syntax* extensions (bit operators, `continue`, `const`, `?.`, `??`, compound assignment, `!=`...) were backported into v2.1.
6. **Bindings:** `party.iroiro.luajava` (MIT, Java 8+, v4.1.0 2026-01-03, active) ships prebuilt LuaJIT natives for Win x86/x64/arm64, Linux x86/x64/armhf/arm64, macOS x64/arm64 (9 binaries, 2.9 MB jar), but builds stock LuaJIT (no CHECKHOOK, no 52compat), exposes a full Java-reflection `java` module by default, and wraps the C API one JNI call per `lua_*` call. Useful as a **build-pipeline template**, not as a runtime dependency. FFM (Panama) is final only in Java 22 (JEP 454); MC 1.21.1 runs on Java 21 where it is preview-only -> JNI is the only option. On Java 24+ (MC 26.x needs Java 25) JNI `System.load` prints a warning (JEP 472).
7. **Loader choice for 1.21.1:** NeoForge overwhelmingly. Modrinth CC:T 1.21.1 downloads: NeoForge 1,720,649 vs Fabric 73,358 (23.5:1). CurseForge (CC:T stopped publishing there after 2024-09-11): 1.21.1 NeoForge 4,481,638 vs Fabric 11,297. (On 1.20.1 the split is far more even: Modrinth Forge 1.67M vs Fabric 1.57M.) CC:T 1.121.0 (2026-11-03) is still published for 1.20.1, 1.21.1 and 26.3.

---

## Methodology

* GitHub REST via `gh api` (repos, commits, issues/comments, security advisories, file contents), `gh search`.
* Raw doc pages via `curl` + a small HTML-to-text script (`work/prior-art/h2t.py`, run with `python -I`).
* WebSearch/WebFetch for pages not on GitHub (lua-users.org archive, corsix.org, facepunch wiki, NVD, OpenJDK JEPs).
* Modrinth public API (`api.modrinth.com/v2/project/cc-tweaked/version`) and the `api.cfwidget.com` CurseForge mirror for download counts.
* NVD API (`services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=LuaJIT`).
* Empirical tests (Windows 11, MinGW-w64 gcc 16.2.0, LuaJIT 2.1 ROLLING from the shared prebuilt tree, copied to my work dir):
  ```
  cp -r <scratch>/luajit-win work/prior-art/luajit-checkhook ; cp -r ... work/prior-art/luajit-plain
  (luajit-checkhook/src) mingw32-make clean; mingw32-make -j24 BUILDMODE=static XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK
  (luajit-plain/src)     mingw32-make clean; mingw32-make -j24 BUILDMODE=static
  gcc -O2 -o hooktest-<v>.exe hooktest.c -I luajit-<v>/src luajit-<v>/src/libluajit.a
  gcc -O2 -o hookyield-<v>.exe hookyield.c -I ... ; gcc -O2 -DNARM=3001 -DSLEEPMS=1 -o hookyield-stress.exe hookyield-stress.c ...
  ```

---

## 1. CraftOS-PC Accelerated (MCJack123)

**What it is.** A fork of the CraftOS-PC emulator (`MCJack123/craftos2`, branch `luajit`) that swaps the PUC-Lua-5.2-derived `craftos2-lua` for a LuaJIT fork (`MCJack123/craftos2-luajit`, submodule at commit `4f4466f`, 2024-01-02). Docs claim "over 30x faster than standard CraftOS-PC in some scenarios" (https://craftos-pc.cc/docs/accelerated, source `MCJack123/CraftOS-PC/docs/accelerated.md`); the v2.4.1-luajit release notes claim "8-16x performance improvements depending on the workload" (https://github.com/MCJack123/craftos2/releases/tag/v2.4.1-luajit). Neither number comes with a published methodology.

**Status.** Last Accelerated release `v2.8.3-luajit` 2024-08-17; last commit on `luajit` branch 2024-08-17 (merge of master). Standard CraftOS-PC master continued to 2025-12-13 without new Accelerated merges. Open issue #359 (2024-07) "luajit/Accelerated AppImage libluajit-craftos.so not found". JIT is **disabled on Apple Silicon** both in C++ (`luaJIT_setmode(... LUAJIT_MODE_OFF)`, `src/Computer.cpp` in `luajit` branch, comment "JIT does not work well on Apple Silicon; disable it for performance") and in its ROM (`craftos2-rom/bios.lua:21-23`, "This is also in the C++ source, but for some reason it doesn't work"). Changelog v2.4.2 (2020): "Application is now built as a Universal binary (excluding CraftOS-PC Accelerated, as LuaJIT does not fully support ARM64 yet)".

**Preemption / "Too long without yielding".**
* v2.2.1-luajit (2019-12-29) release notes: Accelerated "works around this bug by setting the hook count to 1, which ends up causing the hook that watches the run time to use up almost 75% of the CPU time"; offered `os.setHaltOnLongRunMode(false)` to regain speed. (https://github.com/MCJack123/craftos2/releases/tag/v2.2.1-luajit)
* v2.4.2 changelog (2020-09-08): "JIT: Removed `os.setHaltOnLongRunMode`, count hooks are no longer used" (`CraftOS-PC/docs/changelog.md` line ~890).
* In the current `luajit` branch the abort-timeout timer (`eventTimeoutEvent`, `lua_externalerror`, `lua_halt` usage present on `master` `src/runtime.cpp:184-223`) is gone; only removals of `eventTimeout` remain (grep of `src/*.cpp`). The fork's `lua_halt` is literally: `// do nothing; halting LuaJIT is much more complex than PUC Lua` (craftos2-luajit commit 5cd001e, `src/lj_api.c`). **So Accelerated has no timeout enforcement at all.** A runaway loop just hangs that computer's OS thread.
* Its CI test patch for CC:T (`resources/CCT-Tests.patch` on branch `luajit`, 451 lines) deletes `spec/lua/timeout_spec.lua` entirely, whereas the standard (PUC) build only drops the string-pattern timeout cases (`master` `resources/CCT-Tests.patch`).

**Compat shims (what broke and how it was fixed)** - from the fork's commits (diffs saved in `work/prior-art/cospc-luajit-patches/*.diff`):
| Area | Fix in craftos2-luajit | Commit |
|---|---|---|
| `utf8` library | ported `lib_utf8.c`, registered `utf8` | de36407, aa5e292 |
| `\u{XXXX}` escapes | lexer patch (contains a bug: `'A'..'F'` uses `c - 'F' + 10`) | de36407 |
| `table.pack`/`table.unpack`, `__len` on tables | un-`#if LJ_52`'d in `lib_table.c`, `lj_meta.c`, `lj_record.c`, `vm_*.dasc` | de36407 |
| `string.pack/unpack/packsize` | PUC 5.3 code pasted into `lib_string.c` | 5cd001e |
| `bit32` | LuaJIT BitOp replaced by PUC 5.2 `bit32` because BitOp rounds via FPU mode while bit32 truncates ("would yield 0, not 1 as expected"); library renamed to `bit32` | 5cd001e |
| Lua 5.2 bits | built with `-DLUAJIT_ENABLE_LUA52COMPAT` (makes `goto` a reserved word) | 4f4466f |
| per-coroutine hooks | `debug.sethook` rewritten to store per-thread hook tables (LuaJIT hooks are global) | eccc061 |
| error hooks | `callhook(L, LUA_HOOKERROR...)` added then partly commented out ("TODO: fix this") | eccc061, ce53437, 639f543 |
| macOS hardened runtime | allocation changes | f714362 |
| `_ENV` | not supportable in LuaJIT; ROM `load` wrapper sets `env._ENV = env` and calls `setfenv` | `craftos2-rom/bios.lua:37-80` |

Note the ROM `load` shim **ignores the `mode` argument** and calls native `loadstring` (`craftos2-rom/bios.lua:44-58`), i.e. Accelerated accepts bytecode - acceptable for a local emulator, not for a server.

**Yields across C boundaries.** The Accelerated test patch deletes CC:T's whole `coroutine_spec.lua` (yield within debug hooks, `string.gsub` callbacks, `table.sort` comparators, `table.foreach(i)`, `xpcall` handlers, metamethods). LuaJIT natively supports yield across `pcall`/`xpcall`, iterators and metamethods ("Fully Resumable VM", https://luajit.org/extensions.html) but not across C library callbacks (gsub/sort) or Lua-level hooks. Standard CraftOS-PC fixed these in its PUC fork in v2.4.2 ("The only function that can't yield from a callback is `load`").

**Other removed tests:** `call_spec.lua` (error messages using `__name`), `vm_spec.lua` (`{ (" "):rep(2^19):byte(1,-1) }` - LuaJIT's result/stack limits), and `textutils.serialise` ordering asserts (LuaJIT 2.1 randomizes hash order per VM; FAQ "Table iteration with pairs() does not result in the same order?").

Key files: https://github.com/MCJack123/craftos2/tree/luajit/src (Computer.cpp, runtime.cpp), https://github.com/MCJack123/craftos2/blob/luajit/resources/CCT-Tests.patch, https://github.com/MCJack123/craftos2-luajit/commits/v2.1 , https://github.com/MCJack123/craftos2-rom/blob/master/bios.lua .

---

## 2. LuaJIT hooks in compiled code

**Official statements.**
* FAQ (https://luajit.org/faq.html#ctrlc): Ctrl-C sets a debug hook, "But this is ignored by compiled code. If your program is running in a tight loop and never falls back to the interpreter, the debug hook never runs".
* Mike Pall, lua-l 2011-02-21 (http://lua-users.org/lists/lua-l/2011-02/msg01106.html): debug hooks are not called from compiled code; "There is an undocumented compile option LUAJIT_ENABLE_CHECKHOOK, which only works under certain constraints"; also "string.find, string.rep and other C functions can be abused and will either run forever or allocate unlimited amounts of memory"; "The only reasonably safe way to handle untrusted Lua scripts is to isolate them in a process context".
* Mike Pall, lua-l 2011-06-13 (http://lua-users.org/lists/lua-l/2011-06/msg00513.html): never use line hooks except for debugging; "set the count hook with a count of 1 from a timer (signal or other thread). And keep it off most of the time"; CHECKHOOK "comes with a speed penalty and you can only use this with the timer method -- the hook must be off most of the time"; hooks in LuaJIT apply to all coroutines at once.
* Source comment, `vendor/luajit/src/lj_record.c:2880-2899` (also upstream): "Regularly check for instruction/line hooks from compiled code and exit to the interpreter if the hooks are set ... disabled by default, since the hook checks may be quite expensive in tight loops ... only useful if hooks are *not* set most of the time ... You can set the instruction hook via lua_sethook() with a count of 1 from a signal handler or another native thread." Implementation: a volatile `IR_XLOAD` of `g->hookmask` + guard at trace start.
* GitHub issues: #723 (2021, Pall: "JIT-compiled code doesn't check for hooks by default ... see the end of src/lj_record.c for a workaround (but it does come at a cost)" and it won't help code stuck in a C function), #779 (2021, Pall: `make XCFLAGS=-DLUAJIT_ENABLE_CHECKHOOK` "(unsupported)", "use LUA_MASKCOUNT -- never use LUA_MASKLINE"), #1120 (2023, Pall points to FAQ), #1488 (2026-07, CHECKHOOK recommended again).
* **Thread-safety caveat** - #824 (2022-03-31, https://github.com/LuaJIT/LuaJIT/issues/824#issuecomment-1084710118): "`lua_sethook()` (only when *setting* a count hook) is *signal-safe*, but *not thread-safe*" - send a signal from the other thread and call `lua_sethook()` in the signal handler. (This supersedes his 2011 "only cross-thread-safe" remark.) #1012 discussion: on Windows `SuspendThread`/`lua_sethook`/`ResumeThread` works; on POSIX use a signal. For a JVM host, signals need care (HotSpot uses several signals; chaining needs `libjsig`, https://docs.oracle.com/en/java/javase/21/vm/signal-chaining.html).
* Hooks are **global per VM** in LuaJIT (Pall, #1230 and #666): `debug.sethook(co, ...)` sets the hook for every coroutine. User code calling `debug.sethook` would clobber the host's watchdog hook unless the host wraps/chains it.
* Recent upstream fix touching this area: `ab834de8` (2026-02-11) "Avoid recording interference due to invocation of VM hooks" (#1429/#1434).

**Other embedders.**
* **Tarantool** turned CHECKHOOK **on by default** after benchmarks showed "neglible platform performance degradation", to stop infinite loops (https://github.com/tarantool/tarantool/issues/7762; `cmake/luajit.cmake: set(LUAJIT_ENABLE_CHECKHOOK ON ...)`; option added in tarantool/luajit commit 701de8c; test `test/app-luatest/gh_7762_luajit_enable_checkhook_test.lua`).
* **Garry's Mod** (LuaJIT): wiki says the hook "is used by default for infinite loop detection" but "Hooks are not always ran when code that has been compiled by LuaJIT's JIT compiler is being executed" (https://wiki.facepunch.com/gmod/debug.sethook); hosts add external watchdogs.
* **OpenResty/ngx_lua**: no built-in way to abort `while true do end`; maintainers suggested a separate OS thread doing `lua_sethook(count=1)` as a last resort (https://groups.google.com/g/openresty-en/c/NsmFB48gl-I).
* **Luau/Roblox** (not LuaJIT, design reference): an `interrupt` callback invoked at safepoints (loop back-edges, calls) - the shape CC:T needs.

**Empirical verification (this lane).** `work/prior-art/hooktest/hooktest.c` - watchdog thread sleeps 300 ms, then calls `lua_sethook(gL, hook, LUA_MASKCOUNT, 1)`; the C hook throws `"Too long without yielding"`; hard limit 5 s. Output (`hooktest-results.txt`):

| # | Loop shape | stock build | CHECKHOOK build (latency) |
|---|---|---|---|
| 0 | `while true do end` | NOT interrupted | interrupted, 0.033 ms |
| 1 | while + arithmetic | NOT | 0.019 ms |
| 2 | numeric for + table stores | NOT | 0.021 ms |
| 3 | for + `math.sin` | NOT | 0.015 ms |
| 4 | nested loops | NOT | 0.012 ms |
| 5 | infinite tail recursion | NOT | 0.020 ms |
| 6 | loop calling Lua function | NOT | 0.032 ms |
| 7 | loop calling `pcall(closure)` (FNEW is NYI -> interpreted) | 0.005 ms | 0.005 ms |
| 8 | `string.find(('a'):rep(3e5), '.-.-.-.-b$')` (C function) | NOT | **NOT** |
| 9 | repeat-until with string concat | NOT | 0.017 ms |

`hookyield.c`: same, but the hook calls `lua_yield(L, 0)` inside a coroutine; main resumes 5 times -> with CHECKHOOK every resume returns `LUA_YIELD` and the loop counter keeps advancing (2.7e8 -> 1.37e9); stock build hangs (exit 124). `hookyield-stress.c` (3,000 arm/yield/resume cycles with `Sleep(1)` between arms - effectively ~1-16 ms on Windows timer granularity - three runs, ~9,000 cycles total): all completed, counter monotonic, no crash. This is a smoke test, not proof of race-freedom. So CC:T's **pause** semantics (`TimeoutState.isPaused`) and **soft abort** are both implementable on JIT code, with the thread-safety caveat above.

Overhead (`bench.lua`, best of 3, two runs each, `bench-results.txt`): tight int loop 0.374-0.380 s (stock) vs 0.373-0.399 s (CHECKHOOK); while loop 0.083-0.088 vs 0.084-0.086; table fill 0.160-0.166 vs 0.168-0.171; string ops 0.119 vs 0.118-0.122. Within noise / <~5%; micro-benchmarks only, not a proper perf study.

---

## 3. LuaJIT sandbox security

* FAQ (https://luajit.org/faq.html): sandboxing is possible only "for an extremely restricted subset of Lua"; "loading untrusted bytecode is not safe! It's trivial to crash the Lua or LuaJIT VM with maliciously crafted bytecode ... there's no bytecode verification on purpose"; mitigation: use the `mode` parameter of `load*()`; "the only promising approach is to sandbox Lua code at the process level".
* **Bytecode -> native code execution**: Peter Cawley (corsix), "Malicious LuaJIT bytecode", 2015-11-11 (https://www.corsix.org/content/malicious-luajit-bytecode): from a sandbox exposing almost nothing but `jit`, crafted bytecode yields memory read and finally native code exec. Precedent in PUC Lua land: Redis `EVAL` bytecode (CVE-2015-4335).
* **Luanti (Minetest)** - the closest analogue (game, untrusted mods, LuaJIT): GHSA-g596-mf82-w8c3 / CVE-2026-41196 (CVSS 10.0, 2026-04): "A malicious mod can trivially escape the sandboxed Lua environment ... only exploitable when using LuaJIT"; workaround `getfenv = nil`; fix 8a929dfb re-sets the fenv of every whitelisted function to the sandbox globals (`src/script/cpp_api/s_security.cpp`). Root cause: LuaJIT builtins carry an environment; `getfenv(builtin)` returns the *original* globals. CVE-2026-40959 (9.3) is also "when LuaJIT is used". Implication: never keep a richer "real" `_G` behind the sandbox; open only the libs you expose.
* Other LuaJIT CVEs (NVD keyword search, 10 hits): CVE-2019-19391 (`debug.getinfo` type confusion -> arbitrary memory write, 9.1), CVE-2020-15890 and CVE-2020-24372 (OOB reads), CVE-2024-25176 (stack buffer overflow in `lj_strfmt_wfnum`, 9.8), CVE-2024-25177 (JIT unsinking DoS), CVE-2024-25178 (OOB read in stack-overflow handler). The vendored 19878ec0 (2024-11-28) already contains the 2024 fixes but is 184 commits behind `v2.1` HEAD, including fixes such as `d2c1327f` "Fix stack overflow relimit handling", `e3fa3c48`/`cd4af8ad` stack-overflow PC fixes, `233ad240` "Fix G->jit_base relocation on stack resize", `e76bb50d` "Fix error generation in load*".
* Vectors with only base/string/table/math/bit/coroutine exposed: (a) `load`/`loadstring` with binary chunks (+ `string.dump` to obtain templates); (b) `getfenv`/`setfenv` on builtins if the host has a different master `_G`; (c) `debug` library if exposed (`getregistry()._LOADED` reveals any lib ever opened, e.g. `ffi`/`io`/`os`; `upvalueid` leaks pointers; CVE-2019-19391); (d) `jit`/`jit.util` (address leaks, used in corsix's exploit) and of course `ffi`; (e) `newproxy`/`__gc` - OpenComputers' `allowGC: false` because "garbage collection callbacks are not sandboxed (hooks are disabled while they run)"; (f) resource DoS in C functions: pathological patterns, `string.rep` (Pall 2011) - verified above that `string.find` is not interruptible even with CHECKHOOK; (g) the JNI glue itself (stack misuse, unchecked indices) and crafted values crossing into Java.
* **OpenComputers' native-Lua hardening** (MightyPirates/OpenComputers `application.conf`, `machine.lua`): `allowBytecode: false` ("MASSIVE SECURITY RISK ... running arbitrary code on the real server"); machine.lua `load = function(ld, source, mode, env) if not system.allowBytecode() then mode = "t" end ...`; `allowGC: false`; `LuaStateFactory.scala` nils `loadstring`, `dofile`, `loadfile`, replaces `math.random`, and keeps `os` as a fresh table.
* CC:T/Cobalt side: since CC:T 1.109.0 "Remove support for dumping and loading binary chunks"; Cobalt throws "attempt to load a binary chunk" (`cobalt-src/org/squiddev/cobalt/compiler/LuaC.java:171`).

---

## 4. LuaJIT project status (2026)

* Status page (https://luajit.org/status.html): "LuaJIT is actively developed and maintained"; v2.1 = production, maintained, non-breaking; v2.0 bugfix-only; next dev branch "TBA". **Rolling releases**, no tarballs; version = `2.1.<commit timestamp>`.
* Platforms (status page): Windows 7+ on x86, x64 or ARM64; Linux; macOS 10.4+ (v2.1); CPUs x86 (SSE2), x64, ARM, ARM64 (little+big), MIPS32/64; RISC-V 64 "(TBA)"; PPC32 EOL. Build docs: "All LuaJIT 64 bit ports use 64 bit GC objects by default (LJ_GC64)"; GC64 default on x64 since commit bd00094c (2019-12-08); "64 bit targets always require compilation on a 64 bit host".
* Activity (GitHub mirror LuaJIT/LuaJIT): HEAD `ea9eb7ed` 2026-10-30; 125 commits on v2.1 since 2025-11-07; 184 since 2024-11-28 (149 non-merge, 61 titled "Fix"). Single maintainer (Mike Pall) plus regular contributors (Sergey Kaplun/Tarantool, Peter Cawley...). 63 open issues.
* v3.0: tracking issue #1092 (2023-09-21): targets x64 (x86-64-v3, AVX2/BMI2), ARM64 (v8.0+Crypto), RV64 (RVA23); self-hosted VM in Lua; GC work incl. "Public API to set allocator limits"; "LuaJIT v2.1 is actively maintained and *it's not going away*". #1476 (2026-06-22): "There is no public release and no ETA, at this time. Please don't ask."
* v3.0 syntax backports **merged into v2.1** (`a2ce8114` 2026-07-20, `0218af7e`, `c41d727d` 2026-10): bit operators `~ & | << >> ~>>`, `! && || !=`, `?:`, `?.`, `??`, compound assignment `+= ... ..=`, `continue` (soft keyword), `const`, short function expressions, `_` in numbers, empty statements, line directive (https://luajit.org/extensions.html, issue #1475). Bytecode using bit ops needs LuaJIT >= 2.1.1784535649. Implication: a bumped LuaJIT accepts non-Lua-5.2 syntax that Cobalt rejects - programs written for CCLuaJIT could silently become non-portable (a parser flag to disable them would be desirable; none exists upstream as far as found).
* Memory: with GC64, `lua_newstate(allocf, ud)` is available (`vendor/luajit/src/lj_state.c:239-243`; only non-GC64 x64 forces `luaL_newstate`), so a per-computer accounting/limiting allocator is possible today; an official limit API is a v3.0 item.
* macOS: `LUAJIT_ENABLE_OSX_HRT` build option (commit 4f2bb199, 2025-03-10, issue #1334) for MAP_JIT under Apple hardened runtime; OpenJDK's `java` launcher is signed with `allow-jit`, `allow-unsigned-executable-memory` and `disable-library-validation` (openjdk/jdk21u `make/data/macosxsigning/java.plist`), so a stock-built LuaJIT dylib should load and JIT inside a standard JDK on macOS (not verified on hardware; Mojang's bundled runtime not checked). Recent macOS-specific fixes: `716f74a6` (2026-10, mcode vs xzone malloc collision, #1520).

---

## 5. Java <-> LuaJIT bindings

**party.iroiro.luajava** (https://github.com/gudzpoz/luajava, docs https://luajava.iroiro.party/):
* License MIT (example dir GPL-3); Java 8 baseline; latest release v4.1.0 (2026-01-03), commits through 2026-08-16; 261 stars, 17 open issues.
* Supports Lua 5.1-5.5, LuaJIT (git submodule `LuaJIT/LuaJIT` v2.1; README pins `1edc3e5`, 2026-08-03; the v4.1.0 DLL reports `LuaJIT 2.1.1765228720` = 2025-12-08) and LuaJ.
* Prebuilt LuaJIT natives (docs matrix: "tested" for Linux x64/ARM64, Windows x64, macOS x64/ARM64, Android; "available" Linux x86/ARM, Windows x86, iOS). Downloaded `luajit-platform-4.1.0-natives-desktop.jar` (2.9 MB) contains: `libluajit.so, libluajit64.so, libluajitarm.so, libluajitarm64.so, luajit.dll, luajit64.dll, luajitarm64.dll, libluajit64.dylib, libluajitarm64.dylib`. glibc floor for linux64: GLIBC_2.11 (docs/glibc.md).
* Build: `luajit/build.gradle` uses `make amalg` with cross toolchains from Linux (`x86_64-w64-mingw32-`, `aarch64-w64-mingw32-`, `aarch64-linux-gnu-`, `arm-linux-gnueabihf-`, `i686-...`) and `TARGET_FLAGS=-arch x86_64|arm64` on macOS; **no XCFLAGS** -> stock LuaJIT (no CHECKHOOK, no 52compat). Using a patched LuaJIT means forking the repo and rebuilding all natives (feasible; CI `build-natives.yml` exists).
* API: thin JNI wrapper, roughly one JNI call per C-API call ("every argument requires one JNI call"), plus a `java` module (import/new/proxy = full reflection) loaded **by default** ("By default, the `Lua` state is only loaded with the `java` library", docs/java.md). Must be removed for a sandbox.
* Perf (docs/perf.md, JMH, JDK 17 Linux): pure `getGlobal+pcall` 63 ns (LuaJIT) vs ~22 ns in C ("JNI costs are mostly 2x ~ 3x pure C"); Lua->Java object method call ~810 ns; "LuaJIT is not specifically faster in use cases where JNI calls are used heavily."
* Verdict: good reference for cross-platform native build/packaging and loader code; poor fit as the runtime layer for CC:T (needs custom C glue for argument marshalling, hooks, timeouts, memory accounting anyway).

**JNLua**: original (code.google.com/p/jnlua, mirror airminer/jnlua, last commit 2016). OpenComputers maintains **OC-JNLua** (https://github.com/MightyPirates/OC-JNLua, last commit 2023-05) for PUC Lua 5.2-5.4 + Eris; natives for darwin aarch64/x86_64, freebsd x86_64, linux aarch64/armhf/i686/x86_64, windows i686/x86_64, all **cross-compiled from one Linux host with Zig + mingw-w64 via Meson cross files** (`native/build-all.sh`, `native/cross/*.ini`). `hyee/JNLuaJIT` (2 stars) is a JNLua-for-LuaJIT fork - not production-grade.

**Java FFM (Panama)**: final in JDK 22 (JEP 454, https://openjdk.org/jeps/454); in JDK 21 it is the third preview (JEP 442, https://openjdk.org/jeps/442) needing `--enable-preview` -> unusable for a MC 1.21.1 mod. MC 26.1+ requires Java 25 (https://minecraft.wiki/w/Java_Edition_26.1-snapshot-1), where FFM is available - SquidDev measured Panama ~50% faster than JNI for native->Java calls (CC:T #769, 2022-12-17). JEP 472 (JDK 24, https://openjdk.org/jeps/472): JNI `System.load` without `--enable-native-access` emits a warning (future: exception) - irrelevant on Java 21, relevant for a later 26.x port.

---

## 6. CC:Tweaked maintainers' views, Cobalt, extension point

* **CC:T #769 "Investigate alternative Lua VMs"** (SquidDev, 2021-04 .. closed `not_planned` 2023-10-04, https://github.com/cc-tweaked/CC-Tweaked/issues/769). Requirements (2021-06-06): 1 Safe ("without the risk of sandbox escapes, for a VM *with the debug library enabled*"), 2 memory limits, 3 "Fast. Or at least no worse than Cobalt", 4 Maintainable; "Native Lua satisfies 2, 3, and 4 (not 1, because Lua bugs can and do crash the game)". 2022-12-17 braindump: safety "rules out PUC Lua. It's fairly easy to cause a segfault when you've access to the `debug` API"; wasmtime ~25% faster than Cobalt, wasm2c ~80%, "native Lua is 150% faster than Cobalt"; "The big problem with any native approach is that you have to use the JNI ... native -> Java calls have a lot of overhead ... any call into Java (so the `term` API, peripherals) are noticeably slower"; Panama ~50% faster; `speaker.playAudio` (130k numbers from a table) needs batch APIs; persistence/suspension at arbitrary points is hard ("make it possible to yield from any C function, which gets really hard for something like `load`"); `__gc` suspension "basically impossible". Final: "I think this is looking very unlikely at this point."
* Lua version policy: CC:T is Lua 5.2 (1.109.0) with selected 5.3/5.4/5.5 library backports; SquidDev declined Lua 5.3 integers/bitwise operators (#950, closed 2025-02-14: "Lua 5.3's implementation of integers has really soured on me") and 5.4 `<close>` (#2228).
* **Cobalt** (https://codeberg.org/SquidDev/Cobalt; GitHub mirror archived): "Allows yielding *anywhere* within a Lua program, including debug hooks and inside any native function", "Support for interrupting and resuming the VM at arbitrary points", backports 5.3/5.4 stdlib, `goto` as identifier, string ropes; "There is no guarantee of API stability between versions". CC:T 1.82.0 introduced preemptive interruption of computers; 1.104.0 Cobalt 0.7 ("Coroutines no longer run on multiple threads", "Timeout errors should be thrown more reliably"); 1.109.0 Lua 5.2 + memory *tracking*; 1.109.4 Cobalt 0.9; 1.119.0 Cobalt 0.9.9 (some 5.5 changes). No published Cobalt benchmark history was found; Cobalt #81 (2023): "Our benchmarking suite right now is incredibly unreliable". Waluaigi README numbers (unrepresentative): PUC 5174 / Cobalt 570 / Waluaigi 50 iterations (https://github.com/SquidDev/waluaigi). Cobalt #66 memory limits via sampling - SquidDev concluded sampling is not adversary-safe and chose monitoring only.
* **Features that make alternative runtimes hard** (from the above + CC:T source): preemptive pause/resume (`TimeoutState` 7 s soft abort, +1.5 s hard abort, `isPaused` time slicing at 50 ms latency / 5 ms min period, `ComputerThread` CFS-like scheduler); hard abort abandons the worker thread (`ComputerThread.java:530-546`) - a native thread stuck in JIT code would keep burning a core forever; yield-anywhere; 5.2 `_ENV` (18 references in the 1.21.1 ROM, e.g. `bios.lua:461-463` sets `tEnv._ENV = tEnv`); `__name` in errors; `debug` library exposure; string ropes; pattern-matching timeouts (`timeout_spec.lua`).
* **Extension point**: `ILuaMachine` lives in `dan200.computercraft.core.lua` (core, not core-api). Its Javadoc: "There should only be one concrete implementation at any one time ... If external mod authors are interested in registering their own machines, we can look into how we can provide some mechanism". `ServerContext.java:54-55` (1.121.0): `@VisibleForTesting public static ILuaMachine.Factory luaMachine = CobaltLuaMachine::new;` read at `ServerContext` construction (`.luaFactory(luaMachine)`, line 77). No stability guarantee; but `ILuaMachine` and `MachineEnvironment(context, metrics, timeout, apis, luaMethods, hostString)` are identical between 1.115.0 and 1.121.0 except `@Nullable` annotations - the integration surface is small and has been stable. CC:T's core is MC-version-independent: 1.121.0 ships simultaneously for 1.20.1, 1.21.1 and 26.3.
* No CC:T issue or discussion specifically about LuaJIT was found (searched "LuaJIT", "native lua", "ILuaMachine", "lua machine").

---

## 7. The original CCLuaJIT and analogues

* `vereena0x13/CCLuaJIT` (https://github.com/vereena0x13/CCLuaJIT): MIT, created 2024-04-25 by re-uploading older code ("Copy old files; needs fixed." on every branch), 1 star, 1 fork (the user's), 0 issues/PRs ever, no releases, README has no benchmarks or status. Branch histories: `1.7.10_CC`, `1.12.2_CC` = only "copy old files" commits; `1.14.4_CCT` "Update things to actually work." (2024-04-25); `1.20.1_CCT` "Begin porting..." / "cooking?" (2025-02-16) and last commit "Hmmmh. Yeah, we're probably in for a fair bit of pain here, huh..." (2025-03-14). The pre-2024 history/home of the project was not found (searches for older copies, forum posts and code fragments returned nothing). The author's other repos moved on to FPGA/CPU projects. Stall reason (inference, medium confidence): the 1.20.1 port hit the CC:T 1.9x+ architecture (Cobalt-specific `ILuaMachine` contract, preemption, Lua 5.2) and was abandoned mid-port.
* Fork `mindbound/CCLuaJIT` created 2026-11-07, no new commits.
* **OpenComputers** (https://github.com/MightyPirates/OpenComputers, active 2026): native Lua 5.2/5.3/5.4 via OC-JNLua + Eris persistence; per-platform natives extracted to `opencomputers/natives` or tmp with byte-compare and stale cleanup (`LuaStateFactory.scala`); LuaJ fallback when no native for the platform (`forceLuaJ`, "Unsupported platform, you won't be able to host games with persistent computers"); per-computer memory limit via a custom allocator (`limitMemory`); timeout via a count hook calibrated with a "bogomips" loop in `machine.lua` (`debug.sethook(co, checkDeadline, "", hookInterval)`, default `timeout: 5.0`); bytecode and `__gc` disabled by default. Lessons: ship natives for all mainstream OS/arch pairs, provide a pure-Java fallback, version-stamp extracted libraries, make dangerous features opt-in, and accept that native Lua makes platform support a permanent maintenance cost.
* Other VM swaps: SquidDev's own CCTweaks (archived 2017) offered Cobalt, LuaJC (Lua->JVM bytecode) and a Rembulan integration; CC:T absorbed Cobalt. No LuaJIT-based CC mod exists on Modrinth (search "luajit", "lua jit": 0 hits).

---

## 8. Ecosystem: loader choice for 1.21.1

Modrinth API (fetched 2026-11-07; `work/prior-art/web/mr-versions.json`): CC:T project total 5,363,058 downloads; loaders fabric/forge/neoforge.

| MC version | Fabric | NeoForge / Forge |
|---|---|---|
| 1.20.1 | 1,567,311 | Forge 1,667,744 |
| 1.21.1 | **73,358** | **NeoForge 1,720,649** |
| 1.21.x later (1.21.7/8/11) | 26,087 | 8,457 |
| 26.1.2 / 26.2 / 26.3 | 7,658 / 15,852 / 5,055 | 5,687 / 5,028 / 2,410 |

CurseForge (via api.cfwidget.com; `cf-widget.json`): project page says "CC: Tweaked is no longer published to CurseForge. Please download from Modrinth instead"; last CF files 2024-09-11. 1.21.1 CF files: NeoForge 4,481,638 vs Fabric 11,297; 1.20.1 CF: Forge 12.44M vs Fabric 4.77M; CF all-time 80.8M.
Note the NeoForge 1.21.1 jar is named `cc-tweaked-1.21.1-forge-<ver>.jar`. Counts include modpack installs (big NeoForge 1.21.1 packs inflate the NeoForge share), but the gap (23x on Modrinth, ~400x on CF) is decisive. The Fabric build needs Fabric API.

---

## Implications for this project

1. **Preemption is solvable, and the solution is known prior art**: build LuaJIT with `-DLUAJIT_ENABLE_CHECKHOOK`, keep hooks off in normal running, and when CC:T's `TimeoutState` says pause/soft-abort, arm `lua_sethook(L, h, LUA_MASKCOUNT, 1)`; the C hook either `lua_yield`s (pause, resumed next slice) or raises "Too long without yielding". Verified working here on Windows x64. Make the arming thread-safe: either (a) a tiny LuaJIT patch so the CHECKHOOK guard also tests a host-owned volatile "interrupt requested" byte that the watchdog may set with a plain atomic store, and the trace-exit/interpreter path then calls `lua_sethook` *on the Lua thread*; or (b) OS-level suspension (Windows `SuspendThread`) / a dedicated signal (POSIX) around `lua_sethook`. Option (a) avoids JVM signal interplay and is the recommended direction. Also wrap/chain user `debug.sethook` (LuaJIT hooks are global) and add interrupt polling inside `lib_string` pattern matching (`string.find/match/gmatch/gsub`) - CHECKHOOK does not cover C functions (verified).
2. **Do not re-learn CraftOS-PC's lessons**: count-hook-of-1 costs ~75% CPU; no timeout is unacceptable on a server; LuaJIT BitOp != `bit32` (verified: `bit.bnot(0) = -1`, `bit.band(2.7,255) = 3`, unsigned/truncating semantics differ) - the current 1.20.1 code aliases `bit32 = bit`, which is wrong; port PUC `bit32`, `utf8`, `string.pack` as Accelerated did, and implement `_ENV` via `env._ENV = env` + `setfenv`. Expect CC:T test specs for coroutines-across-C-callbacks, `__name`, huge unpack and serialise-ordering to fail unless specifically addressed; use CC:T's own test-rom (as Accelerated does) as the compatibility gate.
3. **Security baseline before any release**: force text-only loading (`load` mode "t" at the C level, drop `string.dump`), never open `ffi`/`jit`/`io`/`os`/`package`, decide carefully about `debug` (CC:T exposes it; LuaJIT's debug lib has had memory-safety CVEs), make sure every exposed function's fenv is the computer's globals (Luanti CVE-2026-41196), no `newproxy`/`__gc`, per-computer allocator with a byte limit (`lua_newstate` works with GC64), stay current with LuaJIT v2.1 HEAD (vendored copy is 184 commits behind). Document clearly that a native VM is a server-crash/compromise risk (this is exactly why CC:T's maintainer rejected native Lua); make it opt-in/server-configurable.
4. **Performance expectations**: big wins only for pure-Lua compute; JNI native->Java calls (term, peripherals, `speaker.playAudio`) are the bottleneck SquidDev measured - design batch/bulk marshalling in C, avoid per-value JNI round trips. Third-party claims: 8-16x / "over 30x" (CraftOS-PC Accelerated vs PUC-based emulator), 2.5x for native PUC vs Cobalt (SquidDev). These need re-measurement in-game.
5. **Bindings**: write a purpose-built JNI layer (as CCLuaJIT does); borrow LuaJava's / OC-JNLua's cross-compile and native-extraction approach (mingw-w64 and Zig/clang cross files; macOS built on macOS runners) for Windows x64/arm64, Linux x64/arm64, macOS x64/arm64. FFM is not an option on Java 21 (1.21.1).
6. **Integration point**: replace `ServerContext.luaMachine` (public static, `@VisibleForTesting`, internal API) from the mod initializer; it has been stable across 1.115->1.121 but carries no guarantee - pin the CC:T version range, keep a pure-Cobalt fallback (as OC does with LuaJ) when natives fail to load, and consider asking upstream for a registration hook (the Javadoc explicitly invites this).
7. **Loader**: target NeoForge for 1.21.1 (NeoForge is ~95.9% of CC:T 1.21.1 downloads on Modrinth and ~99.7% on CurseForge). Since the Lua-machine core is loader-independent, a common module + thin NeoForge (and optionally Fabric) entrypoints is cheap if multi-loader is desired later.
8. **LuaJIT version pinning**: new v3.0-syntax backports in v2.1 (since 2026-07) make LuaJIT accept `continue`, `?.`, `+=` etc. that CC:T rejects - pin a known commit and consider a small parser patch/flag to keep CC:T-compatible syntax.

## Artifacts produced

* `work/prior-art/hooktest/hooktest.c`, `hooktest-results.txt` - async hook interrupt test (stock vs CHECKHOOK).
* `work/prior-art/hooktest/hookyield.c`, `hookyield-results.txt`, `hookyield-stress.c`, `hookyield-stress-results.txt` - pause/resume via hook yield.
* `work/prior-art/hooktest/bench.lua`, `bench-results.txt` - CHECKHOOK overhead micro-benchmarks.
* `work/prior-art/luajit-checkhook/`, `work/prior-art/luajit-plain/` - static LuaJIT builds (build logs `build-checkhook.log`, `build-plain.log`).
* `work/prior-art/cospc-luajit-patches/*.diff` - CraftOS-PC LuaJIT fork commit diffs; `cospc-luajit-tree/` (craftos2 `luajit` branch tarball), `master-CCT-Tests.patch`, `cospc-site/` (CraftOS-PC docs), `cospc-src/`.
* `work/prior-art/web/` - fetched pages/data: LuaJIT docs (faq/status/extensions/install), lua-l posts, LuaJIT 3.0 syntax doc (`lj-1475-doc.md`), `luajit-v21-commits-since-2024-11-28.txt`, Tarantool `fiber.c`, OC `machine.lua` / `LuaStateFactory.scala`, LuaJava docs, `craftos2-rom-bios.lua`, `nvd-luajit.json`, Modrinth/CurseForge JSON.
* `work/prior-art/dl-luajava/luajit-platform-4.1.0-natives-desktop.jar` (+ extracted `x/luajit64.dll`).

## Source URLs (primary)

* CraftOS-PC: https://craftos-pc.cc/docs/accelerated ; https://github.com/MCJack123/craftos2/tree/luajit ; https://github.com/MCJack123/craftos2/blob/luajit/resources/CCT-Tests.patch ; https://github.com/MCJack123/craftos2-luajit ; https://github.com/MCJack123/craftos2/releases/tag/v2.2.1-luajit ; https://github.com/MCJack123/craftos2/releases/tag/v2.4.2-luajit ; https://github.com/MCJack123/craftos2/releases/tag/v2.4.1-luajit ; https://github.com/MCJack123/CraftOS-PC/blob/master/docs/changelog.md ; https://github.com/MCJack123/craftos2-rom/blob/master/bios.lua
* LuaJIT: https://luajit.org/faq.html ; https://luajit.org/status.html ; https://luajit.org/extensions.html ; https://luajit.org/install.html ; https://github.com/LuaJIT/LuaJIT/issues/723 ; /779 ; /824 ; /1012 ; /1092 ; /1120 ; /1230 ; /1334 ; /1475 ; /1476 ; /1488 ; http://lua-users.org/lists/lua-l/2011-02/msg01106.html ; http://lua-users.org/lists/lua-l/2011-06/msg00513.html
* Embedders: https://github.com/tarantool/tarantool/issues/7762 ; https://github.com/tarantool/luajit/commit/701de8c ; https://wiki.facepunch.com/gmod/debug.sethook ; https://groups.google.com/g/openresty-en/c/NsmFB48gl-I
* Security: https://www.corsix.org/content/malicious-luajit-bytecode ; https://github.com/luanti-org/luanti/security/advisories/GHSA-g596-mf82-w8c3 ; https://github.com/luanti-org/luanti/commit/8a929dfb97aa08337f49ba1bb96a56d6557dc896 ; https://nvd.nist.gov/vuln/detail/CVE-2024-25176 (and -25177, -25178, CVE-2019-19391, CVE-2026-40959, CVE-2026-41196)
* Bindings/Java: https://github.com/gudzpoz/luajava ; https://luajava.iroiro.party/ ; https://repo1.maven.org/maven2/party/iroiro/luajava/luajit-platform/4.1.0/ ; https://github.com/MightyPirates/OC-JNLua ; https://openjdk.org/jeps/454 ; https://openjdk.org/jeps/442 ; https://openjdk.org/jeps/472 ; https://docs.oracle.com/en/java/javase/21/vm/signal-chaining.html ; https://github.com/openjdk/jdk21u/blob/master/make/data/macosxsigning/java.plist ; https://minecraft.wiki/w/Java_Edition_26.1-snapshot-1
* CC:T/Cobalt: https://github.com/cc-tweaked/CC-Tweaked/issues/769 ; /768 ; /950 ; /2228 ; https://codeberg.org/SquidDev/Cobalt ; https://github.com/SquidDev/Cobalt/issues/66 ; /81 ; https://github.com/SquidDev/waluaigi
* Original mod and analogues: https://github.com/vereena0x13/CCLuaJIT ; https://github.com/mindbound/CCLuaJIT ; https://github.com/MightyPirates/OpenComputers ; https://github.com/SquidDev-CC/CCTweaks
* Ecosystem: https://modrinth.com/mod/cc-tweaked (API https://api.modrinth.com/v2/project/cc-tweaked/version) ; https://www.curseforge.com/minecraft/mc-mods/cc-tweaked (via https://api.cfwidget.com/minecraft/mc-mods/cc-tweaked)
