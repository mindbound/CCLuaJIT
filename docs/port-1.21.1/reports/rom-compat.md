# rom-compat: Lua dialect compatibility, CC:T 1.121.0 ROM vs Cobalt 0.9.9 vs LuaJIT 2.1

Lane: what CC: Tweaked 1.121.0 (MC 1.21.1) `bios.lua` + ROM and typical CC programs expect, what
Cobalt 0.9.9 provides, and what LuaJIT 2.1 provides in a default build and in a
`LUAJIT_ENABLE_LUA52COMPAT` build. I tested this empirically with:

* a real-ROM boot harness that runs unchanged under Cobalt and under LuaJIT,
* a Cobalt runner, so every probe can be compared against the real VM,
* differential fuzzers.

All work is in `<scratch>\work\rom-compat`
(called `W` below). Nothing was written to the repository.

## 0. Bottom line

* **Without a compatibility layer, CCLuaJIT cannot boot CC:T 1.121.0 on any LuaJIT build.** The BIOS
  dies during API loading:
  `bios.lua:561: Failed to load API colours due to /rom/apis/colours.lua:13: attempt to index local 'colours' (a nil value)`.
  Fifteen ROM files use `_ENV` as a value, and LuaJIT has no `_ENV`. The computer stops with a blank
  screen after the first resume.
* The libraries that CCLuaJIT 1.20.1 `main.cpp` opens are not enough:
  * There is no `debug` library. `cc/internal/exception.lua:90` needs it, so the `parallel` API fails to load.
  * `bit32` is only an alias of `bit`.
  * There is no `utf8` or `string.pack`.
  * There is no `table.pack` without 52COMPAT.
* With the shim from this lane (`cclj_prebios.lua`, about 830 lines of Lua), the **real BIOS boots to the
  multishell shell on every LuaJIT build and runs `ls`, `lua`, `mkdir`, `cd`, `delete`, `id`, `sleep`, `alias`,
  `programs`, `apis`, `shutdown`, `terminate`**. User programs and `require` also work. Screens match Cobalt
  except for the random MOTD line and Cobalt's column-based error snippet.
* Some fixes need C:
  * Two correctness problems cannot be shimmed cheaply and need small LuaJIT C patches:
    * `textutils.serialiseJSON` fails on every string, because LuaJIT patterns cannot contain `\0`.
    * `tostring(number)` uses 14 significant digits, so textutils/settings/JSON serialisation silently loses precision.
  * The goto/52COMPAT conflict below also needs a C patch.
  * Security-relevant pieces must be native. Examples are bytecode loading and `debug.sethook`. Any Lua
    wrapper can be bypassed with `debug.getupvalue`; I demonstrated this.
* **Recommendation:**
  * Build LuaJIT with `LUAJIT_ENABLE_LUA52COMPAT` + `LUAJIT_DISABLE_FFI`, plus four small patches I wrote and
    tested: goto stays contextual, Cobalt-style number formatting, NUL-safe patterns, and no bytecode
    loading (`LUAJIT_DISABLE_BCLOAD`).
  * Add the prebios shim on top.
  * Do **not** use plain 52COMPAT: it makes `goto` a reserved word. Every CC program with a `goto(x, y, z)`
    function or a `t.goto` field then fails to compile. This is a common turtle-navigation idiom. Cobalt
    allows it.
* Cross-lane observation, verified: CCLuaJIT's `initMachine` never calls `luaopen_jit`, so **LuaJIT runs
  interpreter-only**. A benchmark ran in 0.879 s without `luaopen_jit`, 0.887 s under `luajit -joff`, and
  0.044 s with `luaopen_jit` (20x). The shim's Lua `bit32` is 28x faster than Cobalt with the JIT on, but 4x
  slower than Cobalt with the JIT off.

## 1. Methodology and tools built

| Tool | Path | Purpose |
|---|---|---|
| LuaJIT default | `W\luajit-default-src\src\luajit.exe` | copy of the shared MinGW build (2.1 ROLLING, GC64) |
| LuaJIT 52COMPAT | `W\luajit-52\src\luajit.exe` | `mingw32-make XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT` (log `W\build-52compat.log`) |
| LuaJIT "cc" (recommended) | `W\luajit-cc\src\luajit.exe`; reproducible via `W\build_luajit_cc.sh <pristine-tree> <out>` → `W\luajit-cc-repro` | 52COMPAT + DISABLE_FFI + DISABLE_BCLOAD + 4 patches in `W\patches\` |
| Cobalt runner | `W\cobalt-runner\CobaltRun.java` (`java -cp cobalt-0.9.9.jar;. CobaltRun a.lua b.lua`) | Runs Lua under Cobalt 0.9.9 with the **same globals CobaltLuaMachine installs**: `CoreLibraries.debugGlobals` + `Bit32Lib` + `_HOST`. Adds `__write`, `__readfile`, `__clock`, and a Lua `print`. |
| 3-way runner | `W\diff3.sh file.lua` + `W\diffparse.py` / `W\diff2.py` | same probe file under Cobalt / LJ default / LJ 52COMPAT |
| Boot harness | `W\harness\cc_harness.lua` (+ `configs\*.lua` via `make_config.py`, `run.sh <cobalt\|ljdef\|lj52\|ljcc> <config>`) | Runs the **real** 1.21.1 `bios.lua` + ROM. It mocks the Java APIs (term with a 51x19 screen buffer, a virtual fs whose read-only rom mount is backed by the extracted ROM, os timers/events/clock, peripheral, redstone/rs, `http=nil`). It drives the BIOS coroutine exactly like `CobaltLuaMachine.handleEvent`: first resume with no args, event filter, terminate always delivered. It feeds scripted input (`paste` + `key enter`) and dumps the screen. The same file runs under Cobalt, which gives a ground-truth transcript. |
| In-computer probes | `W\harness\compat.lua` (94 checks → `/compat.out`), `extra.lua`, `syn.lua`, `errtest.lua`, `mymod.lua`, `envprobe.lua`, `strictprobe.lua` | run as CC programs inside the harness |
| Probe battery | `W\t-sem.lua` (255 probes), `W\t-syntax.lua`, `W\t-goto.lua`, `W\t-surface.lua`, `W\t-loadmsg2.lua` | dialect differences |
| Fuzzers | `W\t-numfmt.lua` (123,516 doubles), `W\t-patfuzz.lua` (2×30,000 patterns), `W\t-shimfuzz.lua` (62,001 bit32/utf8/pack cases) | Use a deterministic Park-Miller LCG that is exact in doubles, so all VMs see identical inputs. |
| Benchmarks | `W\t-shimbench.lua`, `W\jitcheck\jitcheck.c` | shim overhead; JIT-on/off check |

Harness outputs: `W\harness\out\*.txt`, named `<config>-<vm>.txt`.
* Configs: `cobalt` = reference; `mod120` = libs exactly as 1.20.1 `main.cpp`; `full` = all safe std libs,
  no shim; `s1`/`s2` = staged shims; `shim` = final shim; `shim120` = final shim with main.cpp libs;
  `x*` = extra session; `y*` = syntax-error session.
* `*.compat.txt` are the extracted probe results.

## 2. Syntax: every ROM file under every build

`W\syntaxcheck.lua` calls `loadfile` on all 105 `.lua` files (bios.lua + rom/**).

```
luajit-default : checked 105 files, 0 failed
luajit-52      : checked 105 files, 0 failed
luajit-cc      : checked 105 files, 0 failed
PUC Lua 5.4.6  : checked 105 files, 0 failed
```

The ROM avoids every syntax difference. User programs do not; see section 6.

## 3. Cobalt 0.9.9 dialect and what CC:T adds

Sources: `cct\cobalt-src\org\squiddev\cobalt\...` and `cct\src-1.21.1-1.121.0\dan200\computercraft\core\lua\CobaltLuaMachine.java`.

* **Machine globals** (`CobaltLuaMachine.java:53-57`):
  * `CoreLibraries.debugGlobals`: Base, Table, String, Coroutine, Math, Utf8, Debug.
  * `Bit32Lib`.
  * `ErrorInfoLib`: registers `cc.internal.error_info` in `_LOADED`.
  * `_HOST` and `_CC_DEFAULT_SETTINGS`.
  * Plus the Java APIs.
  * No `io`, `os` lib, `package`/`require`, `print`, `dofile`, `loadfile`, `collectgarbage`, `gcinfo`, or `newproxy`.
    The BIOS defines `print`/`dofile`/`loadfile`; the ROM supplies `io` and `require`.
* **Language level: Lua 5.2** (`BaseLib.java`: `_VERSION = "Lua 5.2"`). The VM uses 5.2 opcodes
  (`OP_GETTABUP`, `OP_LOADKX` in `ErrorInfoLib.java`).
  * `_ENV` is a real upvalue.
  * `goto` and labels are supported. `goto` is **contextual**, not reserved (`Lex.java:58-59`: "We skip GOTO and inject it later").
  * Escapes `\x`, `\z` and `\u{}` are supported; `\u{}` goes up to 0x7FFFFFFF (`Lex.java:430-462`).
  * Empty statements are allowed.
  * No `//`, no bitwise operators, no `<const>`/`<close>`, no integer subtype. Integral doubles are cached
    as `LuaInteger`, so `-0.0` normalises to `0`.
* **Base library**:
  * Functions: `error`, `setfenv`/`getfenv` (via `LegacyEnv`), `assert`, `getmetatable`, `loadstring`,
    `select` (negative indices allowed), `type`, `rawequal`, `rawget`, `rawset`, `rawlen`, `setmetatable`,
    `tostring`, `tonumber`, `next`, `unpack` (global), `pcall`.
  * `pairs` honours `__pairs`.
  * `ipairs` honours `__index`, as in Lua 5.3. It ignores `__ipairs`.
  * `xpcall` takes extra arguments.
  * `load(str|fn, name, mode, env)`. CC:T configures no bytecode format, so binary chunks are rejected and
    `string.dump` fails with "unable to dump given function".
* **Libraries**:
  * table: `getn`, `maxn`, `create`, `remove`, `concat`, `insert`, `move`, `pack`, `sort`, `foreach`,
    `foreachi`, `unpack`. These honour metamethods, as in Lua 5.3.
  * string: 5.1 set plus `pack`, `unpack`, `packsize`, `gfind`. Patterns may contain `\0`. `format("%q")`
    follows Lua 5.3 (numbers unquoted, hex floats).
  * coroutine: `create`, `running` (2 results), `status`, `isyieldable`, `wrap`, `resume`, `yield`. No `close`.
  * math: 5.1 set + `mod`, and `atan(y, x)`. No `type`, `tointeger`, `ult`, `maxinteger`, `mininteger`.
    `log(x, b)` is computed as `Math.log(x) / Math.log(b)`.
  * utf8: full 5.3 library, with max code point U+10FFFF.
  * debug: full, including `getinfo(...).currentcolumn`.
  * bit32: full, with Java semantics (`Bit32Lib.java`).
* **Number to string**: `%.15g`, falling back to `%.17g` when that does not round-trip (`LuaDouble.java:125-145`).

### Library surface measured (`W\t-surface.out`, Y = present)

| name | Cobalt | LJ default | LJ 52COMPAT |
|---|---|---|---|
| table.pack / table.unpack / rawlen | Y | - | Y |
| table.move, getn, maxn, foreach(i) | Y | Y | Y |
| table.create, math.mod, string.gfind | Y | - | - |
| utf8.* | Y | - | - |
| string.pack/unpack/packsize | Y | - | - |
| bit32.* | Y | - | - (LuaJIT has `bit`) |
| coroutine.isyieldable / running | Y | Y | Y |
| coroutine.close, math.type/tointeger/ult | - | - | - |
| dofile, loadfile, collectgarbage, gcinfo, newproxy, module, require, package, io, os, jit | - | Y | Y |
| debug.getuservalue/setuservalue | - | - | Y |

(`print` shows as present under Cobalt only because CobaltRun defines it.)

## 4. Staged boot: every failure and its fix (real ROM, harness)

Configs and outputs are in `W\harness\configs` and `W\harness\out`. Each stage adds one fix and records the
first new failure.

| Stage | Build(s) | Result (verbatim) | Fix applied in next stage |
|---|---|---|---|
| 0. no shim, libs as `main.cpp` (`mod120`) or full safe libs (`full`) | default, 52COMPAT, cc | `!!!! BIOS COROUTINE ERRORED: bios.lua:561: Failed to load API colours due to /rom/apis/colours.lua:13: attempt to index local 'colours' (a nil value)`. 0 events, blank screen. | `_ENV` emulation: `_G._ENV=_G`, and `load(...,env)` sets `env._ENV=env`. This mirrors what CC's own 5.1-era BIOS did; it is also why `os.loadAPI` still skips the `_ENV` key (bios.lua:515). |
| 1. +_ENV (`stages/stage1.lua`) | default | Screen shows `/rom/apis/parallel.lua:72: attempt to call field 'pack' (a nil value)`, then `Press any key to continue` → shutdown | table.pack/unpack |
| 1. +_ENV | 52COMPAT, cc | **Boots to shell**. `lua> bit32.extract(...)` → `attempt to call field 'extract' (a nil value)`. `colors.packRGB(0.5,0.25,1)` = 8405247 (Cobalt 8339455). The utf8 global is nil. compat: 14 errors. | real bit32, utf8, ... |
| 2. +table.pack (`stages/stage2.lua`) | default | Boots. compat: 15 errors. `serialiseJSON: textutils.lua:448: malformed pattern (missing ']')`; `textutils.lua:432` and `json.lua:63: attempt to index global 'utf8'`; `bit32.extract/lrotate` nil; `string.pack/unpack` nil (breaks `rom/programs/fun/speaker.lua:75`); `rawlen` nil; `table.sort: invalid order function for sorting`. Silent: packRGB wrong; serialise not round-tripping. | full shim, and NUL-pattern + numfmt C patches |
| full shim, libs as `main.cpp` (`shim120`) | default | `bios.lua:561: Failed to load API parallel due to /rom/modules/main/cc/internal/exception.lua:90: attempt to index local 'debug' (a nil value)` | **native must open the debug library** |
| full shim (`shim`) | default | Boots and runs the whole session. 23/94 probe checks differ from Cobalt, including `serialiseJSON !! ERROR textutils.lua:448` and `serialise-roundtrip = false,false,false`. | C patches |
| full shim | 52COMPAT | 21/94 differ. Also `goto-ident = false`: `local function goto` fails to compile. | goto patch |
| **full shim** | **cc (patched)** | **11/94 differ.** All are expected or unfixable; see below. | — |

The session script is in `make_config.py`. Its last screen under luajit-cc + shim: `CraftOS 1.9`, MOTD,
`ls rom` listing, the Lua REPL evaluating `1+1`, `textutils.serialise`, `2^53`/`1/3` printed with Cobalt's
digits, `bit32`, `colors`, `utf8`, the error for `local t=nil; t.x=1`, `compat: 94 checks, 1 errors`,
`errtest` error, `mkdir/cd/delete`, `id`, `sleep 1`, `alias`, `programs`, terminate, `shutdown`.

Remaining 11 differences, recommended build + shim vs Cobalt (`out\cobalt-cobalt.compat.txt` vs `out\shim-ljcc.compat.txt`):
1. `utf8.charpattern` displayed differently: only `%q` style.
2. `#{1,nil,3}`: 1 vs 3. The border of a table with holes is implementation-defined.
3. `local _ENV = {...}` is ignored (parser feature).
4–5. `math.random` sequence differs. `math.random(0)` returns 1; Cobalt errors with "interval is empty".
6. `tonumber("0b11")` = 3; Cobalt gives nil. I left this harmless superset alone.
7. Yield inside a `string.gsub` callback → "attempt to yield across C-call boundary".
8. Function address format: `function: 0x...` vs `function: 79698539`.
9–10. `cc.expect` errors raised directly through `pcall` carry no position prefix. Ordinary API-call
   errors match: see the `extra` session, `api-error`.
11. `_ENV` is visible as a global. It also shows up in the `apis` program listing (`_ENV bit bit32 colors ...`).

Other sessions:
* The syntax-error session (`out\ycobalt-cobalt.txt` vs `out\yshim-ljcc.txt`) is identical except for the
  random MOTD line, which uses a different `math.random` sequence. That includes the multi-line "Unexpected
  end of file ... Block started here" diagnostics. Syntax errors are rendered by the ROM's Lua parser
  `cc.internal.syntax`, so they work unchanged.
* The extra session matches on `require`, API error messages, `fs` errors, `os.run` env, `strict_globals`
  (`Attempt to create global undeclared_global`) and the table-globals list (plus `_ENV`).
* The only visible screen difference besides these is the random MOTD line.

One feature degrades gracefully: CC:T 1.109+'s rich runtime-error display (code snippet with caret) needs
`debug.getinfo(...).currentcolumn` (`cc/internal/exception.lua:213`). LuaJIT has no column info, so
`report()` returns early and only the plain message is shown. The "did you mean" hints need the Java
`cc.internal.error_info` (`error_hints.lua:146`) and are silently skipped as well.

## 5. Static scan of the ROM: runtime features LuaJIT lacks or does differently

Counts exclude `rom/help`. Paths are relative to `cct\rom-1.21.1\data\computercraft\lua`.

| Feature | ROM use (file:line examples) | Impact on LuaJIT without fixes |
|---|---|---|
| `_ENV` as a value | 18 hits / 15 files: `rom/apis/colours.lua:11`, `fs.lua:12`, `keys.lua:137`, `term.lua:18`, `command/commands.lua:54`, `turtle/turtle.lua:32`, `textutils.lua:807`, `cc/require.lua:19`, `cc/internal/edit_runner.lua:23`, `programs/lua.lua:26`, `shell.lua:70`, `edit.lua:181`, `http/pastebin.lua:151`, `http/wget.lua:65`, `bios.lua:463` | **Fatal**: the BIOS cannot load APIs |
| `table.pack` / `table.unpack` | 25 / 39 hits (e.g. `bios.lua:42,46`, `parallel.lua:72`) | Fatal on default build; fine on 52COMPAT |
| `bit32` (full) | `bios.lua:22-30`; `colors.lua:223,250,270,290-292,311-313`; `textutils.lua:436` (extract), `763` (band/arshift); `cc/audio/dfpwm.lua:64`; `cc/base64.lua:19` | The mod's `bit32 = bit` alias lacks extract/replace/btest/lrotate/rrotate, gives signed results, and rounds non-integers (127.5 → 128; Cobalt truncates to 127), so `colors.packRGB` is wrong |
| `utf8` | `textutils.lua:432` (`serialiseJSON` with `unicode_strings`), `cc/internal/json.lua:63` (every `\uXXXX` escape in JSON input), `snbt.lua:63,67,71` | JSON parsing of escaped text fails |
| `string.unpack` | `programs/fun/speaker.lua:75,77,103` | `speaker play file.wav` fails |
| `\0` inside a Lua pattern | `textutils.lua:448` `s:gsub("[\0-\x1f\"\\]", map)` (every `serialiseJSON` of a string) | **Error on every JSON string**. In general, patterns containing `\0` silently mis-match: the pattern is truncated at the NUL. |
| `tostring(number)` precision | `textutils.lua:367` (serialise), `textutils.lua:531` (JSON) → `settings.save`, rednet, user saves | Silent data loss: `0.1+0.2` → `"0.3"`; `2^53-1` → `"9.007199254741e+15"` |
| `debug` library | `cc/expect.lua:53,75`, `cc/internal/exception.lua:20,90,101,121,128`, `cc/require.lua:134`, `cc/pretty.lua:38`, `cc/internal/error_hints.lua:108-124` | Fatal if not opened (main.cpp does not open it) |
| `debug.getregistry()._LOADED` copied into `package.loaded` | `cc/require.lua:134-140` | **Security**: whatever is in `_LOADED` (`jit`, `ffi`, `io`, `os`, `package` if opened) becomes `require`-able by users |
| `currentcolumn` | `cc/internal/exception.lua:213` | Graceful degradation (no snippet) |
| `cc.internal.error_info` | `error_hints.lua:146` | Graceful (no hints) |
| `coroutine.running()` | `parallel.lua:70,183`, `exception.lua:118` | OK: always called inside a coroutine |
| `load(s, name, mode, env)` | `bios.lua:436` | OK (LuaJIT 2.1 supports it) |
| `math.log(x, 2)` | `window.lua:69`, `colors.lua:375` (fallback paths only) | LuaJIT is inexact for 2^3, 2^6, 2^7, 2^12, 2^13, 2^14. I verified these ROM paths only see non-powers, so no ROM impact. User idiom `t[math.log(c,2)+1]` → nil. |
| `math.random` | 34 hits (motd, fun programs) | Different sequence; cosmetic |
| `__name`, `__tostring` metatables | `io.lua:25`, `peripheral.lua:174-293`, `vector.lua:218`, `exception.lua:51-68`, `expect.lua:56` | OK (read via rawget/getmetatable) |
| `__pairs`/`__len`/`__ipairs`/`__gc`/`__close`, `goto`, `string.dump`, `setfenv`/`getfenv`, `rawlen`, `table.move`, `math.type`, `xpcall` with args, `select(-n)` | none in the ROM (`xpcall` only as a table key in exception.lua:105) | — |
| `string.format('%q')` | `textutils.lua:360`, json/snbt error messages | Cosmetic: `\r` → `\13`, `\0` → `\0` instead of `\000`; still valid Lua |
| `table.sort` | 14 calls, all with valid comparators | OK |

## 6. Silent semantic differences: user programs that run but behave differently

Measured with the 255-probe battery `W\t-sem.lua`, counted as identical / error-text-only / behaviour differs:
* Cobalt vs LuaJIT default: 158 / 18 / **79**.
* Cobalt vs LuaJIT 52COMPAT: 164 / 18 / **73**.
* Cobalt vs **luajit-cc + shim**: 195 / 18 / **42**.

Several of those 42 are artefacts of the test runner (`print` removed by the shim, `%q` display). The raw
dumps are `W\t-sem.out`, `W\t-sem-shim.out` and `W\t-sem-final-valuediffs.txt`.

| Difference | Example (Cobalt → LuaJIT) | How much it matters for CC programs | Fix |
|---|---|---|---|
| Number to string | `1/3` "0.33333333333333331" → "0.33333333333333"; `0.1+0.2` "0.30000000000000004" → "0.3"; `2^53` "9007199254740992" → "9.007199254741e+15"; `1e14` "100000000000000" → "1e+14"; `-0` "0" → "-0" | **High**: serialise/settings/JSON/rednet round trips lose data, and output looks different. 86,706 of 123,516 test values print differently. | **C patch** `luajit-numfmt-roundtrip.patch`: 0 / 123,516 differences after patching (`W\numfmt.*.txt`) |
| `\0` in patterns | `("a\0b"):gsub("[\0-\31]","?")` → error; `("a\0a\0"):gsub("\0","0")` → `"0a0\0000a0\0000",5` (Cobalt `"a0a0",2`) | **High** (breaks serialiseJSON; silently wrong for binary-data parsers) | **C patch** `luajit-pattern-nul.patch`. Pattern fuzz without NUL: patched == unpatched (0/30,000). With NUL: 3,312 → 296 differences vs Cobalt. The remaining 296 are a Cobalt quirk where `%f` matches at end-of-subject; LuaJIT agrees with PUC 5.4 there. |
| `bit32` rounding/sign | `bit32.band(127.5,255)` 127 → 128; `bit32.bnot(0)` 4294967295 → -1 | **High** (colours, checksums, networking) | Shim (20,000-case fuzz: 0 differences) |
| `goto` reserved under 52COMPAT | `local function goto(x,y,z)` → compile error | **High** for turtle scripts | **C patch** `luajit-goto-contextual.patch` |
| 5.1 syntax in default mode | `;;`, `break` not last, `f\n(x)` → compile errors | Medium | 52COMPAT |
| `#t` with holes | `#{1,nil,3}`: 3 → 1; `select('#', unpack({1,nil,3}))`: 3 → 1 | Medium (`local args = {...}` with nil args) | Unfixable (implementation-defined) |
| `pairs` order | different key order | Low-medium (serialised output order, "first key" logic) | Unfixable |
| `math.log(x,2)` | `math.log(8,2)` 3 → 2.9999999999999996 | Medium (colour-index idiom) | Shim (exact via frexp; 0/81 powers off) |
| `math.atan(y,x)` | the second argument is ignored silently | Low-medium | Shim |
| `math.random` | `random(0)`, `random(3,1)`, `random(2^40)`: Cobalt errors, LuaJIT returns a value; different sequence for the same seed | Low (procedural generation with a seed differs) | Sequence unfixable; errors could be shimmed |
| `tonumber("inf"/"nan")` | nil → inf/nan | Low-medium: input validation, e.g. `for i=1,tonumber(read())` runs forever | Shim |
| `table.sort` with an inconsistent comparator (`<=`) | Cobalt sorts silently → LuaJIT raises "invalid order function for sorting" | Medium (a common beginner bug that "works" on CC:T) | Shim: pure-Lua sort. It never throws, allows yields, and is faster: 22 vs 39 ms (no comparator), 54 vs 80 ms (with comparator). |
| Yield across C boundary | Yield in a `gsub` callback, a `string.format("%s")` `__tostring`, or a `load` reader function → "attempt to yield across C-call boundary" (Cobalt allows). Metamethod, `pcall`, `xpcall`, `pairs` and `tostring` yields work in LuaJIT. | Low-medium (a peripheral call inside a gsub callback) | Sort is fixed by the shim; the rest needs a Lua reimplementation |
| table library ignores metamethods | `table.concat`/`unpack`/`insert` on proxy tables (`__index`/`__newindex`) | Low | Shim possible (slow) |
| `ipairs` with `__index` | Cobalt follows `__index`; LuaJIT is raw | Low | Shim (fast path kept: 4 ms vs 3 ms per 10M iterations) |
| `__len`, `__pairs`, `rawlen` | ignored / missing in default | Low-medium (OOP libraries) | 52COMPAT (or shim for `pairs`/`rawlen`) |
| `local _ENV = t`, `_ENV = t` | ignored by LuaJIT | Low (sandbox libraries) | Needs LuaJIT parser work; not recommended |
| `for i=1,3,0` | Cobalt skips; LuaJIT loops forever | Low (relies on the timeout) | — |
| `%q` | `"\r"` → `\13`, `\0` → `\0`, numbers quoted | Low (still valid Lua) | Optional |
| Error-message wording | `bad argument #1 to '?' (...)` vs Cobalt `p:1: bad argument #1 (...)`; `near '<eof>'` vs `near <eof>` | Low (programs matching error text) | — |
| `tostring(f)` / `tostring(t)` | `function: 0x01b6...` vs `function: 79698539`; Cobalt also honours `__name` in `tostring` | Low | — |
| FFI literals | `1LL`, `1i` compile in default LuaJIT (cdata!) | Low (and a sandbox surface) | `LUAJIT_DISABLE_FFI` |

## 7. Security-relevant dialect findings in this lane

* **Bytecode**: LuaJIT's `load`/`loadstring`/`loadfile` accept binary chunks, and LuaJIT has no verifier
  (already established). A Lua-level guard is not enough. I showed that `debug.getupvalue(load, 8)` returns
  the shim's captured `native_load`, and `native_load(bc)()` then prints `bytecode ran`.
  * Fix in C: `patches\luajit-no-bytecode.patch`, which adds `-DLUAJIT_DISABLE_BCLOAD` in `lj_load.c` cpparser.
  * Verified: `load(string.dump(f))` → `nil  attempt to load a binary chunk` for load, loadstring and mode "b".
* **`debug.sethook`**: LuaJIT hooks are global per `lua_State` group, and CCLuaJIT's timeout uses
  `lua_sethook` (`main.cpp:466,825`). A user `debug.sethook()` can therefore clear the timeout hook. The shim
  replaces `sethook` with an error, but the real fix must be native: a chaining or virtualised hook.
* **`require` leaks**: `cc/require.lua:134` copies every string key of `registry._LOADED` into each program's
  `package.loaded`. In stock LuaJIT, `_LOADED` lists `_G,bit,coroutine,debug,io,jit,jit.opt,math,os,package,string,table`
  (`t-sem.out` probe dbg8). Cobalt lists `bit32,coroutine,debug,math,string,table,utf8`.
  * Do not open io/os/package/ffi natively.
  * Remove `jit`/`jit.opt` from `_LOADED` after `luaopen_jit`.
  * The shim also sanitises `_LOADED` (verified `registry-loaded` matches Cobalt).
* `main.cpp` sets a global `__inext` (`main.cpp:716-717`). It also leaves `dofile`, `loadfile` and `print`
  from `luaopen_base`; these touch the server's file system and stdout until the BIOS overrides them. The
  shim removes them, but native removal is cleaner.

## 8. Compatibility matrix

| Feature | Needed by ROM? | Cobalt 0.9.9 | LuaJIT default | LuaJIT 52COMPAT | Fix (recommended) |
|---|---|---|---|---|---|
| `_ENV` as value (`local x = _ENV`, `load(...,_ENV)`) | **yes** (15 files) | yes | no | no | Lua shim (`env._ENV = env`) — done, verified |
| `local _ENV` / assigning `_ENV` | no | yes | no | no | unfixable without parser patch |
| `bit32` (full, Cobalt semantics) | **yes** | yes | no (`bit`: signed, rounding, missing fns) | no | Lua shim on `bit.*` (fuzz 0/20k); C if JIT off |
| `table.pack`/`table.unpack` | **yes** | yes | no | yes | 52COMPAT (shim fallback) |
| `utf8` library | **yes** | yes | no | no | Lua shim (fuzz 0/22k); C optional |
| `string.pack/unpack/packsize` | **yes** (speaker) | yes | no | no | Lua shim (fuzz 0/20k, 5x slower than Cobalt) → **C impl recommended** |
| `\0` in patterns | **yes** (serialiseJSON) | yes | no | no | **LuaJIT C patch** (done) |
| Round-trip `tostring(number)` | **yes** (serialise/settings/JSON) | yes | no | no | **LuaJIT C patch** (done, 0/123k) |
| `debug` library | **yes** | yes | yes (if opened) | yes | **native: open it** (main.cpp does not) |
| `debug.getinfo` `currentcolumn` | optional (error snippet) | yes | no | no | unfixable cheaply (graceful) |
| `cc.internal.error_info` | optional | Java | no | no | port later (graceful) |
| `load` mode/env | **yes** | yes | yes | yes | shim wrapper (text-only, `_ENV`, Cobalt messages) |
| Binary chunks / `string.dump` | no | rejected | **accepted (unsafe)** | **accepted** | **C patch BCLOAD** + shim `string.dump` error |
| `setfenv`/`getfenv` | no | yes | yes | yes | — |
| `coroutine.running` (2 values) | uses 1 | yes | 1 value (nil in main) | yes | 52COMPAT / shim |
| `coroutine.isyieldable` | no | yes | yes | yes | — |
| `xpcall(f, h, ...)` | no | yes | yes | yes | — |
| `__pairs` | no | yes | no | yes | 52COMPAT / shim |
| `__len` (tables) | no | yes | no | yes | 52COMPAT (VM level) |
| `__ipairs` | no | ignored | ignored | honoured | — (rare) |
| `ipairs` honours `__index` | no | yes | no | no | shim |
| `rawlen` | no | yes | no | yes | 52COMPAT / shim |
| `;;`, `break` mid-block, `f\n(x)` | no | yes | **compile error** | yes | 52COMPAT |
| `goto` as identifier | no | yes (contextual) | yes | **compile error** | **C patch goto-contextual** (done) |
| `goto`/labels | no | yes | yes | yes | — |
| `\x`, `\z`, `\u{}` escapes | no | yes (`\u` up to 2^31) | yes (`\u` up to U+10FFFF) | yes | — |
| `//`, bitwise ops, `<const>` | no | no | no | no | — (parity) |
| FFI literals `1LL`, `1i` | no | no | yes | yes | `LUAJIT_DISABLE_FFI` |
| `math.atan(y,x)` | no | yes | ignores x | ignores x | shim |
| `math.log(x,2)` exact on 2^k | fallback only | yes | no | no | shim |
| `math.mod`, `string.gfind`, `table.create` | no | yes | no | no | shim aliases |
| `math.random` semantics/sequence | motd only | 5.4-like | lenient | lenient | sequence unfixable |
| `tonumber("inf"/"nan")` → nil | no | yes | no | no | shim |
| `table.sort` tolerant + yieldable | no | yes | no | no | shim (Lua sort) |
| Yield in gsub callback / `format %s` / load reader | no | yes | no | no | unfixable cheaply |
| table library honours metamethods | no | yes | no | no | possible shim (slow) |
| `#` border with holes / `pairs` order | — | impl | impl | impl | unfixable |
| `%q` style, error wording, `tostring(fn)` format | cosmetic | 5.3 style | 5.1 style | 5.1 style | optional |
| `_VERSION` | no | "Lua 5.2" | "Lua 5.1" | "Lua 5.1" | shim sets "Lua 5.2" |
| `io`/`os`/`package`/`jit`/`ffi`/`dofile`/`loadfile`/`collectgarbage`/`gcinfo`/`newproxy` | must be absent | absent | present | present | native: don't open / remove (shim also removes) |
| `debug.sethook` safety | no | per-thread, can't stop interrupts | **global, can clear timeout hook** | same | **native virtualisation** (shim: refuse) |

## 9. Performance of shimmed functions (`W\t-shimbench.lua`, ms, best of 3)

| Benchmark | Cobalt (JVM, after warm-up) | LuaJIT native | LuaJIT + shim (JIT) | LuaJIT + shim (`-joff`) |
|---|---|---|---|---|
| bit32 band/bxor/rshift ×3M | 202 | (n/a; `bit` 0, hoisted) | **7** | 825 |
| ipairs 1M ×10 | 302 | 3 | 4 | 41 |
| table.sort 200k | 69 | 39 | **22** | 67 |
| table.sort 200k + comparator | 336 | 80 | **54** | 68 |
| tonumber(str) ×1M | 75 | ~0 | ~0 | 130 |
| utf8.len 140 KB ×10 | 3.1 | — | 2 | 50 |
| string.pack/unpack `<i4d` ×100k | **31.5** | — | 149 | 329 |
| load 200-line chunk ×2000 | 136 | 30 | 30 | 38 |

`os.clock` resolution on Windows is about 1 ms; "0" means the JIT optimised the loop away.

* With the JIT on, the shim is faster than or comparable to Cobalt everywhere except `string.pack`.
* With the JIT off, Lua-level `bit32` is 4x slower than Cobalt. As shipped (no `luaopen_jit`), CCLuaJIT is
  in exactly that state. Verified with `W\jitcheck\jitcheck.c`:
  * no `luaopen_jit` (as `main.cpp`): 0.879 s
  * `luaopen_jit`: 0.044 s
  * `luajit -joff`: 0.887 s

## 10. Recommendation

1. **Build flags:** `XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD"`.
2. **Patches** (`W\patches\`; apply and build with `W\build_luajit_cc.sh`; tested from a pristine tree):
   * `luajit-goto-contextual.patch` (lj_parse.c, 5 conditions): `goto` stays contextual under 52COMPAT.
   * `luajit-numfmt-roundtrip.patch` (lj_strfmt.h/lj_strfmt_num.c): `tostring` and concat use `%.15g`,
     falling back to `%.17g` (Cobalt), and print `-0` as `0`. Explicit `string.format("%.14g")` is unaffected.
   * `luajit-pattern-nul.patch` (lib_string.c): Lua 5.2-style `p_end` handling, so patterns may contain `\0`.
   * `luajit-no-bytecode.patch` (lj_load.c): no bytecode in any loader.
3. **Native libraries:**
   * Open: base (includes coroutine), string, table, math, **debug**, bit, and **jit** (to enable the
     compiler). Then remove `jit`/`jit.opt` from `_G` and `_LOADED`.
   * Do not open: io, os, package, ffi.
   * Remove natively: `dofile`, `loadfile`, `print`, `collectgarbage`, `gcinfo`, `newproxy`, `__inext`.
   * Replace `debug.sethook`/`gethook` with a virtualised version that cannot disable the machine's timeout hook.
4. **Prebios shim** `cclj_prebios.lua`: run once per machine before bios.lua. It is not a security boundary.
   It provides:
   * `_ENV` emulation;
   * a text-only `load`/`loadstring` with Cobalt messages, and the `string.dump` error;
   * `bit32`, `utf8` and `string.pack` with Cobalt-exact semantics (fuzz-verified);
   * `table.sort`, `ipairs`, `math.atan`/`math.log`, `tonumber`;
   * 52COMPAT fallbacks, aliases, `_VERSION`;
   * `_LOADED` sanitising.
5. **Move into C later, for speed and robustness:**
   * `string.pack`/`unpack`: port of Lua 5.3 lstrlib with Cobalt's long semantics.
   * Optionally `bit32` and `utf8`, if the JIT could ever be off (e.g. platforms without JIT support).
6. **Accept and document:**
   * `local _ENV`;
   * the column-based error snippets;
   * yields across `gsub`/`format`/reader functions;
   * `#` borders and `pairs` order;
   * the random sequence;
   * the `%q` and error-message cosmetics.

## 11. Effort for this lane

* The shim and the C patches already exist and are tested. Integrating them into the 1.21.1 fork is about
  0.5–1 day for an experienced developer: embed `cclj_prebios.lua` as a resource and run it before the BIOS,
  apply the patches in the native build, and set the flags.
* Native work:
  * Library selection, `jit` hiding and removing globals: about 0.5 day.
  * Virtualised `debug.sethook`: 1–2 days.
  * C `string.pack`/`unpack`: about 1 day.
* Not recommended: `_ENV` locals (parser work, more than 5 days) and column info for error snippets (more
  than 5 days).
* A modding newcomer should expect roughly 2–3x these times. The C-patch maintenance needs comfort with
  LuaJIT internals; the patches are small, but they must be re-applied on LuaJIT updates.

## 12. Artifacts

* **Shim:** `W\cclj_prebios.lua` (same as `W\harness\cclj_prebios.lua`); staged versions in `W\harness\stages\`.
* **Patches and build:**
  * `W\patches\luajit-goto-contextual.patch`
  * `W\patches\luajit-numfmt-roundtrip.patch`
  * `W\patches\luajit-pattern-nul.patch`
  * `W\patches\luajit-no-bytecode.patch`
  * `W\build_luajit_cc.sh`
  * Patch generators: `W\patch_lib_string_nul.py`, `W\patch_lj_load_nobc.py`
* **Builds:** `W\luajit-default-src`, `W\luajit-52`, `W\luajit-cc`, `W\luajit-cc-repro`.
* **Harness:**
  * `W\harness\cc_harness.lua`, `compat.lua`, `extra.lua`, `make_config.py`, `run.sh`
  * `configs\`
  * `out\` (all transcripts, e.g. `out\cobalt-cobalt.txt`, `out\shim-ljcc.txt`, `out\full-ljdef.txt`, `out\shim120-ljdef.txt`)
* **Cobalt runner:** `W\cobalt-runner\CobaltRun.java`.
* **Tests and fuzz:**
  * `W\t-*.lua`, `W\syntaxcheck.lua`, `W\diff3.sh`, `W\diffparse.py`, `W\diff2.py`
  * Outputs: `W\t-sem*.out`, `W\numfmt.*.txt`, `W\pf.*.txt`, `W\sf.*.txt`
* **JIT check:** `W\jitcheck\jitcheck.c`.
