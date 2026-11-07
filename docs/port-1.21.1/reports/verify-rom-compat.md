# verify-rom-compat: adversarial verification of the rom-compat findings

Verifier work dir: `<scratch>\work\verify-rom-compat` (called `V` below).
Investigator dir `...\work\rom-compat` was only read (the shim was copied to `V\shim-copy.lua`, SHA-1 identical, and its 4 patches were copied to `V\patches\`). Nothing was written to the repository.

## Summary of verdicts

| ID | Verdict | My severity | One-line result |
|---|---|---|---|
| RC-1 | confirmed (minor count nit) | critical (trivial fix) | Real bios.lua in a main.cpp-identical host dies on first resume with the exact `colours.lua:13` error on default, 52COMPAT and patched builds. 14 files (not 15) read `_ENV` as a value; bios.lua:463 only writes a field named `_ENV`. |
| RC-2 | confirmed | critical | Reproduced the staged chain: no debug -> `exception.lua:90`; no table.pack -> `parallel.lua:72` + "Press any key to continue"; bit32 alias missing 5 functions, signed, rounds; packRGB 8405247 vs 8339455; utf8/string.pack absent. |
| RC-3 | confirmed | critical | `debug.getupvalue(load, 8)` = `native_load` runs bytecode after the shim; the BCLOAD patch rejects load/loadstring/mode b/BOM-prefixed bytecode. |
| RC-4 | confirmed, and worse than claimed (now empirically verified) | critical | Global hooks: a pre-installed timeout hook is cleared/replaced by `debug.sethook`; main.cpp's one-shot abort hook is defeated by `while true do sh() end` (3/3 hung); a user hook function with an infinite loop defeats EVERY count-hook design (hooks suppressed while a hook runs). A "virtualised/chaining sethook" is therefore not a safe fix. |
| RC-5 | confirmed | high | NUL patterns: exact outputs reproduced; serialiseJSON fails inside the real ROM; own fuzz 5,649/20,000 differ, 112 after patch (all `%f`), patch is a no-op without NUL. |
| RC-6 | partially confirmed (rednet overstated) | high | %.14g confirmed; own fuzz 14,180/25,056 differ, 0 after patch. rednet does NOT serialise (rednet.lua:209 passes the table to modem.transmit). settings.set loses precision immediately (reserialize). |
| RC-7 | confirmed | medium | 7/8 identifier uses of `goto` fail to compile on 52COMPAT; default and patched builds accept all 8. Prevalence of the idiom not verified. |
| RC-9 | confirmed (core); perf sub-claim partially refuted | high | main.cpp-identical state: 0.991-1.008 s (= `-joff` 1.008 s) vs 0.039 s with luaopen_jit. Shim bit32 is 25x faster than Cobalt only for small values; 2.2-2.4x SLOWER for full 32-bit values (JIT trace aborts). |
| RC-10 | partially confirmed (overstated for main.cpp) | medium | Mechanism real, but main.cpp's `_LOADED` is only `_G,bit,coroutine,math,string,table` (+debug, +jit/jit.opt if opened); io/os/package come from luaL_openlibs, which main.cpp does not call. Missed: `registry._PRELOAD` still exposes `jit.util`/`jit.profile` after the shim. |

## 1. Tools I built (independent of the investigator's)

| Tool | Path | Notes |
|---|---|---|
| LuaJIT builds | `V\lj-def` (copy of the shared build), `V\lj-52` (`mingw32-make -j16 XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT`, log `V\build-52.log`), `V\lj-cc` (fresh copy + the investigator's 4 patches applied with `patch -p1`, built with `XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD"`, log `V\build-cc.log`) | I checked that the shared build's sources equal the repo's `vendor/luajit` submodule (19878ec0) for lj_parse.c, lib_string.c, lj_strfmt_num.c, lj_load.c, lib_jit.c, lj_dispatch.c, lib_debug.c, lj_lex.c (`cmp`, all identical). |
| Cobalt runner | `V\cob\VRun.java` (own code, javac 21, `java -cp "cobalt-0.9.9.jar;." VRun file.lua`) | `CoreLibraries.debugGlobals` + `Bit32Lib` (as `CobaltLuaMachine.java:79-80`), plus `__out`, `__clock`, Lua `print`. |
| main.cpp-identical host | `V\host\vhost.c` -> `V\host\{lj-def,lj-52,lj-cc}\vhost.exe` | Opens libraries exactly like `main.cpp:707-735` (direct `luaopen_base/math/string/table/bit`, `bit32 = bit`, `__inext` copied verbatim, `_VERSION`, nil'd collectgarbage/gcinfo/newproxy), optional `debug`/`jit`. Modes: `run`, `bench`, `hook` (timeout-hook designs), `boot` (loads bios.lua into `lua_newthread` with `luaL_loadbuffer(...,"@bios.lua")` and drives it with `lua_resume`, dropping filtered events like `CobaltLuaMachine.handleEvent:120`). |
| Boot mock | `V\boot\mock.lua` + `V\boot\manifest.lua` (generated from the extracted 1.21.1 ROM) | Own minimal fs/term(51x19)/os/peripheral/redstone mocks; scripted `paste`+`key 257` input. |
| Minimal shims (mine) | `V\boot\pre-env.lua` (`_G._ENV=_G` + load wrapper that rawsets `env._ENV`), `V\boot\pre-pack.lua` (table.pack/unpack) | Deliberately minimal, not the investigator's 825-line shim. |
| Probes / fuzz | `V\t-micro.lua`, `V\t-goto.lua`, `V\t-reg.lua`, `V\t-shimsec.lua`, `V\t-numfuzz.lua` (25,056 values, own generator), `V\t-patfuzz2.lua` (2 x 20,000 cases, own generator), `V\t-bitbench2.lua` | Outputs next to them (`*.cobalt.txt`, `*.lj-*.txt`, `pf2.*`, `numfuzz.*`, `bench-results.txt`, `hook\results.txt`, `boot\out-*.txt`). |

## 2. RC-1: `_ENV` as a value (confirmed)

Static: `grep -rn _ENV` over the 1.21.1 ROM (excluding rom/help):
* Value reads (14 files): `rom/apis/colours.lua:11`, `command/commands.lua:54`, `fs.lua:12`, `keys.lua:137`, `term.lua:18`, `textutils.lua:807`, `turtle/turtle.lua:32`, `cc/internal/edit_runner.lua:23`, `cc/require.lua:19`, `programs/edit.lua:181`, `http/pastebin.lua:151`, `http/wget.lua:65`, `programs/lua.lua:26`, `programs/shell.lua:70`.
* `bios.lua:463` is `tEnv._ENV = tEnv` (a field write inside `os.run` under `bios.strict_globals`) and `bios.lua:515` compares a key string. Neither reads `_ENV`; both work on LuaJIT. So "15 files" is really 14 + bios's harmless field write. This does not change the conclusion.
* `os.loadAPI` (bios.lua:496-523) does **not** set `tEnv._ENV`, so `local colours = _ENV` reads `_G._ENV` = nil.

Dynamic (`V\boot\out-A-*.txt`, `out-A2-*.txt`):
```
vhost boot none      mock-run.lua bios.lua     (lj-def, lj-52)
vhost boot debug,jit mock-run.lua bios.lua     (lj-def, lj-52, lj-cc)
!!!! BIOS COROUTINE ERRORED (status 2) after 0 events / 1 resumes: bios.lua:561: Failed to load API colours due to /rom/apis/colours.lua:13: attempt to index local 'colours' (a nil value)
```
Blank screen, 0 events. Nuance: in real CC:T, `ComputerExecutor.resumeMachine` (ComputerExecutor.java:553-556) calls `displayFailure("Error running computer", msg)` and shuts down, if the port returns `MachineResult.error`. So the user would see that message, not a blank screen. The fix is tiny: my 8-line `pre-env.lua` removes this failure.

## 3. RC-2: native library set (confirmed)

`main.cpp:707-714` (read via `git show HEAD:src/main/cpp/main.cpp`) opens base, math, string, table, bit; then `bit32 = bit`; no `luaopen_debug`, no `luaopen_jit`.

Staged boots with my own minimal shims (`V\boot\out-*.txt`):

| Run | lj-def | lj-52 |
|---|---|---|
| B: main.cpp libs + `_ENV` | `bios.lua:561: Failed to load API parallel due to /rom/modules/main/cc/internal/exception.lua:90: attempt to index local 'debug' (a nil value)` | same |
| C: + debug | screen: `/rom/apis/parallel.lua:72: attempt to call field 'pack' (a nil value)` / `Press any key to continue` | **boots**; `id`, `ls rom`, `shutdown` work |
| D: + table.pack | boots; `id`, `ls rom`, `shutdown` work | — |

So the minimum needed to reach the shell is `_ENV` emulation + `debug` + table.pack (or 52COMPAT).

REPL probes inside the real ROM (`out-D2-lj-def.txt`, `out-D2-lj-52.txt`): `textutils.serialiseJSON("a")` -> `textutils.lua:448: malformed pattern (missing ']')`; `textutils.serialise(0.1+0.2)` -> `"0.3"`; `colors.packRGB(0.5,0.25,1)` -> `8405247`; `bit32.extract` -> nil.

Micro probes (`V\t-micro.*.txt`, LuaJIT with `bit32=bit` like main.cpp):

| Probe | Cobalt 0.9.9 | LuaJIT def/52 |
|---|---|---|
| bit32.extract/replace/btest/lrotate/rrotate | functions | nil |
| bit32.bnot(0) | 4294967295 | -1 |
| bit32.band(127.5,255) | 127 | 128 |
| bit32.lshift(1,31) | 2147483648 | -2147483648 |
| packRGB(0.5,0.25,1) | 8339455 | 8405247 |
| table.pack, utf8, string.pack, rawlen | all present | def: none; 52: table.pack/unpack, rawlen |

Also, a CRC32 written with bit32 gives `-933153386` with the alias versus `3361813910` on Cobalt (`V\bench-results.txt`). Signed results silently break checksums.

ROM uses: `textutils.lua:432` (utf8.codes), `cc/internal/json.lua:63`, `snbt.lua:63,67,71` (utf8.char), `programs/fun/speaker.lua:75,77,103` (`("<I4"):unpack`). Confirmed by grep.

## 4. RC-3: bytecode (confirmed)

`V\t-shimsec.lua`, run through `vhost run debug,jit` and `vhost run debug` with the copied shim. The bytecode was made with the pre-shim `string.dump`; it stands in for bytecode crafted offline.
```
after shim: load(bc) -> attempt to load a binary chunk
after shim: loadstring(bc) -> attempt to load a binary chunk
bypass: debug.getupvalue(load, 8) = native_load -> loads bytecode -> BYTECODE RAN
```
The patched build `lj-cc` gives:
* `load`/`loadstring`/`load(bc,'x','b')`/`load(bc,'x','bt')` -> `attempt to load a binary chunk`.
* BOM + bytecode -> `cannot load malformed bytecode`.
* Text chunks still load.

On `lj-def` both `load(bc)()` and `loadstring(bc)()` print `BYTECODE RAN`. The patch hooks `cpparser` in `lj_load.c` right after `lj_lex_setup`, which every `lua_load` path goes through.

Note: a C-level `load` replacement that keeps the native `load` as a C upvalue is just as reachable, because LuaJIT's `debug.getupvalue` returns C-closure upvalues too. A C fix must call `luaL_loadbufferx(...,"t")` directly, or patch `lj_load.c` as the investigator did.

## 5. RC-4: debug.sethook vs the timeout hook (confirmed empirically, worse than claimed)

Source:
* `lua_sethook` writes `g->hookf/hookcount/hookmask` of the global_State (`lj_dispatch.c:329-340`), so hooks are global to all coroutines.
* `callhook` skips every hook while `hook_active(g)` (`lj_dispatch.c:362`, `HOOK_ACTIVE` `lj_obj.h:678-679`).

Cobalt is different: hooks are per-thread `DebugState`, and the interrupt is checked on every instruction regardless of hooks (`LuaInterpreter.java:177`).

Experiment: `vhost hook <flags> <design> file`.
* `abort` = main.cpp's `abortMachine`: `lua_sethook(main_routine, thread_interrupt_hook, LUA_MASKCOUNT, 1)` is set from another OS thread 300 ms after start. `thread_interrupt_hook` is a verbatim copy of main.cpp:182-187.
* `abortrep` re-arms the hook every 20 ms (or 1 ms).
* `pre` installs a periodic count hook (every 1000 instructions) before the run; it checks an abort flag.
* A watchdog reports HUNG at 3 s, or 5-10 s for the long runs.

Results are in `V\hook\results.txt`. JIT off, as main.cpp ships:

| User code | pre | abort (main.cpp) | abortrep 20 ms |
|---|---|---|---|
| h1 `while true do end` | aborted | aborted | aborted |
| h2 `debug.sethook() while true do end` | **HUNG** | aborted | aborted |
| h3 `local sh=debug.sethook while true do sh() end` | **HUNG** | **HUNG** (3/3 runs, 5 s) | aborted after 0.25-1.9 s (3/3, 10 s cap); 1 ms re-arm: 0.2-0.6 s |
| h4 `debug.sethook(function() while true do end end,"",1)` | **HUNG** | **HUNG** | **HUNG** (also with 1 ms re-arm for 10 s) |
| h5 replace hook, then loop | **HUNG** | aborted | aborted |
| h7 user hook (count 1000) with infinite loop, then loop | — | — | **HUNG** (1 ms re-arm, 5 s) |

With `luaopen_jit` every variant hangs except h3 with re-arming. This matches the already-established "count hooks do not fire in JIT traces".

Conclusions:
* Exposing LuaJIT's native `debug.sethook` lets user code permanently defeat the timeout. That hangs a native server worker thread at 100 % CPU, which CC:T cannot kill.
* Even the investigator's recommended "virtualised/chaining sethook" is unsafe if it runs user hook functions inside LuaJIT's hook context: an infinite loop in the user hook (h4/h7) can never be interrupted by any count hook.
* Safe options: do not expose `debug.sethook` at all (the ROM never uses it, verified by grep), or run user hooks outside the hook context, or use a non-hook interrupt (VM patch).
* With the investigator's shim, `debug.sethook` only has `error` as an upvalue (verified), so the native function is not reachable through it.
* Separate data-race note: `abortMachine` does a non-atomic read-modify-write of `g->hookmask` from a foreign thread, concurrently with `hook_enter`/`hook_leave` on the Lua thread.

## 6. RC-5: NUL in patterns (confirmed)

`lib_string.c:189-207` (`classend` errors at `*p == '\0'`) and `do_match` treat NUL as the end of the pattern.

Probes, Cobalt vs LuaJIT def/52 (`V\t-micro.*.txt`):
* `("a\0a\0"):gsub("\0","0")`: `"a0a0",2` vs `"0a0\0000a0\0000",5`.
* `("a\0b"):gsub("[\0-\31]","?")`: `"a?b",1` vs `malformed pattern (missing ']')`.
* `("a\0b"):match("a\0c")`: nil vs `"a"`.
* `find("\0")` with no special characters works on both, because of the plain-search shortcut.

Own fuzz (`V\t-patfuzz2.lua`, results `V\pf2.*`):

| | def vs Cobalt | patched vs Cobalt | patched vs def |
|---|---|---|---|
| with NUL | 5,649 / 20,000 | 112 (all involve `%f`) | 5,554 |
| without NUL | 99 (all `%f`) | 99 (all `%f`) | 0 |

The patch fixes NUL handling and changes nothing else. The `%f` residue is a Cobalt frontier-at-end-of-subject difference that exists independently of NUL.

## 7. RC-6: number formatting (partially confirmed)

`luaconf.h:123` `LUA_NUMBER_FMT "%.14g"`; `lj_strfmt_num.c:590` uses `STRFMT_G14`. Cobalt vs LuaJIT:
* `1/3`: `0.33333333333333331` vs `0.33333333333333`
* `0.1+0.2`: `0.30000000000000004` vs `0.3`
* `2^53-1`: `9007199254740991` vs `9.007199254741e+15`
* `123456789012345`: `123456789012345` vs `1.2345678901235e+14`
* `-0`: `0` vs `-0`

Own fuzz `V\t-numfuzz.lua` (25,056 values, `tostring` and `..`): def and 52 each differ on 14,180 values; patched `lj-cc` on 0.

ROM paths:
* `textutils.lua:370` (serialise) and `:532` (JSON) use `tostring`.
* `settings.set` calls `reserialize` = serialize then unserialize (`settings.lua:44-46,140`), so `settings.set("x", 0.1+0.2)` stores 0.3 **in memory immediately**, not only on save.
* Refuted part: **rednet does not serialise**. `rednet.lua:209` passes the message table straight to `modem.transmit`, so rednet payloads are unaffected unless user code serialises them itself.

## 8. RC-7: goto under 52COMPAT (confirmed)

`lj_parse.c:1036`: `if (ls->tok != TK_name && (LJ_52 || ls->tok != TK_goto))`.

`V\t-goto.lua` has 8 cases:
* Cobalt, lj-def, lj-cc: 8/8 pass.
* lj-52: 7/8 fail (`'<name>' expected near 'goto'`, `unexpected symbol near 'goto'`); only the real `goto` statement works.
* PUC 5.4 also rejects them, which shows Cobalt is deliberately lax here.

The ROM is unaffected. I did not verify the claim that this is a "common turtle idiom" (no corpus). Medium severity: it only matters if 52COMPAT is chosen, and a tested patch exists.

## 9. RC-9: no luaopen_jit (confirmed core claim; perf sub-claim partially refuted)

Source: `JIT_F_ON` is set only in `lib_jit.c:715` (`jit_init`, called by `luaopen_jit`) or by `luaJIT_setmode`; main.cpp calls neither.

`vhost bench` with main.cpp's exact lib set, 1e8 calls of a `bit.band` wrapper:

| Configuration | Time |
|---|---|
| no `luaopen_jit` | 0.991-1.008 s |
| `luajit -joff` | 1.008 s |
| with `luaopen_jit` | 0.039 s (about 26x faster) |
| `luajit` (JIT on) | 0.040 s |

Shim bit32 performance (`V\t-bitbench2.lua`, `V\bench-results.txt`, best of 5/15):

| Workload | Cobalt | shim, JIT on | shim, -joff |
|---|---|---|---|
| (a) investigator's small-value loop x3M | 171-177 ms | **7 ms** | 811-813 ms |
| (b) full 32-bit values x1M | 157-175 ms | **383-388 ms (2.2-2.4x slower)** | 543-564 ms |
| (c) CRC32 over 64 KB | 10.7-22.2 ms | 19 ms (parity to 1.8x slower) | 30 ms |

`luajit -jv` on (b) shows repeated `loop unroll limit reached at shim-copy.lua:285`. That is the `for i = 1, select("#", ...)` loop in the shim's `bit32.band`/`bxor`, so the traces abort. The "28x faster than Cobalt with JIT" result holds only for small values. Typical bit32 code (hashes, CRCs, RNGs, colours with full 32-bit values) is as fast or slower than Cobalt. A C `bit32`, or specialised 2-argument fast paths, is needed and should not be treated as optional.

## 10. RC-10: registry._LOADED leak (partially confirmed)

The `cc/require.lua:134-140` copy loop is confirmed.

Actual `_LOADED` (`V\t-reg.lua`):

| State | `_LOADED` |
|---|---|
| main.cpp libs | (no debug, so cc.require cannot even run) |
| main.cpp libs + debug | `_G,bit,coroutine,debug,math,string,table` |
| + jit | adds `jit,jit.opt` |
| stock luajit.exe | `_G,bit,coroutine,debug,io,jit,jit.opt,math,os,package,string,table` |
| Cobalt | `bit32,coroutine,debug,math,string,table,utf8` |

So io/os/package leak only if they are opened natively, and main.cpp does not open them. The claim describes stock LuaJIT, not CCLuaJIT. The real exposure is `debug.getregistry()` itself, which any program can call; cc.require only makes it convenient.

`dofile`, `loadfile`, `print` and `__inext` are confirmed as globals in the main.cpp state. bios.lua redefines `print` (bios.lua:121), `loadfile` (423) and `dofile` (441) before any user code runs, so the practical risk is low. `__inext` stays visible to user programs.

**Missed:** after the shim's `_LOADED` sanitising and the removal of the `jit` global, `debug.getregistry()._PRELOAD` still holds `jit.profile,jit.util,string.buffer,table.clear,table.new` (with luaopen_jit). Then `_PRELOAD["jit.util"]("jit.util").ircalladdr(0)` returns `0x7ffd1b8a9020`, a lua51.dll code address (ASLR leak), and `jit.profile` (`start,stop,dumpstack`) is callable. Fix: clear `_PRELOAD` (and `_VMEVENTS`) natively.

## 11. Other issues the investigator missed

1. **User hook functions are uninterruptible** (RC-4 h4/h7): LuaJIT never calls a hook while another hook is active. This defeats any count-hook timeout, including the recommended "virtualised/chaining sethook". It is critical if any form of user hook is offered.
2. **main.cpp's own one-shot abort design is defeated without clearing the hook in advance**: a tight `debug.sethook()` loop hung 3/3 runs.
3. **`_PRELOAD` exposure** (jit.util address leak, jit.profile) survives the shim. See §10.
4. **Shim bit32 slower than Cobalt for full-range values** even with the JIT on, because of trace aborts. See §9.
5. Minor: 14 files, not 15, read `_ENV`; a real port would show "Error running computer" rather than a blank screen.
6. Cross-lane pointer, not verified: main.cpp's `luaL_newmetatable("JavaFN"/"jobject_ref")` metatables have no `__metatable` guard. Once `registerAPI` is restored, user code could call `getmetatable(ud).__gc(ud)` manually; check this in the code-audit lane.

## 12. Reproduction commands (Git Bash; V = verifier dir)

```
cd V/lj-52/src && mingw32-make -j16 XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT
cd V/lj-cc && for p in ../patches/*.patch; do patch -p1 < $p; done && cd src && mingw32-make -j16 XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD"
gcc -O2 -o V/host/lj-def/vhost.exe V/host/vhost.c -IV/lj-def/src V/lj-def/src/lua51.dll   (same for lj-52, lj-cc)
cd V/cob && javac -cp cobalt-0.9.9.jar VRun.java && java -cp "cobalt-0.9.9.jar;." VRun ../t-micro.lua
V/host/lj-def/vhost.exe boot none mock-run.lua <rom>/bios.lua                 # RC-1
V/host/lj-def/vhost.exe boot none mock-run.lua <rom>/bios.lua pre-env.lua     # RC-2 debug
V/host/lj-def/vhost.exe boot debug mock-run.lua <rom>/bios.lua pre-env.lua    # RC-2 table.pack
V/host/lj-def/vhost.exe hook debug abort V/hook/h3-clear-in-loop.lua          # RC-4
VH_REP_MS=1 VH_KILL_MS=10000 V/host/lj-def/vhost.exe hook debug abortrep V/hook/h4-hook-loop.lua
V/host/lj-def/vhost.exe bench none 100000000 ; ... bench jit 100000000        # RC-9
V/host/lj-def/vhost.exe run debug,jit V/t-shimsec-run.lua                      # RC-3, RC-10, _PRELOAD
```
