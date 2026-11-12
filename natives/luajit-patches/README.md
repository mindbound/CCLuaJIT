# LuaJIT patch stack (build copy)

This directory is the canonical copy of CCLuaJIT's LuaJIT patches 01-08 and of the script that builds the patched LuaJIT. `natives/build-natives.sh` uses it. The patches apply on top of the `vendor/luajit` submodule (branch `cclj/v2.1` = upstream `14d8a7a2` + 17 upstream fixes).

For what each patch does, how it was tested, and why the base is `cclj/v2.1`, read:

- [`docs/port-1.21.1/artifacts/luajit-patches/README.md`](../../docs/port-1.21.1/artifacts/luajit-patches/README.md): the patch table, the build flags and the verification history;
- [`docs/port-1.21.1/artifacts/luajit-patches/BACKPORTS.md`](../../docs/port-1.21.1/artifacts/luajit-patches/BACKPORTS.md): the backport branch and the monthly upstream triage.

The copy in `docs/` is a historical record of the evaluation. Change the patches here, not there.

## Usage

```
natives/luajit-patches/build-patched-luajit.sh vendor/luajit <out-dir> [extra XCFLAGS]
```

The script exports `HEAD` of the submodule (or `LUAJIT_REV`) with `git archive`, applies `0*.patch` in order and runs `make BUILDMODE=static` with the recommended flags:

```
-DLUAJIT_ENABLE_INTERRUPT -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_DISABLE_FFI -DLUAJIT_DISABLE_BCLOAD
-DLUAJIT_ENABLE_CSTACKGUARD -DLUAJIT_DISABLE_PROFILE -DLUAJIT_DISABLE_VMEVENT
```

For a test build, pass `"-DLUA_USE_ASSERT -DLUA_USE_APICHECK"` as the third argument. The submodule itself is never modified.

## Differences from the `docs/` copy

`build-patched-luajit.sh` has these changes:

- `TARGET_CFLAGS` from the environment is passed to `make`. It applies to the target objects only, not to the host build tools (`minilua`, `buildvm`). `build-natives.sh` sets `TARGET_CFLAGS=-fPIC` on Linux, so that the static `libluajit.a` can be linked into `libcclj.so`. The draft in `docs/.../scaffold-multiloader/natives/build-natives.sh` passed `CFLAGS=-fPIC`, which also reaches the host tools.
- `git apply` runs with `GIT_CEILING_DIRECTORIES` set to the parent of `<out-dir>`, and the script fails if `git apply -v` reports `Skipped patch`. The output directory is now inside the CCLuaJIT working tree (`natives/build/`). There, `git apply` takes the paths of git-format patches (01, 07 and 08) as relative to the top of the CCLuaJIT repository, skips them as outside the current directory, and still exits 0. Seen on 2026-11-07: the docs version of the script reported 8 patches applied and built a LuaJIT without 01, 07 and 08. The other patches are plain `diff -u` output and did apply.
- `git archive` and `git apply` run with `core.autocrlf=false` and `core.eol=lf`. With the Git for Windows default `core.autocrlf=true` (a fresh CI clone; the local submodule happens to have `core.autocrlf=false`), `git archive` exports every LuaJIT source with CRLF line endings: `git -c core.autocrlf=true -C vendor/luajit archive HEAD` gives `src/Makefile: ASCII text, with CRLF line terminators`. Inside the CCLuaJIT working tree (`* text=auto`), a plain `git apply` of patch 03 also wrote `src/lj_load.c` with CRLF.
- `git apply` failures are fatal. The docs version fell back to GNU `patch`, which applies with fuzz and is missing on AlmaLinux 8. `patch` is now used only when `git` is not installed.
- It checks that `<luajit-git-tree>` contains `src/luajit_rolling.h` and exports with `git archive` only from the top of a git work tree. An uninitialized submodule is an empty directory inside the CCLuaJIT work tree. Before this check, git commands there silently used the CCLuaJIT repository, so the script exported CCLuaJIT and reported its commit as the LuaJIT commit.
- `git apply` runs with `LC_ALL=C`, so the `Skipped patch` check also works in other locales. The script refuses an empty, `/`, `.` or `..` `<out-dir>`, or the source tree itself, before it deletes and recreates it.
- It writes `<out-dir>/cclj-build.txt` with the exported commit, the flags and the applied patches. `build-natives.sh` and `tests/run-harnesses.sh` read the flags from it.

`05-numfmt-roundtrip.patch` rounds every exact decimal tie half up, `string.format` included (S2, D34 (c); [`docs/port-1.21.1/S2.md`](../../docs/port-1.21.1/S2.md) section 4.8). `nd_round` loses its round-to-even branch, so `tostring(391.786041259765625)` gives `391.78604125976563` and `string.format("%.0f", 2.5)` gives `3`, as on Cobalt. The `docs/` copy keeps upstream's `nd_round`, which gives `391.78604125976562` and `2` there, as C `printf` does. That branch is meant to round ties half to even, but it misses some: with it, `string.format("%.0e", 2.5e10)` gives `3e+10`, and `printf` gives `2e+10`. Patch 05 also turns the `%f` digit limit of `nd_div2k` back on (S2, D35, section 4.9). Upstream turned it off in 7152e154, so that `nd_round` could round to even, and `%f` of a number with about 560 or more fraction digits (every double below about 1e-171, and most below about 5e-153) has printed garbage since then: `string.format("%.0f", 1e-160)` gave `22772133350372314`. Half up reads no digit past the rounding digit, so the limit is safe again, and the text is `0`, as on Cobalt and with C `printf`. The patch was regenerated with `diff -u`, so its last hunk now names the line where that code starts in the `vendor/luajit` tree (305); the `docs/` copy names line 268, which `git apply` accepts with an offset.
