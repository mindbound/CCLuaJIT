# How CC programs detect _VERSION, _HOST and LuaJIT

*Written on 2026-11-07 by an investigation agent (decision 5/6 in PLAN.md). Reusable probes and results were copied to `../artifacts/harnesses/version-survey/`. Paths under `scratchpad/work/...` refer to the session scratch workspace and no longer exist.*

## Summary

Recommendation: keep `_VERSION = "Lua 5.2"` (option a), expose a small `jit` table holding only `version` and `version_num` (option c), and add a separate CCLuaJIT tag in brackets at the end of `_HOST` (option d). Also make LuaJIT's `bit` library loadable with require, but not as a global. Do not change `_VERSION` (option b), and leave a dedicated API (option e) for later.

The evidence:
- **CC:T itself:** `_VERSION` comes from Cobalt (BaseLib.java:66). `_HOST` is built once per machine (ServerContext.java:233 → MachineEnvironment.hostString → CobaltLuaMachine.java:82). Nothing in CC:T's Java or ROM parses either value: `about.lua` only prints `_HOST`, and the test-rom never mentions `_VERSION`, `_HOST` or `jit`.
- **CraftOS-PC:** Accelerated reports `"Lua 5.1"`, `_HOST` "(CraftOS-PC Accelerated v2.8.3-luajit)" and the full LuaJIT `jit` library (source only; Accelerated was not run). Standard CraftOS-PC v2.8.3 was run and reports `"Lua 5.2"`, "(CraftOS-PC v2.8.3)" and no `jit`.
- **How CC programs detect LuaJIT:** in a sample of 897 CC files from code search, 29 files in 18 repos test `if jit`. None check `_VERSION`, and one checks `_HOST:find("luajit")`. The main users are AUKit, sanjuuni's video player and several of MCJack123's demos. They use `jit` to switch to chunks of 7680–8000 values, because LuaJIT returns fewer results.
- **That limit is real on the patched build:** at most 8000 results from `string.byte` and 7999 from `unpack`; Cobalt handles 524288. So without a visible `jit`, AUKit and the video player take code paths that fail on CCLuaJIT.
- **Why not change `_VERSION`:** every distinct-but-compatible string I tried changes at least one real check: `== "Lua 5.2"` (sha2, Phoenix), `:sub(-3)` (compat53 gets "IT)" and decides it is on 5.3+), `tonumber(_VERSION:sub(5))` (gives nil) and `'^Lua 5%.[12]$'`.
- **Why the `_HOST` tag is safe:** I evaluated 20 real `_HOST` parsers. A separate bracketed tag at the end changes only what UnBIOS's existing " (UnBIOS)" suffix already changes. Putting the tag inside the Minecraft brackets breaks obsi2's parser.
- **sha2.lua breaks under every option:** it detects LuaJIT by a VM quirk (`({false,[1]=true})[1]` is true on LuaJIT, false on Cobalt), not by `jit`. It then calls `require"bit"` with no fallback. CC code that ships it includes the CCSecureBoot mod's ROM and at least 4 repos with CC code; one of them patched the detection out by hand. Making `bit` loadable with require fixes it.

Gaps: running the downloaded libraries on the patched LuaJIT was blocked by the permission classifier. The library verdicts for LuaJIT therefore come from evaluating each library's detection line (quoted, not the library itself), not from running the libraries. Only the stock-Cobalt run of the libraries happened, before the block.

## Recommendation

Use a combination: (a) + (d) + (c), plus `bit` loadable with require.

1. **(a) Keep `_VERSION = "Lua 5.2"` exactly.**
   - Every distinct-but-compatible string I tried changes at least one real check (sha2, Phoenix, compat53, `tonumber(_VERSION:sub(5))`, anchored matches).
   - No CC program looks for LuaJIT in `_VERSION`.
   - Cost: none.

2. **(d) Append a separate bracketed tag to `_HOST`, only inside the LuaJIT machine.**
   - Build it from `MachineEnvironment.hostString()`, so Cobalt-fallback, quarantined or disabled computers keep the stock string. Example: `"ComputerCraft 1.121.0 (Minecraft 1.21.1) (CCLuaJIT 0.1.0; LuaJIT 2.1.1783773675)"`.
   - Optionally add "; JIT off" when the config disables the JIT.
   - Never change the "ComputerCraft <ver>" prefix or the "(Minecraft …)" group, and never put the tag inside that group.
   - Cost: the same small breakage UnBIOS's " (UnBIOS)" suffix already causes (Inksaver clsTurtle's minor version becomes nil), plus cosmetic display changes.
   - This is the identifier for humans, logs and bug reports.

3. **(c) Expose `jit = { version = <real jit.version>, version_num = <real jit.version_num> }`.**
   - Make it a fresh plain table created per state before bios.lua runs, and put the same table in the fake registry's `_LOADED.jit`. Leave out os, arch and every function; the real jit library stays removed from `_G`, `_LOADED` and `_PRELOAD`.
   - Use a plain table rather than a read-only proxy: each computer has its own state, and a proxy breaks `pairs` and `rawget`.
   - This is the identifier for programs. It is exactly what CC programs already test (`if jit`), and it puts AUKit and sanjuuni on code paths that respect LuaJIT's measured 8000-result limit.
   - Costs:
     - AUKit uses more CPU (larger sinc window).
     - LevelOS shows its JIT alert.
     - Phoenix's preemptive mode refuses to start, with a clear message (it could not work anyway without `debug.sethook`).
     - General Lua libraries that call `jit.off()`, `jit.status()` or `require "ffi"` after an `if jit` test will error. None were found in CC code. If that turns out to matter, add no-op `on`/`off`/`flush`, but `status()` would then have to misreport, so I would not add it up front.
   - DESIGN §3.5 needs a one-line change.

4. **Independently of identity: register LuaJIT's BitOp as `_LOADED.bit`** (keep the CC stub as the global `bit`).
   - sha2.lua detects LuaJIT through a VM quirk under every option and calls `require"bit"`, so today it fails on CCLuaJIT. It ships in CCSecureBoot's ROM and in at least 4 repos with CC code.
   - `bit` also satisfies the common `jit and require "bit"` idiom, and kikito's md5.lua will use it.
   - Risk: low (pure functions). On stock CC:T `require "bit"` errors, so no stock program relies on that.
   - Verify with a real sha2 run on the patched build. I could not run it because the permission classifier blocked running third-party code.

5. **(b) Do not adopt; (e) defer.** If structured data is ever needed (CCLuaJIT version, LuaJIT pin, JIT on/off, result limits), expose it as a `require "ccluajit"` module in the registry `_LOADED` rather than as a new global.

How programs and humans tell the difference:
- **Programs:** `if jit then` (LuaJIT), and `_HOST:find("(CCLuaJIT", 1, true)` (specifically CCLuaJIT).
- **Version numbers:** `jit.version` / `jit.version_num` for LuaJIT; the `_HOST` tag for the CCLuaJIT version.
- **Humans:** the `about` program, and the startup log banner from DESIGN §5.
- Stock-CC:T code is unaffected because `_VERSION` stays identical and the `_HOST` change has the same shape as one the ecosystem already copes with.

## Details

W = <scratch>\work\version-survey (all work here; nothing written to <repo>).

## 0. Build used for the probes
- `git clone --reference <repo>/vendor/luajit --dissociate https://github.com/LuaJIT/LuaJIT W/luajit-src` (HEAD ea9eb7ed 2026-10-30; pin 3c4f9fe2 = 2026-07-11).
- `LUAJIT_REV=3c4f9fe2 bash .../artifacts/luajit-patches/build-patched-luajit.sh W/luajit-src W/luajit-cc` → exit=0 (W/build-luajit-cc.log).
- `W/luajit-cc/src/luajit.exe -e 'print(_VERSION, jit.version, jit.version_num, jit.arch, jit.os, bit and "bit", ffi, ({false,[1]=true})[1])'` → `Lua 5.1  LuaJIT 2.1.1783773675  20199  x64  Windows  bit  nil  true`.
- Stock VM reference: Cobalt 0.9.9, the version CC:T 1.121.0 pins in gradle/libs.versions.toml:29 (jar from the Gradle cache). It runs under W/cobalt-runner/CobaltRun.java, copied from the rom-compat lane, with `_HOST` set to the stock string.

## 1. CC:T 1.121.0 (core jar ROM + git tag v1.21.1-1.121.0 @380c6fc, sparse clone W/cct-src)
- ROM extraction: `python -I W/scripts/extract_rom.py <core jar> W/rom-1.121.0` gave 222 files (105 .lua).
- grep findings in the ROM:
  - `_VERSION` appears only in rom/help/changelog.md.
  - `_HOST` appears in rom/programs/about.lua:6 (`print(os.version() .. " on " .. _HOST)`) and the changelog.
  - The word `jit` and "luajit" appear nowhere.
  - bios.lua:21-30 injects the CC `bit` stub (bnot/band/bor/bxor/brshift/blshift/blogic_rshift, with no lshift or tobit).
  - rom/modules/main/cc/require.lua:125-141 copies `debug.getregistry()._LOADED` into each program's `package.loaded`. Whatever CCLuaJIT's fake registry `_LOADED` holds is therefore what `require` finds.
- test-rom (88 .lua): no `_VERSION`, `_HOST` or `jit` at all.
- How CC:T's Java builds `_HOST`:
  - ServerContext.java:232-234: `String.format("ComputerCraft %s (Minecraft %s)", ...)`.
  - GlobalEnvironment.java:17-22 documents the form "ComputerCraft $CC_VERSION ($HOST)".
  - ComputerExecutor.java:374 passes the string into `MachineEnvironment(..., String hostString)` (MachineEnvironment.java:24,27-34).
  - CobaltLuaMachine.java:82 does `globals.rawset("_HOST", ...)`.
  - Other hosts: tweaked.cc builds "ComputerCraft <v> (tweaked.cc)" (projects/web EmulatorEnvironment.java:31); the test fixture builds "ComputerCraft 1.0 (Test environment)" (BasicEnvironment.java:64).
  - Nothing parses it; getUserAgent is separate.
  - Because the string arrives per machine, a LuaJIT machine can append to it while Cobalt-fallback computers keep the stock string.
- `_VERSION`: Cobalt 0.9.9 BaseLib.java:66 `env.rawset("_VERSION", valueOf("Lua 5.2"))`.
- Cobalt registry `_LOADED` (LibFunction.setGlobalLibrary) holds bit32, coroutine, debug, math, string, table and utf8. So on stock CC:T, `require "bit"`, `require "jit"` and `require "ffi"` all fail (seen in the Cobalt PROBE line, W/harness/out-cobalt.txt).
- Docs: doc/stub/global.lua:117-131 says `_HOST` holds the emulator's version outside Minecraft and recommends feature detection over version comparison.

## 2. CraftOS-PC
- Standard v2.8.3 (installed copy), run as `CraftOS-PC_console.exe -d W/cospc-run/data --headless --exec ...`. Result in W/cospc-run/headless-identity.txt:
  - `_VERSION` = "Lua 5.2"
  - `_HOST` = "ComputerCraft 1.112.0 (CraftOS-PC v2.8.3)"
  - jit = nil, bit = table, bit32 = table, ffi = nil
  - os.version() = "CraftOS 1.9"
- Source (clones in W/cospc):
  - craftos2 master @2844cba6 (2025-12-13); craftos2 luajit @aaeec7dd (2024-08-17, submodule craftos2-luajit @4f4466f).
  - craftos2-luajit v2.1 @ffce6c92 (2026-06-09; new commits since prior-art.md: aef3104 "Fixed utf8.char", ffce6c9 "Enable Lua 5.2 compat for Windows").
  - craftos2-rom @e6f63a1b.
- `_HOST` format:
  - master Computer.cpp:625-629: "ComputerCraft " CC_VER " (CraftOS-PC " VER ")".
  - luajit Computer.cpp:435-439: "... (CraftOS-PC Accelerated " VER ")". With util.hpp:29-30 that gives "ComputerCraft 1.112.0 (CraftOS-PC Accelerated v2.8.3-luajit)".
- Accelerated `_VERSION` = "Lua 5.1": the fork's src/lua.h:19 LUA_VERSION; luajit.h:33 says "LuaJIT 2.1.0-beta3".
- `jit`, `bit` and `ffi` in Accelerated:
  - `jit` is opened with luaopen_jit (luajit Computer.cpp:213-222). It is a global and is in `_LOADED`, and it is never removed, so programs see the full library, including jit.util.
  - The library named `bit32` (fork lualib.h:22, LUA_BITLIBNAME="bit32") is PUC bit32 (commit 5cd001e). There is no LuaJIT BitOp.
  - `ffi` is loaded only if `jit_ffi_enable` (Computer.cpp:324-329), which defaults to false (configuration.cpp:212; api/configuration.hpp:67).
  - docs/accelerated.md still advertises ffi.
- craftos2-rom:
  - bios.lua:14,37: the shared ROM switches on `_VERSION == "Lua 5.1"` to choose `loadstring` and turn on a 5.1 compat layer (the CC bit stub at :95).
  - bios.lua:23: `if jit and jit.os == "OSX" and jit.arch == "arm64" then jit.off() end`.
  - motd.lua:13 parses `_HOST:match("CraftOS%-PC [%a]* ?(v[%d%.]+)")`.
  - screenfetch.lua:181-182 displays `_VERSION` and `_HOST`.

## 3. Ecosystem search (GitHub legacy code search via gh api; punctuation is ignored, so every hit was re-checked by regex on the downloaded file)
- Commands:
  - `python -I W/scripts/ghsearch.py W/ghsearch/queries.txt W/ghsearch/results.json`: 49 queries; totals in W/ghsearch/search.log.
  - `python -I W/scripts/fetch_blobs.py results.json blobs`: 3344 unique blobs, 41 HTTP 404.
  - `python -I W/scripts/classify.py results.json blobs cls`: W/ghsearch/cls.txt plus cls-hostpats.json and cls-verexprs.json.
- How a file counts as CC: it uses at least 2 distinct CC-only API names; this gave 897 files. CC/CraftOS-PC ROM copies are excluded.
- Selected totals:
  - `_VERSION "Lua 5.1" language:lua` 13,344
  - `"jit.version"` 8,816
  - `"jit.version_num"` 6,648
  - `jit pullEvent` 117
  - `_HOST CraftOS-PC` 37
  - `is_LuaJIT_21` (sha2.lua) 188 files in 169 repos
- In the CC files (non-ROM):
  - **Test `jit`:** 29 files, 18 repos. Almost all are truthiness checks such as `jit and X or Y`.
    - AUKit aukit.lua:131 `sincWindowSize = jit and 30 or 10`. Also :1011, :1100, :1401, :2357 and :2992 `csize = jit and 7680 or 32768`, which feeds `table_unpack` and `str_byte` ranges. The `sincWindowSize` identifier was found in 6 repos.
    - sanjuuni 32vid-player.lua:592 reads in 8000-byte chunks if `jit` (4 repos).
    - MCJack123 demos (Astronaut, SecondReality, 4thDimension) use it for fps or asset choice; these are guarded by CraftOS-PC's `config`.
    - Phoenix 00-init.lua:1116 panics if `jit and args.preemptive`.
    - LevelOS (an ATM10 startup.lua:730) downloads a "JITAlert" when `jit` is set; I did not see what it contains.
    - dorpos displays `_VERSION .. (jit and " (JIT)" or "")`.
  - **`jit.<field>` access:** 0 files. Only the CraftOS-PC ROM bios copies use jit.os, jit.arch and jit.off, and those are guarded.
  - **`_VERSION` branches:** 14 files, 13 repos. 11 are `== "Lua 5.1"` (Phoenix, old basalt.lua copies, cyberbit/telem, mat-1 miner, dr-roboto, CCTills alib which errors unless 5.1). 1 is `== "Lua 5.2"`. 2 are `< "Lua 5.2"`. 2 are Teal `_tl_compat` lines. None look for LuaJIT in `_VERSION`.
  - **`_HOST` parsing:** 41 files, 35 repos (111 files reference `_HOST` at all). One detects LuaJIT through `_HOST`: Xella37/Pumpkin-Carver pumpkincarver.lua `B=_HOST and _HOST:find("luajit")`, which is case-sensitive and matches Accelerated's "-luajit".
  - **`require 'bit'`:** 5 files; all prefer bit32 first, or are desktop code.
  - **LuaJIT BitOp names:** 3 files (a test fake and `local bit = bit32` aliases).
  - **CC bit names:** 50 files, 39 repos.
  - **ffi:** 1 file, DOOM-CC riscvcc.lua, gated by `if ffi then`.
- In the wider non-CC Lua code: `python -I -X utf8 W/scripts/jitfields.py results.json blobs` (W/ghsearch/jitfields.txt), plus an inline scan for jit-gated `require` calls.
  - 250 files test `jit`. Fields used: arch 72, version_num 71, off 45, version 34, on 16, os 15, status 13, opt 7, flush 3.
  - 73 sites in 56 files call jit functions after a `jit` test; 19 sites do not check the function exists first. Examples: LuLPeg lulpeg.lua:613 `jit and jit.status()`, maf.lua:5 `type(jit)=='table' and jit.status() and require 'ffi'`, LÖVE-style run.lua `jit.off/flush`, goluwa `jit.os:lower()`. None of these are CC files.
  - Unprotected `require` gated on `jit`: `jit and require "bit"` in 6 files (turbo, luamqtt bitwrap.lua:4) and `require "ffi"` in 19 files (mostly LÖVR). 0 are CC files.
- UnBIOS copies: Recrafted and LeonOS unbios.lua:29 do `_G._HOST = _G._HOST .. " (UnBIOS)"`, and their keptAPIs list (:17) keeps `_VERSION` and `_HOST` but drops `jit` and any new global. Phoenix's kernel/unbios.lua:16 keeps `jit` and `ffi`. Phoenix yellowbox.lua:113 rewrites the bracketed part of `_HOST`. So a second bracketed tag in `_HOST` already exists in the wild.
- Egor's sha2.lua used in CC:
  - MCJack123/CCSecureBoot ships it unmodified in a mod ROM (rom/modules/main/sha2.lua, v12; used by enroll-secure-boot.lua).
  - YCL0609/CCT-Factory-System and PrincessCyanMarine/cc_discord_REST_api vendor it unmodified.
  - Alexander1248/CCTweakedStorage replaced the detection with `local is_LuaJIT = false` (core/sha2.lua:120).
  - The check of which of the 169 sha2-vendoring repos contain CC code stopped at 84 of 169 (search rate limits): 5 do. File: W/ghsearch/s1_repo_cc.json.

## 4. Libraries (shallow clones in W/libs; commit in brackets). The "stock" result is a real run under Cobalt with CC's own cc.require: `java -cp cobalt-0.9.9.jar;W/cobalt-runner CobaltRun cfg_cobalt.lua mini_cc.lua libtests.lua` → W/harness/out-cobalt.txt.
- **rxi/json.lua** [dbf4b2d]: no checks (only bench/util/bench.lua:43 prints `jit and jit.version or _VERSION`). Stock: OK.
- **kikito/inspect.lua** [a8ca312]: inspect.lua:1 is the Teal `_tl_compat` line, `tonumber((_VERSION or ''):match('[%d.]*$')) or 0 < 5.3`. It gives 5.2 for "Lua 5.2", and 0 or 2.1 for option (b) strings; the "< 5.3" outcome is unchanged. Stock: OK.
- **rxi/lume** [98847e7], **middleclass** [359f0e2], **serpent** src [139fc18], **LibDeflate** [afc3b78], **lualzw** [11c238e], **empyreuma bigint** [98562d5]: no version or jit logic. Stock: all OK.
- **lbase64** [c261320]: base64.lua:27 uses `bit32.extract` first. :29 `_G.bit` (assumes LuaJIT BitOp; CC's stub has no lshift). :34 `_VERSION == "Lua 5.1"`. It picks `bit32.extract` under every option, so CCLuaJIT's C bit32 must keep `extract`. Stock: OK.
- **sha2.lua** (pure_lua_SHA) [6adac17, v12]:
  - :120 detects LuaJIT by the quirk. :129 `b = require"bit"`. :133-134 `pcall(require,"ffi")`. :136 `loadstring"b=0b0"`. :137 `jit.arch`. :140 order `_VERSION == "Lua 5.2" and {"bit32","bit"} or {"bit","bit32"}`.
  - Stock: OK (bit32 branch; sha256, sha512 and md5 test vectors pass).
  - CCLuaJIT: LuaJIT branch under every option, so `require"bit"` errors. Only with `_LOADED.bit` = LuaJIT bit does the require succeed (`ror` is a function). That path was not run end to end.
- **kikito/md5.lua** [4b5ce0c]: :37 `pcall(require,'bit')`, :38 `pcall(require,'ffi')`, :42 bit32 fallback. Stock: OK (bit32). With `_LOADED.bit` it takes its LuaJIT bit path.
- **lua-MessagePack** [a9e3b1f, framagit]: :5-8 `pcall(require,'jit')`; :13 `not jit and _VERSION < 'Lua 5.3'` leads to :16 `string.dump`. Stock: FAIL ("unable to dump given function"). On CCLuaJIT it skips `string.dump` only if `require 'jit'` succeeds (option c with `_LOADED.jit`), or with a `_VERSION` beginning "LuaJIT".
- **lua-compat-5.3** [2d3c70b]: module.lua:2 `_VERSION:sub(-3)`; :7 `< "5.3"`; :420-422 `string.dump`-based LuaJIT detection in the 5.1 branch only; init.lua:21 calls `string.dump` unconditionally. With "Lua 5.2 (LuaJIT)" the version becomes "IT)", so compat53 thinks it is on 5.3+ and installs no shims. Stock run: FAIL, but that was my harness (no `io` table); I added the `io` stub to mini_cc.lua afterwards and could not re-run.
- **Penlight** [5678f03]: pl/compat.lua:14 `lua51`; :18 `tostring(assert):match('builtin')` (LuaJIT quirk; patch 10's hashed tostring would remove it); :29 `_G.package.config`. Stock: FAIL in CC already ("attempt to index field 'package'"), so Penlight is not usable in CC either way.
- **Basalt** [14c0630]: Basalt/main.lua:458 `_HOST:find("CraftOS%-PC")`; unaffected by any option. **Basalt2** [ba6c691], **PrimeUI** [66f2d28], **classic**, **30log**, **semver**, **dkjson**: nothing found. **ccryptolib** [14489aa]: uses bit32 directly, including lrotate.
- **lua-bint** [08ca8ee]: needs `math.type` (5.3 integers); not run.

## 5. Probes (my own code; library detection lines are quoted, the libraries are not run)
- probe.lua: `java ... CobaltRun cfg_probe_cobalt.lua probe.lua` and `luajit.exe -e 'VARIANTS={...}' probe.lua`. Pivot with `python -I W/scripts/pivot.py probe-table.txt probe-cobalt.tsv probe-luajit.tsv`. Results relative to stock:
  - **sha2:** LuaJIT branch on all LuaJIT variants; `require'bit'` fails except in the `+bitL` variants. CraftOS-PC Accelerated also fails (no BitOp).
  - **MessagePack:** skips `string.dump` only with cL, cL+bitL, b3 and Accelerated.
  - **compat53:** `lua_version` "IT)" (b1), "2.1" (b2), ".2)" (b3).
  - **AUKit:** 30 / 7680 only with c and cL.
  - **Penlight:** `compat.jit = true` on every LuaJIT variant (`tostring(assert)` = "function: builtin#2").
  - **lbase64:** `bit32.extract` everywhere.
  - **CraftOS-PC bios:** 5.1 layer only under Accelerated.
  - `_HOST` checks (Basalt, AUKit, motd): unchanged by d.
- Result limits (limits.lua, limits2.lua → limits-*.txt): patched LuaJIT allows at most **8000** `string.byte` results ("string slice too long") and **7999** `unpack` results ("too many results to unpack"), in the main thread and in coroutines. Cobalt handles 524288 for both. So AUKit (csize 32768) and 32vid-player (48000) fail on CCLuaJIT unless `jit` is visible.
- strprobe.lua: identical on Cobalt and LuaJIT (`diff --strip-trailing-cr`). Output: W/harness/strprobe-cobalt.txt.
  - **`_VERSION` candidates:**
    - "Lua 5.2 (LuaJIT)", "Lua 5.2 LuaJIT 2.1" and "Lua 5.2 (CCLuaJIT 0.1.0)" each change `== "Lua 5.2"` to false, `:sub(-3)`, `tonumber(:sub(5))` to nil, `'^Lua 5%.[12]$'` to nil, and the Teal version number.
    - "LuaJIT 2.1 (Lua 5.2)" also flips `< 'Lua 5.3'`, `>= "Lua 5.3"` and `"^Lua 5%.2"`, and `"(%d+%.%d+)"` returns 2.1.
    - Unaffected: `== "Lua 5.1"`, `< "Lua 5.2"`, `>= "Lua 5.2"`, and croissant's `"Lua (%d+)"`.
  - **`_HOST` candidates (20 parsers):**
    - "… (Minecraft 1.21.1) (CCLuaJIT 0.1.0[; LuaJIT 2.1])" matches stock everywhere except three places: Inksaver clsTurtle.lua:32 (mcVersion becomes "1.21.1) (CCLuaJIT…"; major 1.21 still parses, minor becomes nil); WardenOS's display-only gsub; and rawterm's intermediate gsub (its final `match` result is unchanged).
    - That is exactly the set the existing "(UnBIOS)" suffix changes.
    - "(Minecraft 1.21.1; CCLuaJIT 0.1.0)" additionally breaks obsi2.lua:2004 `%(.-%)`.
    - Parsers that keep working: prefix-based ones (Phoenix/rawterm `ComputerCraft ([0-9%.]+)`, kwei `sub(17,19)`, obsi2 `sub(15,21)`), qtccv's second word, Recrafted/LeonOS keys.lua `%b()`, and old bios `string.find(_HOST,"ComputerCraft") == 1`.
- escprobe.lua (side finding): `"\)"` (an invalid escape, used by vacovsky tk.lua:26 and Inksaver clsTurtle.lua:24) compiles on Cobalt but is a compile error on LuaJIT and on PUC 5.4.

## 6. Options
- **(a) keep "Lua 5.2"**
  - Risk: none found. CC code expects exactly this string, and no CC code looks for LuaJIT in `_VERSION`.
  - Benefit: every check found behaves as on stock CC:T, and it keeps CraftOS-PC-style `== "Lua 5.1"` code on its tested 5.2 path.
- **(b) distinct but compatible**
  - Risk: no string preserves `== "Lua 5.2"` and `:sub(-3)` together. Measured changes: sha2's bit32 ordering and Phoenix kernel.lua:7988 branch; compat53 turns its shims off; `tonumber(:sub(5))` becomes nil; anchored matches fail. A "LuaJIT …" prefix is worse.
  - Benefit: none in CC; it is only for display.
- **(c) minimal `jit` {version, version_num}**
  - Benefit: it matches how CC code actually detects LuaJIT. AUKit and sanjuuni switch to chunk sizes that work under the measured 8000 limit, and MessagePack loads if the table is also in `_LOADED`. It also gives a clean signal that is visible only on LuaJIT computers.
  - Risks:
    - AUKit uses a 3x larger sinc window (more CPU).
    - LevelOS fetches a JIT alert.
    - Phoenix refuses preemptive mode (it would fail anyway, since CCLuaJIT has no `debug.sethook`).
    - Libraries outside CC that assume `jit` means `bit`, `ffi` or `jit.*()` will error (`jit and require "bit"`; unchecked `jit.status()`, `jit.off()`). The `bit` case is solved by making `bit` loadable with require. `version_num` must be present because sha2 evaluates `jit.version_num >= 20000`.
    - UnBIOS in Recrafted and LeonOS drops `jit` after it runs.
- **(d) `_HOST` suffix**
  - Benefit: it shows in `about` for humans, is set per machine, survives UnBIOS (which appends after it), and is parse-safe in all 20 harvested parsers when it is a separate trailing bracketed tag.
  - Risks: Inksaver's minor version becomes nil (as with UnBIOS today); display strings change. It must keep the "ComputerCraft <ver>" prefix and the "(Minecraft …)" group unchanged. A tag of "CCLuaJIT" or "LuaJIT" does not match the one `find("luajit")` user (Pumpkin-Carver).
- **(e) dedicated global or API**
  - Risk: low (a new name). But there are no consumers today, and UnBIOS keep-lists drop it.
  - Benefit: structured data such as result limits and JIT on/off. A module in the (fake) registry `_LOADED` (`require "ccluajit"`) would avoid touching `_G`.

## 7. Out-of-lane notes
- The 8000/7999 result limit is the strongest compatibility gap seen here; it ties to PLAN P13 / patch 09.
- Cobalt accepts invalid escapes such as `\)`; LuaJIT rejects them.
- DESIGN §3.5 line 160 ("Not present: … jit, … bit") would need amending if this recommendation is adopted.

## 8. Not done / limits
- After the first Cobalt library run, the classifier denied running third-party library code ("Code from External"). No LuaJIT-variant library runs happened; I did not work around this. All CCLuaJIT library verdicts come from the quoted detection lines evaluated in probe.lua.
- CraftOS-PC Accelerated was not run; it is analysed from source only.
- The sha2 repo-CC check stopped at 84 of 169 repos.
- Legacy code search covers only part of GitHub, ignores punctuation, and its totals are approximate.

## Files (scratch workspace)

- `work/version-survey/harness/probe.lua`
- `work/version-survey/harness/probe-table.txt`
- `work/version-survey/harness/probe-cobalt.tsv`
- `work/version-survey/harness/probe-luajit.tsv`
- `work/version-survey/harness/strprobe.lua`
- `work/version-survey/harness/strprobe-cobalt.txt`
- `work/version-survey/harness/strprobe-luajit.txt`
- `work/version-survey/harness/limits.lua`
- `work/version-survey/harness/limits2.lua`
- `work/version-survey/harness/limits-luajit.txt`
- `work/version-survey/harness/limits-cobalt.txt`
- `work/version-survey/harness/limits2-luajit.txt`
- `work/version-survey/harness/escprobe.lua`
- `work/version-survey/harness/mini_cc.lua`
- `work/version-survey/harness/libtests.lua`
- `work/version-survey/harness/run_lj.lua`
- `work/version-survey/harness/out-cobalt.txt`
- `work/version-survey/cobalt-runner/CobaltRun.java`
- `work/version-survey/scripts/extract_rom.py`
- `work/version-survey/scripts/ghsearch.py`
- `work/version-survey/scripts/fetch_blobs.py`
- `work/version-survey/scripts/classify.py`
- `work/version-survey/scripts/jitfields.py`
- `work/version-survey/scripts/repo_is_cc.py`
- `work/version-survey/scripts/pivot.py`
- `work/version-survey/ghsearch/queries.txt`
- `work/version-survey/ghsearch/results.json`
- `work/version-survey/ghsearch/search.log`
- `work/version-survey/ghsearch/cls.txt`
- `work/version-survey/ghsearch/cls-hostpats.json`
- `work/version-survey/ghsearch/cls-verexprs.json`
- `work/version-survey/ghsearch/jitfields.txt`
- `work/version-survey/ghsearch/s1_repo_cc.json`
- `work/version-survey/ghsearch/sha2_cc.json`
- `work/version-survey/ghsearch/sha2copies`
- `work/version-survey/cospc-run/headless-identity.txt`
- `work/version-survey/build-luajit-cc.log`
- `work/version-survey/luajit-cc/src/luajit.exe`
