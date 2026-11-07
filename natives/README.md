# libcclj: the native library

`libcclj` is one self-contained JNI library per platform: the JNI glue in `src/` (plain C) with the patched LuaJIT linked in statically. See DESIGN.md §2 (`docs/port-1.21.1/DESIGN.md`). For M0 it only contains the self-test that `./gradlew :common:nativeProbe` runs. The machine bridge comes later.

| Path | What |
|---|---|
| `build-natives.sh <platform-id>` | Builds the patched LuaJIT from `vendor/luajit`, then `libcclj`, then checks its imports and exports |
| `src/cclj.c` | JNI glue. M0: `NativeProbe.selfTest()` |
| `luajit-patches/` | LuaJIT patches 01-08 and `build-patched-luajit.sh` (canonical copy) |
| `tests/` | LuaJIT regression harnesses and `run-harnesses.sh` |
| `build/` | Build output, ignored by git |

## Building

| Platform id | Host | Library | Status |
|---|---|---|---|
| `windows-x86_64` | Git Bash with MinGW-w64 gcc and `mingw32-make` | `cclj.dll` | required |
| `linux-x86_64` | `manylinux_2_28` container, or AlmaLinux 8 (glibc 2.28) | `libcclj.so` | required |
| `linux-aarch64` | the same on arm64 | `libcclj.so` | informational |

macOS is not supported yet. Platforms without a library fall back to Cobalt at runtime.

```
bash natives/build-natives.sh windows-x86_64
./gradlew :common:nativeProbe                                        # load it through NativeLoader, run the self-test
bash natives/tests/run-harnesses.sh natives/build/windows-x86_64/luajit/src
./gradlew build                                                      # the mod jar now contains natives/windows-x86_64/cclj.dll
```

`build-natives.sh` needs `JAVA_HOME` (a JDK, for `jni.h`); without it, the JDK is found from `javac` on `PATH`. The library goes to `natives/build/out/natives/<platform-id>/`. `:common` uses `natives/build/out` as a resource directory, so the mod jar carries whatever has been built there. A jar built without natives is valid: computers then run on Cobalt.

The script fails unless the library has only the expected dependencies:

- Windows: imports only `KERNEL32.dll` and the C runtime (`msvcrt.dll`, or UCRT). Linking uses `-static -static-libgcc`.
- Linux: `NEEDED` only libc, libm, libdl, libpthread (and the dynamic loader), and no `GLIBC_` symbol version above 2.28 (`GLIBC_MAX`). Linking uses `-static-libgcc`, the stack is not executable, and LuaJIT's symbols are hidden (`-Wl,--exclude-libs,ALL`).
- Both: only `Java_*` symbols are exported.

A library that fails the check is deleted, so Gradle cannot package it.

The Windows DLL is linked without a timestamp and with a fixed preferred base address, so the same sources and toolchain give the same bytes (the Linux library already did).

CI (`.github/workflows/build.yml`) builds `linux-x86_64` in the `quay.io/pypa/manylinux_2_28_x86_64` container and `windows-x86_64` with MSYS2 MINGW64, runs `tests/run-harnesses.sh` and the `NativeProbe` self-test on both, and passes the libraries to the `mod` job as the artifacts `natives-linux-x86_64` and `natives-windows-x86_64`. That job puts both into `natives/build/out` and builds the mod jar.

Build Linux natives in AlmaLinux 8 or `manylinux_2_28`, not on a current distro: a library built against a new glibc does not load on older servers (`docs/port-1.21.1/reports/port-mechanics.md`). The `manylinux_2_28` CI image uses a `gcc-toolset` compiler; AlmaLinux 8's own gcc 8.5 also works.

## Licence notices

`build-natives.sh` puts the licence notices of the code linked into each library next to it, so they travel with it into the CI artifacts and the mod jar (`natives/<platform-id>/*.txt`):

| File | Platforms | Source |
|---|---|---|
| `LuaJIT-COPYRIGHT.txt` | all | `COPYRIGHT` of the LuaJIT tree that was built: LuaJIT and the Lua 5.1/5.2 code it contains (MIT), and dlmalloc (public domain) |
| `MinGW-w64-runtime.txt` | `windows-x86_64` | [`LICENSES/MinGW-w64-runtime.txt`](../LICENSES/MinGW-w64-runtime.txt) |

`-static` links parts of the mingw-w64 runtime into `cclj.dll`: startup and TLS support from `libmingw32`, and from `libmingwex` the `gdtoa` number conversion (David M. Gay / Lucent, whose licence requires its notice), the `printf`/`scanf` family and math functions (seen in an `-Wl,-Map` link map). `LICENSES/MinGW-w64-runtime.txt` is upstream's `COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt` (mingw-w64 git, blob `776d5f10`, last changed in `96f1cb7d` on 2025-10-24). It collects the notices of the whole runtime. The Linux library links only libgcc statically, which the GCC Runtime Library Exception covers without a notice.

`LICENSES/LuaJIT-COPYRIGHT.txt` is a copy of `vendor/luajit/COPYRIGHT` for the patches in this repository, which contain LuaJIT code. `build-natives.sh` warns when the submodule's copy has changed and this one has not.
