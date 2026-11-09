# G1 evidence: S1 on commit 5381504

Gate G1 (M1.md §3.16) asks for eight things on one commit. This report gives the evidence for each, on commit 5381504 ("Run Tier 1 and Tier 2 through Gradle in CI on both OSes", the last commit of S1 steps 1–5):
- CI run [42593991784](https://github.com/mindbound/CCLuaJIT/actions/runs/42593991784), 2026-11-09: all six jobs green (`gh run view 42593991784 --json jobs` gives `success` as the conclusion of each). This report uses the logs of four of them; the other two are the informational arm64 harness jobs, which G1 does not use;
- local runs on 2026-11-09 from a clean working tree at the same commit;
- the logs of both: [`artifacts/g1/`](../artifacts/g1/) (the CI job logs and the job list in `ci/`, the full log of each local step in `logs/`, and each phase's script and output in `runs/`).

Item 7, your sign-off of the review, is still open. The review guide for it is [s1-review-guide.md](s1-review-guide.md).

## Summary

| # | G1 requires | Result |
|---|---|---|
| 1 | Tier 0 passes on Windows x64 and Linux x64 (Alma 8 / manylinux_2_28) | Met. 166 of 166 in both natives jobs, on Windows 11 and on AlmaLinux 8. Also 165 of 165 under ASan and UBSan on Ubuntu 26.04. |
| 2 | Tier 1 passes with zero scan hits: both natives jobs, the mod job, Win11, Ubuntu 26.04 with the Alma 8 library | Met. 88 of 88 and a clean scan in all seven places of §2: the five that G1 names, AlmaLinux 8, and the Windows Gradle job. |
| 3 | Tier 2 passes with zero scan hits on Linux CI and on Windows | Met. 33 of 33 and a clean scan in the mod job and the Windows Gradle job, and locally on Windows 11, AlmaLinux 8 and Ubuntu 26.04. |
| 4 | `jni-scan.sh --self-test` passes; the negative control makes `bridgeProbe` fail through the scan | Met. The self-test passes in both natives jobs and locally on Windows and Alma 8. The negative control fails through the scan on Windows (Gradle) and on Alma 8 (by hand, with the natives job's commands). |
| 5 | No G0 regression: `nativeProbe`, Cobalt `headlessSmoke` and `headlessSuite` at 1390/1407, all scan-clean | Met. In the mod job and locally on Windows 11. The harnesses of both natives jobs pass too. |
| 6 | The same scenarios pass against a `CCLJ_DEBUG=1` build on both OSes | Met. Tier 1 and Tier 2 pass with the debug library on Windows 11, AlmaLinux 8 and Ubuntu 26.04, all scan-clean. Tier 0 always runs as a debug build (§1), so item 1 covers it. |
| 7 | You sign off the review in §3.17 | Open. |
| 8 | ns per call from Tier 1 without `-Xcheck:jni` is recorded for D1 | Met. 202–213 ns on Windows 11 and Alma 8, 191–194 ns on Ubuntu 26.04: below D1's 300 ns, so the `byte[]` transport stays. These figures are for this PC; see "Not covered". |

## Environments

| Where | OS and compiler | JDK | Library (SHA-256, first 16 hex digits) |
|---|---|---|---|
| CI `natives-linux-x86_64` | `manylinux_2_28` container, gcc 14.2.1 (gcc-toolset) | Red Hat OpenJDK 21 (`/usr/lib/jvm/java-21-openjdk`) | `291d1cafba225ce9` |
| CI `natives-windows-x86_64` | windows-2025, MSYS2 MINGW64, gcc 16.2.0 (Rev4) | Temurin 21.0.13+9 | `94c7c5149bfec68b` |
| CI `mod` | ubuntu-24.04, with the library of the Linux natives job | Temurin 21.0.13+9 | `291d1cafba225ce9` |
| CI `headless-windows-x86_64` | windows-2025, with the library of the Windows natives job | Temurin 21.0.13+9 | `94c7c5149bfec68b` |
| Windows 11 | MinGW-w64 gcc 16.2.0 (x86_64-msvcrt-posix-seh, Brecht Sanders r1), Git Bash | Temurin 21.0.13+9 | `44f151d9dfe06271` (release) |
| WSL AlmaLinux 8.10 | gcc 8.5.0, glibc 2.28 | Red Hat OpenJDK 21.0.13 | `004c09267fd7ee1d` (release) |
| WSL Ubuntu 26.04.1 | gcc 15.2.0, glibc 2.43; Tier 1 and Tier 2 run the library that Alma 8 built | OpenJDK 21.0.12.1 | `004c09267fd7ee1d` (release) |

The CI libraries differ from the local ones because the compilers differ. The hash in each row is the library that row's Tier 1 and Tier 2 runs used.

The `headless-windows-x86_64` job ran for the first time in this run, with an empty Gradle cache ("Basic caching did not find an entry to restore"): 61 s by GitHub's job times (09:37:47 to 09:38:48 UTC), 40 s of it in the Gradle step, and its post step saved the cache.

## 1. Tier 0

Tier 0 does not use the library or the LuaJIT that `build-natives.sh` builds. `machine-test.sh` compiles `machine.c` with `-DCCLJ_TEST -DCCLJ_DEBUG` and links it with its own assert build of LuaJIT (`-DLUA_USE_ASSERT -DLUA_USE_APICHECK`, in `natives/build/<platform-id>/luajit-assert`). It reuses that tree only when its stamp (the LuaJIT commit, the flags, the patches and the build script, the compiler, and on Linux the C library) matches, so Ubuntu built its own. So every Tier 0 run is a debug build.

| Where | Result |
|---|---|
| CI, both natives jobs | `machine_test: 166 checks, 0 failed`. No sanitizers: MinGW has none, and the `manylinux_2_28` image has no `libasan` (the script prints a note and drops them). |
| Windows 11 | 166 checks, 0 failed (no sanitizers). |
| AlmaLinux 8 | 166 checks, 0 failed (no `libasan`, so no sanitizers). |
| Ubuntu 26.04 (gcc 15.2.0), with the assert LuaJIT that `machine-test.sh` built there | `machine-test: OK (linux-x86_64, with ASan and UBSan)`, 165 checks, 0 failed, in both runs (once in each phase of the local runs). The one check left out is "process memory stays flat" (item 10), which the test skips under ASan: ASan's quarantine keeps freed blocks, so a sanitized build only prints the before and after figures (`machine_test.c:2273-2279`; about 354 MiB before and 355 MiB after in both runs). |

Ubuntu 26.04 is the only place where Tier 0 runs with sanitizers. CI runs Tier 0 only in the two natives jobs, and neither can use them: MinGW has no ASan, and the `manylinux_2_28` image has no `libasan` (M1.md §6).

## 2. Tier 1 (`BridgeProbe`)

Every run: `bridgeProbe: 88 checks, 0 failed`, the JVM exit code 0, and no line of the scan patterns (`jni-warnings.txt`) in the log.

| Where | How | Scan |
|---|---|---|
| CI `natives-windows-x86_64` | plain `java -Xcheck:jni -Xmx256m`, log scanned by `jni-scan.sh` | `jni-scan: OK, none of the 4 patterns` |
| CI `natives-linux-x86_64` (glibc 2.28) | the same | `jni-scan: OK, none of the 4 patterns` |
| CI `mod` (Ubuntu 24.04) | `./gradlew :common:bridgeProbe` | `bridgeProbe: JNI scan clean, exit code 0` |
| CI `headless-windows-x86_64` | `./gradlew.bat --no-daemon … :common:bridgeProbe` | `JNI scan clean, exit code 0` |
| Windows 11 | `./gradlew :common:bridgeProbe` | `JNI scan clean, exit code 0` |
| AlmaLinux 8 | by hand, with the natives job's commands | `jni-scan: OK` |
| Ubuntu 26.04, with the Alma 8 library | by hand, with the natives job's commands | `jni-scan: OK` |

## 3. Tier 2 (`MachineSpecs`)

Every run: `machineSpecs: 33 passed (5 with an allowed difference from Cobalt), 0 failed; native machines live 0, leaked 0`, and a clean scan.

| Where | How |
|---|---|
| CI `mod` (Ubuntu 24.04) | `./gradlew :common:machineSpecs` |
| CI `headless-windows-x86_64` | `./gradlew.bat --no-daemon … :common:machineSpecs` |
| Windows 11 | `./gradlew :common:machineSpecs` |
| AlmaLinux 8 and Ubuntu 26.04 | by hand, with `:common`'s test runtime class path from Windows (see "Reproduce"), the Linux library first on the class path, and the JVM arguments of the `machineSpecs` task (`-Xcheck:jni`, `cct.version`, `headless.bootTimeout`, `ccluajit.nativeDir`) |

## 4. Scan self-test and negative control

`bash natives/tests/jni-scan.sh --self-test` prints `jni-scan: self-test OK` in both natives jobs, on Windows 11 and on AlmaLinux 8.

**Negative control.** A scratch copy of `natives/` and `LICENSES/`, outside the repository, with one change to `natives/src/jni_machine.c`: the `ExceptionCheck` block after `CallObjectMethod` in `jni_host_call` (lines 82–86) is removed. The copy builds against the repository's `vendor/luajit`. The change was not committed, and the working tree stayed clean.

| | Windows 11, through Gradle | AlmaLinux 8, by hand (the natives job's commands) |
|---|---|---|
| How | The mutant `cclj.dll` replaced the release one in `natives/build/out` for one `./gradlew :common:bridgeProbe` | `java -Xcheck:jni -Xmx256m … BridgeProbe 2>&1 \| tee log`, then `jni-scan.sh log`. The scan ran although java exited 1; in the natives job, `set -eo pipefail` would stop the step at that exit, before the scan. |
| Result | `BUILD FAILED`: `bridgeProbe failed: 1273329 line(s) of -Xcheck:jni output, exit code 1`. That names two reasons: the scan's hits, and the JVM's exit code 1 from the failed checks. | java exit 1; `jni-scan.sh` exit 1: `-Xcheck:jni reported problems: 1272129 matching line(s)` |
| The lines | 1,273,325 × `WARNING in native method: JNI call made without checking exceptions when required to from CallObjectMethod`, 4 × `… JNI call made with exception pending` | the same two kinds |
| Checks | 7 of 88 failed: the host-throws checks (an exception now reads as `host call returned null`, is no longer printed, and stays pending, so the next host call fails with `JNI failure in host call`), and the exception exit of the local-reference test | the same 7 |
| Log size | 1.1 GB | 1.1 GB |

So the scan catches the missing check on both OSes, and Tier 1's own checks catch its effect. After the Windows run, the release library was put back (SHA-256 checked) and `./gradlew :common:bridgeProbe` passed again, 88 of 88, scan-clean.

## 5. No G0 regression

| Where | Result |
|---|---|
| CI `mod` | `nativeProbe: OK`; `headlessSmoke` and `headlessSuite` scan-clean; `suite done=true … registered=1407 run=1407 counts={error=5, pass=1390, pending=12}`. All three tasks: `JNI scan clean, exit code 0`. |
| Windows 11 | `nativeProbe`, `headlessSmoke` and `headlessSuite` through Gradle (in one run with `bridgeProbe` and `machineSpecs`): `nativeProbe` OK, `headlessSuite` 1390 pass, 5 error, 12 pending of 1407, all scan-clean. |
| CI, both natives jobs | `run-harnesses.sh`: preempt 33 OK of 35 (the 2 expected hangs), patterns, cstack 31 of 31, compat, syntax 45 of 45, fixcheck 6 of 6, all `PASS`; the NativeProbe self-test passes, scan-clean. |

This is the Cobalt baseline of M0 (1390 pass, 12 pending, 5 errors from the HTTP tests with HTTP disabled). The 5 errors are listed in `common/src/test/resources/headless/expected-failures-cobalt.txt`; `headlessSuite` fails on a fail or error that is not listed there, and on a listed test that does not fail or error.

## 6. `CCLJ_DEBUG=1` build

`CCLJ_DEBUG=1 bash natives/build-natives.sh <platform-id>` builds LuaJIT with `-DLUA_USE_ASSERT -DLUA_USE_APICHECK` (its internal assertions and the Lua C API argument checks, which abort on a failure), and `natives/src` with `-g -O1 -DCCLJ_DEBUG` (the stack-balance checks `CM_CHECK` of `machine.c`). Tier 1 and Tier 2 load this library. Tier 0 does not use it (§1); the Tier 0 runs below repeat the test of §1.

| Where | Library | Lint | Tier 0 | Tier 1 | Tier 2 |
|---|---|---|---|---|---|
| Windows 11 | `cclj.dll` `799f7b6423d72be8` (`DEBUG-BUILD.txt` next to it) | OK | 166 of 166 (as §1) | `./gradlew :common:bridgeProbe`: 88 of 88, `library build: DEBUG`, scan-clean | `./gradlew :common:machineSpecs`: 33 of 33, scan-clean |
| AlmaLinux 8 | `libcclj.so` `4cf7e96b8c470990` | OK | 166 of 166 (as §1) | by hand: 88 of 88, scan-clean | by hand: 33 of 33, scan-clean |
| Ubuntu 26.04, with the Alma 8 debug library (Tier 1 and Tier 2) | the same | — | 165 of 165 with ASan and UBSan (as §1) | by hand: 88 of 88, scan-clean | by hand: 33 of 33, scan-clean |

`nativeProbe` also passed with the Windows debug library, scan-clean. No `CM_CHECK`, LuaJIT assertion or API check fired in any run.

Then both release libraries were built again, and their SHA-256 matched the first builds exactly (`44f151d9dfe06271`, `004c09267fd7ee1d`): on one machine, the builds are reproducible. Lint, Tier 0 and Tier 1 passed again on both. Tier 2 passed again (33 of 33, scan-clean) on Windows, and on AlmaLinux 8 and Ubuntu 26.04 with the Alma 8 library: those are the second Linux runs of §8. `natives/build/out` ends as a release build.

## 8. ns per call (D1)

`BridgeProbe` measures one resume of 200,000 calls of `t.echo("hello")` (a string in, a string out, through `Wire` both ways), five times, and prints the median. Each run below is one JVM.

| Where | Without `-Xcheck:jni` (3 JVMs) | With `-Xcheck:jni` |
|---|---|---|
| Windows 11 (Gradle, `-PcheckJni=false`) | 209.1, 202.2, 204.2 ns | 331.1 ns |
| AlmaLinux 8 | 211.6, 213.3, 212.5 ns | 275.8 ns |
| Ubuntu 26.04 | 190.8, 194.1, 193.1 ns | 247.9 ns |
| CI (shared runners) | not measured | natives-linux 506.0, natives-windows 779.0, mod 494.8, headless-windows 583.5 ns |

D1 says: switch to a direct `ByteBuffer` only if G1 shows more than 300 ns per call without `-Xcheck:jni`. All nine runs are between 190 and 214 ns, so **the `byte[]` transport stays**. For comparison, the planning benchmark measured Cobalt's own CC:T call path at about 94–220 ns for the same kind of call (F14).

`MachineSpecs` scenario 22 measures the whole path through `LuaJITMachine` and CC:T's generated method (100,000 calls of `t.nop()` in one resume, with `-Xcheck:jni`): 308 ns on Windows 11, 320 ns on Alma 8, 310 ns on Ubuntu 26.04, 737 ns in the mod job and 661 ns in the Windows Gradle job. (The first Linux runs of Tier 2 ran at the same time as each other and as the build of the Windows mutant, and gave 390 and 384 ns; the figures above come from a second run of each, alone. Both runs passed 33 of 33.)

## Not covered

- Tier 0 under sanitizers runs only on Ubuntu 26.04, by hand (§1).
- The ns-per-call figures come from one machine (the PC this was developed on), as medians of five runs in one JVM. Some runs had other work beside them: the four Ubuntu runs and the Windows Gradle run of §5 ran at the same time (so the Windows figures with `-Xcheck:jni`, 331.1 ns, and of scenario 22, 308 ns, come from a loaded run), and two of the four Alma runs (the one with `-Xcheck:jni`, and the first without it) ran during the Windows Tier 0 run and its scan self-test; the other two Alma runs (213.3 and 212.5 ns) had no other G1 run beside them. Load can only raise a figure. On the CI runners the `-Xcheck:jni` figures are 1.8–2.4 times the local ones, so on a slower machine the figure without `-Xcheck:jni` could pass 300 ns.
- The scan texts were checked (F13), and the negative control ran, on JDK 21.0.13 only. The Ubuntu 26.04 runs use OpenJDK 21.0.12.1; their zero scan hits (items 2, 3 and 6) assume that this JDK prints the same texts.
- If the fix rounds (S1 step 7) change code, items 1–6 and 8 are all run again on the fix commit, not only the items that the change touches, because G1 asks for all of them on one commit. This report is then updated.

## Reproduce

The local runs used the commands of `natives/tests/README.md` ("Through Gradle", and the Tier 1 commands for Linux); the script of each phase is in `artifacts/g1/runs/`. MachineSpecs under WSL used `:common`'s test runtime class path (`sourceSets.test.runtimeClasspath`). A one-off Gradle init script, not in the repository, added a `printTestClasspath` task to `:common` and printed it on Windows. Its `C:\...` paths were changed to `/mnt/c/...` and `;` to `:`, and `natives/build/out` was put first.
