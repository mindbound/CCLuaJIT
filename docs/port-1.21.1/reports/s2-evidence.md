# S2 evidence: events on commit b2f14d5

The S2 gate ([S2.md](../S2.md) §8.3) asks for seven things on one commit. This report gives the evidence on commit b2f14d5 ("Deliver events through LuaJITMachine", S2 step 3), in the short form of decision D33:
- CI run [43387252021](https://github.com/mindbound/CCLuaJIT/actions/runs/43387252021), a push on 2026-11-13. All five jobs ended with `success`: both natives jobs, `mod`, `headless-windows-x86_64` and the informational `linux-aarch64` harness job. The macOS job runs only on a manual dispatch (b8e81a8), so it did not run.
- Local runs on 2026-11-13 from a clean working tree at the same commit (`git status` empty), on Windows 11, AlmaLinux 8 and Ubuntu 26.04 (WSL).

The logs are not in the repository. That is this report's choice, not D33's: D33 sets what the report holds and says nothing about logs, and S1 kept its logs in `artifacts/g1/`. The CI job logs and artifacts are on the run's page, and the local results are below.

Item 7, your sign-off of the review in §8.4, is open.

## Summary

| # | The gate requires (§8.3) | Result |
|---|---|---|
| 1 | Tier 0 passes on Windows 11 (MSYS2, assert build), AlmaLinux 8 (assert build), Ubuntu 26.04 under ASan and UBSan, and in both natives jobs | Met. 192 of 192 in both natives jobs, on Windows 11 and on AlmaLinux 8. 191 of 191 under ASan and UBSan on Ubuntu 26.04 (§1). On Windows 11, Tier 0 ran in Git Bash with MinGW-w64 gcc 16.2.0 (Brecht Sanders r1), as in S1, not in MSYS2. The MSYS2 MINGW64 run is the Windows natives job. `machine-test.sh` accepts both. |
| 2 | Lint, release build and harnesses pass on Windows and Alma 8, compat with S2's four lines included | Met. On Windows 11, AlmaLinux 8 and in both natives jobs: lint OK, the release build OK, and `run-harnesses.sh` `ALL PASSED` with `compat PASS 14 lines` (§2). |
| 3 | Tier 1 passes with zero scan hits in six places | Met. 125 of 125 and a clean scan in all six: both natives jobs, `mod`, Windows 11, AlmaLinux 8, and Ubuntu 26.04 with the Alma 8 library. Also in `headless-windows-x86_64` (§3). |
| 4 | Tier 2 passes with zero scan hits in `mod`, `headless-windows-x86_64`, and on Windows 11, Alma 8 and Ubuntu 26.04. No differential event scenario has an allow-list note | Met. 62 passed, 0 failed, live 0, leaked 0, and a clean scan in all five. The 5 allowed differences are S1's scenarios 7, 10, 12c, 12d and 13. No event scenario has an allow-list entry (§4). |
| 5 | D33: one `CCLJ_DEBUG=1` run of Tier 1 and Tier 2 on Windows and on Alma 8 | Met. 125 of 125 and 62 passed with the debug library on both, scan-clean (§5). |
| 6 | CI: one green run on the commit; its `nativeProbe`, `headlessSmoke` and `headlessSuite` are scan-clean (no G0 regression) | Met. Run 43387252021. In `mod`: `nativeProbe: OK`, and `headlessSuite` on Cobalt 1390 pass, 5 error, 12 pending of 1407, the 5 errors expected (HTTP off). All three tasks: `JNI scan clean, exit code 0` (§6). |
| 7 | You sign off the review in §8.4 | Open. |

## Environments

| Where | OS and compiler | JDK | Library (SHA-256, first 16 hex digits) |
|---|---|---|---|
| CI `natives-linux-x86_64` | `manylinux_2_28` container on ubuntu-24.04, gcc 14.2.1 (gcc-toolset-14) | Red Hat OpenJDK 21.0.13 | `0ca7040c5b9f0050` |
| CI `natives-windows-x86_64` | windows-2025-vs2026, MSYS2 MINGW64, gcc 16.2.0 (Rev4) | Temurin 21.0.13+9 | `ee34ad0fe161ebd1` |
| CI `mod` | ubuntu-24.04, with the library of the Linux natives job | Temurin 21.0.13+9 | `0ca7040c5b9f0050` |
| CI `headless-windows-x86_64` | windows-2025-vs2026, with the library of the Windows natives job | Temurin 21.0.13+9 | `ee34ad0fe161ebd1` |
| Windows 11 | MinGW-w64 gcc 16.2.0 (x86_64-msvcrt-posix-seh, Brecht Sanders r1), Git Bash | Temurin 21.0.13+9 | release `03db509d66b898e9`, debug `954eeb53d55ef4a1` |
| WSL AlmaLinux 8.10 | gcc 8.5.0, glibc 2.28 | Red Hat OpenJDK 21.0.13 | release `e453f992a47fdb1e`, debug `7f1e3e6245fca744` |
| WSL Ubuntu 26.04.1 | gcc 15.2.0, glibc 2.43. Tier 1 and Tier 2 load the library that Alma 8 built | OpenJDK 21.0.12.1 | `e453f992a47fdb1e` |

- The `mod` job checks that the mod jar holds the natives jobs' files, byte for byte (`cclj.dll` `ee34ad0f…`, `libcclj.so` `0ca7040c…`).
- Each local release library was built twice, before and after the debug build, with the same SHA-256 both times.
- The CI libraries differ from the local ones because other compilers built them.
- Both Linux libraries need at most `GLIBC_2.14` (the limit is 2.28).

## 1. Tier 0 (`machine_test`)

| Where | Result |
|---|---|
| CI, both natives jobs | `machine_test: 192 checks, 0 failed`, without sanitizers. MinGW has none, and the `manylinux_2_28` image has no `libasan`, so the script drops them with a note. |
| Windows 11 | 192 checks, 0 failed (no sanitizers), in each of the three runs: phase A, beside the debug build, and after it. |
| AlmaLinux 8 | 192 checks, 0 failed (no `libasan`, so no sanitizers), in each of the three runs. |
| Ubuntu 26.04 | `machine-test: OK (linux-x86_64, with ASan and UBSan)`, 191 checks, 0 failed. The check left out is item 10's "process memory stays flat". ASan's quarantine keeps freed blocks, so under ASan the test only prints the figures: 355.0 MiB before and 356.2 MiB after. |

`machine-test.sh` always links its own assert LuaJIT (`-DLUA_USE_ASSERT -DLUA_USE_APICHECK`) and `-DCCLJ_TEST -DCCLJ_DEBUG`, so every Tier 0 run is a debug build. Its S2 items are 4, 11 (a to k) and 12, a fuzz of 100,000 records.

## 2. Lint, release build and harnesses

| Where | Lint | Release build | `run-harnesses.sh` |
|---|---|---|---|
| CI natives jobs | `lint-natives: OK` | OK | `ALL PASSED`, `compat PASS 14 lines` |
| Windows 11 | 12 checks, 0 failed | OK | `ALL PASSED`: preempt 33 OK of 35 (the 2 expected hangs), patterns 74 of 74, cstack 31 of 31, compat 14 lines, syntax 45 of 45, fixcheck 6 of 6 |
| AlmaLinux 8 | 12 checks, 0 failed | OK | `ALL PASSED`: the same, with patterns 5 of 5 |

The 14 compat lines are the 10 of M0 and S2's four: three exact decimal ties (D34) and `%f` of tiny numbers (D35). The informational `linux-aarch64` job passes the harnesses too, compat included.

## 3. Tier 1 (`BridgeProbe`)

125 checks, 0 failed, in every row: the S1 checks and S2's items 10 to 17 (§6.2).

| Where | How | Scan |
|---|---|---|
| CI `natives-windows-x86_64` | plain `java -Xcheck:jni -Xmx256m`, log scanned by `jni-scan.sh` | `jni-scan: OK, none of the 4 patterns` |
| CI `natives-linux-x86_64` (glibc 2.28) | the same | `jni-scan: OK, none of the 4 patterns` |
| CI `mod` (Ubuntu 24.04) | `./gradlew :common:bridgeProbe` | `JNI scan clean, exit code 0` |
| CI `headless-windows-x86_64` | `./gradlew.bat --no-daemon … :common:bridgeProbe` | `JNI scan clean, exit code 0` |
| Windows 11 | `./gradlew :common:bridgeProbe` | `JNI scan clean, exit code 0` |
| AlmaLinux 8 | by hand, with the natives job's commands | `jni-scan: OK` |
| Ubuntu 26.04, with the Alma 8 library | by hand, with the natives job's commands | `jni-scan: OK` |

`jni-scan.sh --self-test` passes in both natives jobs and locally on Windows 11 and AlmaLinux 8.

## 4. Tier 2 (`MachineSpecs`)

`machineSpecs: 62 passed (5 with an allowed difference from Cobalt), 0 failed; native machines live 0, leaked 0` in every row, with a clean scan.

| Where | How |
|---|---|
| CI `mod` (Ubuntu 24.04) | `./gradlew :common:machineSpecs` |
| CI `headless-windows-x86_64` | `./gradlew.bat --no-daemon … :common:machineSpecs` |
| Windows 11 | `./gradlew :common:machineSpecs` |
| AlmaLinux 8 and Ubuntu 26.04 | by hand, as in S1 (g1-evidence.md "Reproduce"): `:common`'s test runtime class path from Windows, the Linux library first on it, and the JVM arguments of the `machineSpecs` task |

The 62 are S1's 39, with scenario 23 now a delivered event, and S2's 23 new ones: scenarios 29 to 50, and B4.
- The differential event scenarios (23, 29 to 32, 34 to 37, 42, 44, 45, 48 and 49) match Cobalt in every step, with no allow-list entry.
- The LuaJIT-only scenarios are the planned deviations of §2.2 (33, 38 to 41, 43 and 47) and LuaJIT's own branches (46 and 50).
- B4 delivers three events that are queued through a real `Computer`, in order.

## 5. `CCLJ_DEBUG=1` build

`CCLJ_DEBUG=1 bash natives/build-natives.sh <platform-id>` builds LuaJIT with `-DLUA_USE_ASSERT -DLUA_USE_APICHECK`, and `natives/src` with `-g -O1 -DCCLJ_DEBUG` (the `CM_CHECK`s of `machine.c`, which S2 adds to the event path).

| Where | Library | Lint | Tier 1 | Tier 2 |
|---|---|---|---|---|
| Windows 11 | `cclj.dll` `954eeb53d55ef4a1` (`DEBUG-BUILD.txt` next to it) | OK | `./gradlew :common:bridgeProbe`: 125 of 125, `library build: DEBUG`, scan-clean | `./gradlew :common:machineSpecs`: 62 passed, scan-clean |
| AlmaLinux 8 | `libcclj.so` `7f1e3e6245fca744` | OK | by hand: 125 of 125, scan-clean | by hand: 62 passed, scan-clean |

`nativeProbe` passes with the Windows debug library too, scan-clean. No `CM_CHECK`, LuaJIT assertion or API check fired.

Then both release libraries were built again. Lint, Tier 0 and Tier 1 passed again on both, and Tier 2 again on Windows. `natives/build/out` ends as a release build.

## 6. CI run 43387252021

| Job | Result | Steps that this report uses |
|---|---|---|
| `natives-linux-x86_64` | success | release build, lint, Tier 0, scan self-test, harnesses, `nativeProbe`, Tier 1 |
| `natives-windows-x86_64` | success | the same |
| `mod` | success | `./gradlew build`; the jar check; `nativeProbe`, `bridgeProbe`, `machineSpecs`, `headlessSmoke`, `headlessSuite` |
| `headless-windows-x86_64` | success | `nativeProbe`, `bridgeProbe`, `machineSpecs` |
| `harness-informational (linux-aarch64, not required)` | success | harnesses (informational) |

## Not covered

- Tier 0 under sanitizers runs only on Ubuntu 26.04, by hand (§1).
- macOS and arm64 are not in the S2 gate. The macOS job runs only on a manual dispatch.
- The Ubuntu 26.04 runs use OpenJDK 21.0.12.1. Their clean scans assume that this JDK prints the texts that F13 checked on 21.0.13, as in S1.
- No in-game test: CraftOS boots on LuaJIT only in S4, after S3's tables and S5's trampoline, and the in-game checks are S8 ([M1.md](../M1.md) §2).
- The logs are not kept in the repository. GitHub deletes the run's log and report artifacts (`logs-natives-*`, `logs-harness-*`, `reports-mod` and `reports-headless-windows-x86_64`) 14 days after the run (`retention-days: 14` in `build.yml`), so about 2026-11-27. It deletes the job logs when the repository's log retention ends. The local logs stayed in a scratch folder that is not kept.
- Timings are not part of the gate:
  - `BridgeProbe`'s ns per call with `-Xcheck:jni`: 302.7 on Windows 11, 285.0 and 277.6 on AlmaLinux 8, 246.6 on Ubuntu 26.04, and 421.3 to 840.7 on the CI runners.
  - `MachineSpecs` scenario 22: 285 ns on Windows 11, 332 on AlmaLinux 8, 312 on Ubuntu 26.04, 648 in `mod` and 878 in `headless-windows-x86_64`.
- If a fix round (§8.2 step 5) changes code, items 1 to 6 run again on its commit, and this report is updated.
