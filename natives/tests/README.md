# LuaJIT regression harnesses

These are the harnesses that CI runs against every patched LuaJIT build (PLAN.md §5.2, gate G0), promoted from `docs/port-1.21.1/artifacts/` into the build tree. `run-harnesses.sh` builds them against a LuaJIT tree from `natives/luajit-patches/build-patched-luajit.sh`, runs them, parses the counts and exits non-zero on any deviation.

This directory also holds the lint of the native sources (`lint-natives.sh`), the scan for `-Xcheck:jni` messages (`jni-scan.sh`) and Tier 0 of the machine bridge (`machine-test.sh` with `machine/machine_test.c`). They have their own sections at the end, followed by Tier 1 (`BridgeProbe`, a Java program in `common/src/test`) and a table of the CI steps that run them.

```
bash natives/build-natives.sh windows-x86_64          # or linux-x86_64; leaves natives/build/<platform-id>/luajit
bash natives/tests/run-harnesses.sh natives/build/windows-x86_64/luajit/src
```

It needs gcc (MinGW-w64 on Windows, run from Git Bash or an MSYS2 MINGW64 shell), `timeout` and Python 3.6 or newer (`python` on Windows, `python3` on Linux). Logs go to `natives/build/<platform-id>/harnesses/results/`. A run takes about 2 minutes on Windows (generating the sort-killer inputs takes 1 of them) and 30 seconds on Linux.

macOS is supported only for the informational CI job (`harness-informational` in `.github/workflows/build.yml`, in a manual run with the `macos` input), which builds the LuaJIT tree with `luajit-patches/build-patched-luajit.sh` (`MACOSX_DEPLOYMENT_TARGET` must be set). There, `gcc` is Apple clang, the cstack suite reports SKIP instead of running (`cstack.c` needs `pthread_getattr_np`), Python is not needed, and without coreutils `timeout` the script uses `gtimeout` or an equivalent in perl. The expected results in the table were measured on x86_64 Windows and Linux. In the informational job of the first CI run (PLAN.md §6), linux-aarch64 matched them for every suite except cstack (see the arm64 note below), and macos-aarch64 matched for every suite it runs.

CI (`.github/workflows/build.yml`) runs this script on windows-x86_64 and linux-x86_64 (required) and on linux-aarch64 (informational: a failing step never fails the workflow). On macos-aarch64 it runs only in a manual run with the `macos` input (the "Run workflow" button of `build` on the Actions tab, or `gh workflow run build.yml --ref 1.21.1_CCT -f macos=true`; the first one, run 42765675460, passed): GitHub cancelled that job for lack of a runner on two pushes in a row, and a cancelled job shows the whole run as failed.

| Suite | Windows | Linux | Expected |
|---|---|---|---|
| preempt | `preempt/harness.c` | `preempt/harness_posix.c` + `win32_on_posix.h` | 33 OK, and exactly `X_finalizer_loop` and `X_lua_hook_loop` hang (35 results) |
| patterns (patch 07) | `patterns/pharness.c` (modes error, pcall, hard, hardyield, pause) | `patterns/plinux.c` | Windows: 74 OK of 74. Linux: 5/5 interrupted, 5/5 usable afterwards |
| cstack (patch 08) | `cstack/cstack.c` via `cstack/run_cstack.py`, 1 MB stacks, 128 KB red zone | same | 31 completed, 0 crashed (see below for arm64) |
| compat | `lua/compat_check.lua` | same | output equals `lua/compat_check.expected` |
| syntax | `lua/syntax_probe.lua` | same | `SUMMARY ok=45 bad=0` |
| fixcheck | `lua/fixcheck.lua` with `-Ohotloop=1` | same | 6 PASS, 0 FAIL |

The two preemption hangs are expected: those cases need `newproxy` and `debug.sethook`, which the sandbox removes.

**arm64 cstack expectation.** Patch 08 checks the C stack in the interpreter's fast path of the stock `coroutine.resume`/`wrap` only on x64 with GC64, where `lj_arch.h` defines `LJ_HASCSTACKGUARD_FF`. Without that check, the 4 cstack cases that recurse only through the stock `coroutine.resume`/`wrap` overflow the native stack: `coroutine_wrap_nesting`, `coroutine_resume_nesting`, `coroutine_precreated_chain` and `stock_resume_stays_resumable`. (`mixed_chain` also uses `coroutine.wrap`, but its gsub and sort callbacks are guarded re-entries, so it completes.) The first CI run's `linux-aarch64` job (PLAN.md §6) gave exactly that: 27 completed, these 4 crashed with SIGSEGV. An x64 Linux build with the macro removed gives the same result.

The script reads the target and the macro from `lj_arch.h`, preprocessed with the build's XCFLAGS (`gcc -E -dM`, as LuaJIT's Makefile does to choose the VM's features):

- x64: the macro must be defined and the result must be 31 completed, 0 crashed. An x64 build without the check fails the suite.
- Linux arm64 without the macro: 27 completed, and exactly these 4 crashed with SIGSEGV. Any other crash, a timeout, another exit code, or a missing crash fails the suite.
- Any other target (Windows on arm64 included, which has not been measured), or arm64 with the macro: 31 completed, 0 crashed.
- If `lj_arch.h` cannot be preprocessed, the suite fails, and gcc's errors are in `results/lj_arch-dM.err`.

The mod will not depend on this fast path once the coroutine wrapper of DESIGN §3.3 (F1, planned for M1) is in place. It replaces the sandbox's `coroutine.resume`/`wrap` on every platform with a C `raw_resume` that goes through the guarded `lua_resume` and has its own nesting limit. The harness returns to 31 completed on arm64 only if 08's 4-instruction check is ported to `vm_arm64.dasc` and `lj_arch.h` defines `LJ_HASCSTACKGUARD_FF` for arm64 (PLAN.md WP B, optional).

The harnesses that read `G(L)->vmstate` include LuaJIT's internal `lj_obj.h`, so the runner compiles them with the same feature flags as the library (read from `cclj-build.txt` in the LuaJIT tree).

## Provenance

Every file in `preempt/`, `patterns/`, `cstack/` and `lua/` except `lua/compat_check.expected` is a verbatim copy, made on 2026-11-07 from commit `c48cace`. The git blob ids are identical to those of the sources. The sources in `docs/` stay as the historical record. Fix the harnesses here.

| File | Copied from (`docs/port-1.21.1/artifacts/...`) | Blob |
|---|---|---|
| `preempt/harness.c` | `harnesses/preempt/harness.c` | `2101d7e8` |
| `preempt/harness_posix.c` | `harnesses/preempt/harness_posix.c` | `b6725922` |
| `preempt/win32_on_posix.h` | `harnesses/preempt/win32_on_posix.h` | `9717e681` |
| `preempt/compat_intr.h` | `harnesses/preempt/compat_intr.h` | `8b7f7c6c` |
| `patterns/pharness.c` | `harnesses/patterns-cstack/harness/pharness.c` | `f1d8cfdd` |
| `patterns/plinux.c` | `harnesses/patterns-cstack/linux/plinux.c` | `ce832c8d` |
| `patterns/genkiller.lua` | `harnesses/patterns-cstack/lua/genkiller.lua` | `af580f9e` |
| `patterns/killer.lua` | `harnesses/patterns-cstack/lua/killer.lua` | `f16db889` |
| `cstack/cstack.c` | `harnesses/patterns-cstack/harness/cstack.c` (identical to `.../linux/cstack.c`) | `de4ef7ec` |
| `cstack/run_cstack.py` | `harnesses/patterns-cstack/harness/run_cstack.py` (already Python 3.6-compatible) | `b913ee3f` |
| `lua/compat_check.lua` | `lua/compat_check.lua` | `563dd2d9` |
| `lua/syntax_probe.lua` | `luajit-patches/tools/syntax_probe.lua` | `6551eb30` |
| `lua/fixcheck.lua` | `luajit-patches/tools/fixcheck.lua` | `e0cc5684` |

`run-harnesses.sh` replaces `luajit-patches/verify-patch-stack.sh` (Windows only) and `luajit-patches/tools/linux-sanity.sh` (Linux only). It uses the same case loops and timeouts, and adds:

- exact count checks and a non-zero exit code. The old scripts only printed counts. On AlmaLinux 8, `linux-sanity.sh` printed `0 completed, 0 crashed` for the C-stack harness and carried on;
- the exact names of the two expected preemption hangs;
- a target-dependent C-stack expectation: on Linux arm64 without patch 08's fast-path check, the exact names of the 4 expected SIGSEGV crashes (see the arm64 note above);
- a comparison of `compat_check.lua` with an expected file, where the old scripts printed the output for a human to read;
- `fixcheck.lua`.

The background for each harness is in `docs/port-1.21.1/reports/preempt-patch.md` and `docs/port-1.21.1/reports/proto-patterns-cstack.md`, and in `docs/port-1.21.1/artifacts/harnesses/patterns-cstack/README.md`.

## Lint: `lint-natives.sh`

```
bash natives/build-natives.sh windows-x86_64          # leaves the objects in natives/build/<platform-id>/obj
bash natives/tests/lint-natives.sh windows-x86_64
```

The lint reads `natives/src`, the objects and the library, and fails on any of these (M1.md §3.14):

| Check | Rule |
|---|---|
| Include graph | `machine.c` and `cclj_machine.h` do not include `jni.h`. `jni_machine.c` includes none of `lua.h`, `lauxlib.h`, `lualib.h`, `luajit.h`, `lj_*`. `cclj_machine.h` includes only `<stddef.h>` and `<stdint.h>`. Includes of headers in `natives/src` are followed. A computed `#include` fails. |
| Forbidden names | `FindClass`, `NewGlobalRef`, `NewWeakGlobalRef`, `NewStringUTF`, `GetStringUTFChars`, `Critical`, `ThrowNew`, `lua_pushlightuserdata`, `luaL_openlibs`, `lua_sethook`, `luaL_traceback`, in `natives/src/*.c` and `*.h` except `cclj.c`, and in any other file of `natives/src` that `machine.c`, `jni_machine.c` or `cclj_machine.h` includes (a `.inc`, for example). Comments and the contents of string and character literals are removed first. An identifier that contains a name fails, so `Critical` catches every `Get/Release*Critical`. `nm` also checks that `machine.o` and `jni_machine.o` reference none of the four Lua functions, whatever the spelling in the source (a macro, token pasting). |
| Writable statics | `nm` shows no `b`, `B`, `d`, `D`, `C`, `G`, `S`, `V` or `v` symbol in `machine.o` and `jni_machine.o` (`V` and `v` are weak data). Section symbols such as `.bss` are ignored: only names that `objdump -h` lists as sections of the object, so MinGW's `.weak.<name>.<default>` data symbol still fails. On Linux (`-fPIC`), a `static const char *const` table is a `d` symbol: its pointers need relocations, so it is in `.data.rel.ro`. Use a `char` array table or a `switch` instead. |
| Objects up to date | No file in `natives/src` is newer than the objects. |
| No `CCLJ_TEST` | No `cm_test_` symbol in the objects or the library, and no file of `natives/src` (the files of the forbidden-names check, and `cclj.c`) defines `CCLJ_TEST`. This applies to release and `CCLJ_DEBUG=1` builds. |

Exit 0: every check passed. 1: a check failed. 2: a usage error, or a source, object or library is missing.

## JNI warning scan: `jni-scan.sh`

```
set -o pipefail
java -Xcheck:jni ... 2>&1 | tee probe.log
bash natives/tests/jni-scan.sh probe.log
bash natives/tests/jni-scan.sh --self-test
```

`jni-warnings.txt` lists the messages of `-Xcheck:jni` that fail a run, one per line. The `jniScan` wrapper of the JavaExec tasks in `common/build.gradle` reads the same file (see "Through Gradle" below). `jni-scan.sh <log>...` matches each one as a fixed string, case-sensitive, and prints the number of matching lines and the first 100 of them in each log (a broken native method can print a warning on every JNI call, a million lines). Exit 0: no match. 1: a match. 2: a missing log or pattern file.

| Pattern | JDK 21 prints it to | JVM exit code |
|---|---|---|
| `WARNING in native method` | stdout | 0 |
| `FATAL ERROR in native method` | stdout | Windows 1, Linux 134 (SIGABRT) |
| `Warning: Calling other JNI functions in the scope of Get/Release` | stdout | 0 |
| `MXCSR changed by native JNI code` | stderr | Windows: a crash (`hs_err`), 1. Linux: 0 |

So the log must hold stdout and stderr, and the exit code must still be checked (F13): any non-zero exit fails the run, since FATAL ERROR gives 1 on Windows but 134 on Linux. Leaked local refs print nothing.

`--self-test` checks the scanner and the list against `jni-scan-fixtures/`: every `dirty-*.log` must fail the scan, every pattern must occur in a dirty fixture, and `clean.log` must pass. `clean.log` is ordinary JVM and Gradle output with near misses (`warning in native method` in lower case, `WARNING: A restricted method`, `-XX:-RestoreMXCSROnJNICall`). The message lines in the dirty fixtures, and the table above, come from a small JNI program that provokes each message, run on 2026-11-09 with Temurin 21.0.13 (Windows) and Red Hat OpenJDK 21.0.13 (AlmaLinux 8). The other lines and the stack frames are adapted to this project. Check the texts again on every JDK upgrade.

## Tier 0: `machine-test.sh`

```
bash natives/tests/machine-test.sh windows-x86_64     # or linux-x86_64 (linux-aarch64 on arm64): the host's platform id
```

Tier 0 tests the native machine core, `natives/src/machine.c`, without a JVM (M1.md §3.15, items 1-10): the allow-lists of the Lua state, loading, a fuzzer of the API table description, resume outcomes, error rendering and tracebacks, host round trips, a fuzzer of host replies, out-of-memory handling, re-entrancy, and threads and churn. `machine/machine_test.c` plays the host with its own codec of wire format v1, written from the format description, so the codec of `machine.c` is checked against an independent implementation too. It does not need `build-natives.sh` first, and it needs no JDK.

The script:

1. Builds an assert and API-check LuaJIT (`-DLUA_USE_ASSERT -DLUA_USE_APICHECK`) from `vendor/luajit` with the patches, into `natives/build/<platform-id>/luajit-assert`, with the log in `natives/build/<platform-id>/luajit-assert-build.log`. A tree built earlier is reused while the LuaJIT commit, the flags, the patches, the build script, the compiler and (on Linux) the C library stay the same.
2. Compiles `machine.c` and `machine/machine_test.c` with `-g -O1 -DCCLJ_TEST -DCCLJ_DEBUG -Wall -Wextra` and the xcflags of that tree, and links them with its `libluajit.a`. `CCLJ_TEST` compiles in the `cm_test_*` hooks of `machine.c` (such as `cm_test_fail_after(n)`, which makes a buffer growth fail), and `CCLJ_DEBUG` its stack-balance checks (`CM_CHECK`, which print one line and abort). Any compiler or linker warning fails the run.
3. Runs the program under `timeout`. It prints one line per check (`ok   <name>` or `FAIL <name>: <detail>`) and a summary; the script ends with `machine-test: OK` or `machine-test: FAILED`.

**Sanitizers.** On Linux the script first builds with `-fsanitize=address,undefined -fno-sanitize-recover=all` and runs that program with `--smoke` (one machine end to end):

- The sanitized build does not compile or link (no `libasan`, as on AlmaLinux 8 with its gcc 8.5): the script prints a note and the reason, and builds and runs the test without sanitizers.
- It builds, but the smoke run fails: the script fails and prints the sanitizer report. A script cannot tell a bug in `machine.c` from a problem of ASan with LuaJIT's GC64 mode (M1.md §6). Only when a person has found that the report is such a problem, and not a bug, rerun with `MACHINE_TEST_SANITIZE=0`.
- It builds and the smoke run passes: the whole test runs sanitized, and any report fails it.

ASan and UBSan with LuaJIT's GC64 mode are verified on Ubuntu 26.04 (gcc 15), so that is where Tier 0 runs sanitized. MinGW has no ASan: on Windows the script builds without sanitizers directly. The Linux CI job installs no `libasan`, so whether Tier 0 runs sanitized there depends on the gcc-toolset of the `manylinux_2_28` image; the log says which build ran.

| Variable | Default | Effect |
|---|---|---|
| `MACHINE_TEST_SANITIZE` | `1` | `0`: no sanitized build on Linux |
| `MACHINE_TEST_TIMEOUT` | `900` | Seconds the test program may run. A timeout fails the run. |
| `LUAJIT_SRC` | `vendor/luajit` | The LuaJIT git checkout to build |

It needs gcc (MinGW-w64 with `mingw32-make` on Windows, run from Git Bash or an MSYS2 MINGW64 shell; `make` on Linux), git, and `timeout` and `sha256sum` from coreutils. The platform id must match the host. Output goes to `natives/build/<platform-id>/machine-test/`: the program, its compiler output (`machine_test.log`, on Windows `machine_test.exe.log`; empty when clean), `smoke.log` (the sanitized smoke run) and `machine-test.log` (the run).

Exit 0: every check passed. 1: a check failed, the sanitized smoke run failed, or the test did not finish in time. 2: a usage error, a build failure or a compiler warning.

## Tier 1: `BridgeProbe`

`common/src/test/java/io/github/mindbound/cclj/natives/BridgeProbe.java` tests the JNI layer (`natives/src/jni_machine.c`) and the production `natives` Java package (`NativeLibrary`, `NativeMachine`, `Wire`) against a fake host, under `-Xcheck:jni` (M1.md §3.15). It uses only the JDK. It is compiled with plain `javac` and no classpath, together with every class of the `natives` package, so a class of that package that needs anything beyond the JDK fails the compile (M1.md F17).

| # | Check |
|---|---|
| 1 | Happy path, and the ABI check of `NativeLibrary` |
| 2 | The host throws `RuntimeException` and `new OutOfMemoryError("fake")`: the first line of the error text is fixed, and `nResume` leaves no exception pending |
| 3 | The host returns null |
| 4 | 10k seeded random replies |
| 5 | A re-entrant `resume` from the host gives `IllegalStateException`, which becomes a Lua error |
| 6 | `close()` from another thread while the host blocks on a latch: the free is deferred, `closedWhileRunning` is true, and `LIVE == 0` at the end |
| 7 | Create on thread A, resume on B, close on C |
| 8 | Local-ref leaks: one resume with 100k upcalls of 4 KiB each, under `-Xmx256m`. Inside the last upcall, after `System.gc()`, the used heap must be below the baseline + 32 MiB. `-Xcheck:jni` prints nothing for leaked local refs (F13), so this test catches them through the heap. |
| 9 | Prints the ns per call |

The class comment of `BridgeProbe` lists every check in detail. Beyond the table, it also checks the other exits of the host call (a JNI failure, provoked by filling the Java heap, and the local frames of the failing exits), that the host's exception is printed, that `NativeLibrary` loads once, and, in a second JVM, that a failed load stays failed. It prints one line per check (`ok   ...` or `FAIL ...`), the ns per call and a summary line. On stderr, the stack traces of the fake `RuntimeException` and `OutOfMemoryError`, and one long line of `Exception in thread "bridge-section"` prefixes, are expected (the probe prints a note before them). Exit 0: every check passed. 1: a check failed, or its watchdog stopped the run. 2: no library is bundled for this platform. 3: the library could not be loaded, or its ABI does not match. It loads the library from `natives/build/out` on the classpath and extracts it into the cache directory given as its argument. The second JVM is the same `java` with the same class path and `-Xmx64m`; its output goes into the probe's log.

Run it as CI does: stdout and stderr into one log, then the scan of that log. A non-zero exit code of `java` is a failure even when the scan passes; CI runs these lines under `set -eo pipefail`, so the step stops there. Run by hand, check the `java exit` line.

Windows (Git Bash or MSYS2 MINGW64; the classpath separator is `;`):

```
bash natives/build-natives.sh windows-x86_64
set -o pipefail
jdk=$(cygpath -u "$JAVA_HOME")/bin
out=natives/build/bridge-probe
rm -rf "$out"; mkdir -p "$out"
"$jdk/javac" -d "$out/classes" \
  common/src/main/java/io/github/mindbound/cclj/natives/*.java \
  common/src/test/java/io/github/mindbound/cclj/natives/NativeProbe.java \
  common/src/test/java/io/github/mindbound/cclj/natives/BridgeProbe.java
"$jdk/java" -Xcheck:jni -Xmx256m -cp "$out/classes;natives/build/out" \
  io.github.mindbound.cclj.natives.BridgeProbe "$out/cache" 2>&1 | tee "$out/bridge-probe.log"
echo "java exit: $?"
bash natives/tests/jni-scan.sh "$out/bridge-probe.log"
```

Linux (the classpath separator is `:`): build the library in AlmaLinux 8, at the glibc 2.28 floor, then run the probe there (`JAVA_HOME=/usr/lib/jvm/java-21-openjdk`, as in CI) or on a current distro, such as Ubuntu 26.04 (`JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64`). Under WSL both work on the Windows checkout (`/mnt/c/...`).

```
bash natives/build-natives.sh linux-x86_64            # in AlmaLinux 8
set -o pipefail
out=natives/build/bridge-probe
rm -rf "$out"; mkdir -p "$out"
"$JAVA_HOME/bin/javac" -d "$out/classes" \
  common/src/main/java/io/github/mindbound/cclj/natives/*.java \
  common/src/test/java/io/github/mindbound/cclj/natives/NativeProbe.java \
  common/src/test/java/io/github/mindbound/cclj/natives/BridgeProbe.java
"$JAVA_HOME/bin/java" -Xcheck:jni -Xmx256m -cp "$out/classes:natives/build/out" \
  io.github.mindbound.cclj.natives.BridgeProbe "$out/cache" 2>&1 | tee "$out/bridge-probe.log"
echo "java exit: $?"
bash natives/tests/jni-scan.sh "$out/bridge-probe.log"
```

The ns per call printed here includes the cost of `-Xcheck:jni`. The figure for G1 (M1.md §3.16, item 8) is measured without it: drop `-Xcheck:jni` from the `java` line, or run `./gradlew :common:bridgeProbe -PcheckJni=false`.

## Through Gradle

The Gradle tasks of `:common` run the same programs with the classes Gradle builds, and take the library from `natives/build/out` (build it first):

```
./gradlew :common:bridgeProbe                     # Tier 1, -Xcheck:jni -Xmx256m
./gradlew :common:bridgeProbe -PcheckJni=false    # the same without -Xcheck:jni, for the ns per call of G1
./gradlew :common:machineSpecs                    # Tier 2: LuaJITMachine, differential against Cobalt
```

`MachineSpecs` (Tier 2, `common/src/test/java/io/github/mindbound/cclj/headless/MachineSpecs.java`) runs `LuaJITMachine` against the CC:T machine contract (M1.md §3.15): 30 scenarios through a machine environment built by hand (23 of them also on Cobalt, which the results are compared with; 7 test what only LuaJIT does), and three through a real `Computer`. It prints one line per scenario (`ok` or `FAIL`), both machines' results, and a note for each allowed difference from Cobalt; the report goes to `common/build/headless/machineSpecs.tsv`.

Every JavaExec task of `:common` (`nativeProbe`, `bridgeProbe`, `machineSpecs`, `headlessSmoke`, `headlessSuite` and `fairnessProbe`) runs through the `jniScan` wrapper of `common/build.gradle`: stdout and stderr also go, whole lines at a time, to `common/build/headless/logs/<task>.log`, and the task fails when that log holds a line with a pattern of `jni-warnings.txt` (it prints the number and the first 100) or when the JVM exits non-zero.

## What CI runs

Both natives jobs of `.github/workflows/build.yml` run these steps in this order: `natives-linux-x86_64` in the `manylinux_2_28` container (bash), `natives-windows-x86_64` in MSYS2 MINGW64. A failing step stops its job. The logs are uploaded as `logs-natives-<platform-id>` whether the job passed or failed: the LuaJIT build logs, `harnesses/results/`, the `*.log` files of `machine-test/`, `natives/build/probe/native-probe.log`, `natives/build/bridge-probe/bridge-probe.log`, and the `hs_err_pid*.log` of a JVM crash.

| Step | Runs |
|---|---|
| Build libcclj | `bash natives/build-natives.sh <platform-id>` |
| Lint | `bash natives/tests/lint-natives.sh <platform-id>` |
| Tier 0 | `bash natives/tests/machine-test.sh <platform-id>`. On Windows without sanitizers; in the container sanitized only if the image's gcc-toolset can link `-fsanitize=address,undefined` (the job installs no `libasan`) |
| Scan self-test | `bash natives/tests/jni-scan.sh --self-test` |
| Harnesses | `bash natives/tests/run-harnesses.sh natives/build/<platform-id>/luajit/src` |
| NativeProbe | `javac` of `NativeLoader.java` and `NativeProbe.java`; `java -Xcheck:jni ... NativeProbe natives/build/probe/cache 2>&1 \| tee natives/build/probe/native-probe.log`; `jni-scan.sh` of that log |
| Tier 1 | `javac` of `natives/*.java`, `NativeProbe.java` and `BridgeProbe.java`; `java -Xcheck:jni -Xmx256m ... BridgeProbe natives/build/bridge-probe/cache 2>&1 \| tee natives/build/bridge-probe/bridge-probe.log`; `jni-scan.sh` of that log |

The probe steps run under `set -eo pipefail`, so the exit code of the pipeline is the one of `java`, not of `tee`: a FATAL ERROR of `-Xcheck:jni` exits 1 on Windows and 134 on Linux, and either fails the step before the scan.

Two more jobs run the Gradle tasks of "Through Gradle" with the library from the natives job's artifact, each task through the `jniScan` wrapper:

| Job | Runs | Reports |
|---|---|---|
| `mod` (ubuntu-24.04, after both natives jobs) | `./gradlew build`; then `:common:nativeProbe`; `--continue :common:bridgeProbe :common:machineSpecs`; `--continue :common:headlessSmoke :common:headlessSuite` (Cobalt) | `reports-mod`: `suite.tsv`, `machineSpecs.tsv`, `common/build/headless/logs/`, `hs_err_pid*.log` |
| `headless-windows-x86_64` (windows-2025, after the Windows natives job) | `./gradlew.bat --no-daemon --continue :common:nativeProbe :common:bridgeProbe :common:machineSpecs` in pwsh (`--no-daemon`: on Windows a running daemon would hold the Gradle cache's lock files when setup-gradle saves the cache) | `reports-headless-windows-x86_64`: `machineSpecs.tsv`, `common/build/headless/logs/`, `hs_err_pid*.log` |

In every job that uses `actions/setup-java`, a step removes its problem matcher `java` (`::remove-matcher owner=java::`). That matcher turns each `Exception in thread ...` line into an error annotation, and Tier 1 prints such lines on purpose, for its fake host failures; a green run would show red annotations. setup-java's `javac` matcher stays.
