# Adversarial verification of the "port-mechanics" claims PM-1, PM-2 and PM-3

Verifier work dir: `<scratch>\work\verify-port-mechanics` (written as `$V` below).
I did not write to the repo. I read it only with `git archive`, `git show` and `git log`.
I used none of the investigator's build outputs as evidence. Their Linux `.so` files were inspected only to compare them with my own rebuilds.
Date: 2026-11-07.

## Verdicts in short

| Claim | Verdict | Severity (mine) | Key independent evidence |
|---|---|---|---|
| PM-1: CC:T 1.121.0 forces Loom >= 1.18, so Gradle must run on JDK 25 | **partially confirmed** | low (claim said high) | The facts reproduce. The word "forces" is wrong: it applies only when CC:T 1.121.0's **Fabric jar** is a Loom `mod*` dependency. Two workarounds build on Loom 1.17.21 / JDK 21 (verified). |
| PM-2: 1.20.1_CCT compiles but ships no natives; main.cpp does not compile as-is | **confirmed** | high (for the missing native pipeline). The `assert` error is a trivial one-liner. | Fresh-Gradle-home build succeeds. The jar has 14 entries and no natives. main.cpp fails on MinGW g++ 16.2 **and** Ubuntu g++ 15.2. The `assert(false)` came in the last WIP commit 505b1f9. |
| PM-3: Linux natives built on a current distro need GLIBC_2.38; build in manylinux_2_28 | **confirmed and strengthened** | medium (claim said high) | My own Ubuntu 26.04 build needs GLIBC_2.38, and loading it in a JVM on AlmaLinux 8 (glibc 2.28) **fails with UnsatisfiedLinkError**. My own AlmaLinux 8 build needs at most GLIBC_2.14 and loads on both. |

---

## PM-1: Loom >= 1.18 and JDK 25

### What I checked

1. **Manifest of the CC:T Fabric jar** (`unzip -p cc-tweaked-1.21.1-fabric-1.121.0.jar META-INF/MANIFEST.MF`):
   `Fabric-Loom-Version: 1.18.1`, `Fabric-Gradle-Version: 9.7.1`, `Fabric-Loom-Mixin-Remap-Type: static`. Confirmed.
   For comparison, 1.20.1-fabric-1.115.0 has Loom 1.9.2 and Gradle 8.12.
2. **Where the check is, and when it fires.** I disassembled `net/fabricmc/loom/configuration/mods/ArtifactMetadata.class` from `fabric-loom-1.17.21.jar` (`$V/loom1.17/ArtifactMetadata.javap.txt`):
   - `validateLoomVersion(modLoomVersion, currentLoomVersion)` is called **only when `Fabric-Loom-Version` is present and `Fabric-Loom-Mixin-Remap-Type == STATIC`** (bytecode offsets 188-204).
   - It compares **major.minor** only (loop `i < 2`) and throws `IllegalStateException("Mod was built with a newer version of Loom (%s), you are using Loom (%s)")`.
   - It is skipped only when the running Loom reports `0.0.0+unknown`. There is no property to opt out.
3. **Loom JVM and Gradle requirements.** I downloaded each `.module` myself from maven.fabricmc.net (`$V/meta/loom-*.module`):

   | Loom | runtimeElements `org.gradle.plugin.api-version` | `org.gradle.jvm.version` |
   |---|---|---|
   | 1.17.21 | 9.5.0 | 21 |
   | 1.18.1 / 1.18.2 / 1.18.3 / 1.18.4 | 9.7.0 | **25** |
   | 1.19.1 / 1.19.4 | 9.8.0 | **25** |

   Every 1.18.x release needs JDK 25, including 1.18.1, the version CC:T itself used.
4. **Builds.** Each build ran in its own project under `$V`. All use Gradle 9.8.2, MC 1.21.1, loader 0.20.1, Fabric API 0.116.17, Mojmap, the same stub sources as the investigator's scaffold, and `--no-daemon`.

   | Project | Loom | JDK running Gradle | CC:T dependency setup | Result | Log |
   |---|---|---|---|---|---|
   | `v17-mod` | 1.17.21 | 21 | compileOnly core/core-api/common/common-api 1.121.0 **+ `modRuntimeOnly` fabric 1.121.0** | **FAIL**: `Mod was built with a newer version of Loom (1.18.1), you are using Loom (1.17.21)` | `$V/build-v17-mod-jdk21.log` |
   | `v19-jdk21` | 1.19.4 | 21 | same | **FAIL**: `Dependency requires at least JVM runtime version 25. This build uses a Java 21 JVM.` | `$V/build-v19-jdk21.log` |
   | `v19-jdk25` | 1.19.4 | 25 | same | **BUILD SUCCESSFUL in 40s** | `$V/build-v19-jdk25.log` |
   | `v17-nomod` | 1.17.21 | 21 | compileOnly core/core-api/common/common-api 1.121.0 only (no `mod*` CC:T jar) | **BUILD SUCCESSFUL in 38s** | `$V/build-v17-nomod-jdk21.log` |
   | `v17-cct1202` | 1.17.21 | 21 | everything at CC:T **1.120.2**, including `modRuntimeOnly` fabric | **BUILD SUCCESSFUL in 36s** (one non-fatal Mercury error while remapping the sources jar) | `$V/build-v17-cct1202-jdk21.log` |

5. **Output equivalence.** `CCLuaJIT.class`, `LuaJITMachine.class` and `fabric.mod.json` from `v17-nomod` and `v19-jdk25` have **byte-identical sha256**, and both are class major 65 (Java 21). The manifests differ only in `Fabric-Loom-Version` and `Fabric-Tiny-Remapper-Version`.
6. **Earlier CC:T 1.21.1 releases** (manifests downloaded from maven.squiddev.cc): 1.117.0 used Loom 1.14.5, 1.119.0 used Loom 1.16.1, and **1.120.0 and 1.120.2 used Loom 1.17.11 (Gradle 9.5.0)**. Only 1.121.0, released 2026-11-03, moved to Loom 1.18.1.

### Conclusion

- **True:** the CC:T 1.121.0 Fabric jar has Loom 1.18.1 plus the `static` mixin remap type. Loom refuses it on 1.17.x. Loom 1.18 and later need JDK 25 to run Gradle. Loom 1.19.4 on Gradle 9.8.2 / JDK 25 with `release 21` builds.
- **Overstated:** "forces". The mod only needs `ServerContext`, `ILuaMachine` and similar types, and it compiles against the non-mod `core`/`common` artifacts with plain `compileOnly`. Loom 1.17.21 on JDK 21 then builds byte-identical classes. JDK 25 is the price of putting CC:T **1.121.0's Fabric jar** on the Loom dev runtime classpath (runClient/runServer). Two alternatives keep a dev runtime on JDK 21: use CC:T 1.120.2 as `modRuntimeOnly` (verified), or drop the jar into `run/mods`. For the second, fabric-loader 0.20.1 ships `RuntimeModRemapper`, but I did not run it.
- Gradle 9 can also auto-provision the daemon JVM, so a manual JDK 25 install is not strictly required (not tested).
- **Severity:** low. This is a toolchain choice with a working recommended path, not a correctness or feasibility problem. The recommendation (Loom 1.19.4 + JDK 25) is still sensible, because it tracks CC:T's own toolchain.

---

## PM-2: the 1.20.1_CCT build and main.cpp

### Java build (my own run, fresh Gradle home)

```
git -C <repo> archive HEAD | tar -x -C $V/src-1.20.1        # HEAD = 1.20.1_CCT @ 505b1f9
cd $V/src-1.20.1 && GRADLE_USER_HOME=$V/gradle-home-fresh JAVA_HOME=.../jdk-17.0.20.101-hotspot \
    ./gradlew build --no-daemon --console=plain --refresh-dependencies
```

- The fresh Gradle home proves that `1.9-SNAPSHOT` still resolves today and that nothing came from a stale cache.
- The log (`$V/build-1.20.1-fresh-home.log`) shows Gradle 8.11.1, `Fabric Loom: 1.9.2`, and `BUILD SUCCESSFUL in 2m 11s`.
- `build/libs/ccluajit-0.1.0.jar` is 5507 B with 14 entries: manifest, LICENSE, icon, `ccluajit.mixins.json`, `fabric.mod.json`, `CCLuaJIT.class` and `LuaJITMachine.class`. **There are no native libraries.**
- `build.gradle` has no Exec, cpp or native task, so `src/main/cpp` is not part of the Gradle build. The Makefile is standalone: `OUT_DIR=/root/out`, it links dynamically against a prebuilt `vendor/luajit/bin/{linux,windows}`, and it expects Docker (there is a `.dockerignore`).
- On the Java side, `grep -rn "native " src/main/java` finds only the comment `// Used in native code.`. There are no `native` method declarations. `LuaJITMachine.java:17-21` holds the hard-coded absolute `System.load` paths (already established).

### main.cpp

- `g++ -fsyntax-only -I jni-headers -I jni-headers/win32 -I luajit/src main.cpp` with MinGW g++ 16.2.0 gives `main.cpp:291:9: error: 'assert' was not declared in this scope` (`$V/native/syntax-asis.log`).
- I independently reproduced it on **Ubuntu 26.04 g++ 15.2** (`$V/linux/ubuntu-all.log`, same error). So the failure does not depend on the compiler.
- **Root cause and history.** No included header pulls in `<assert.h>`. LuaJIT's `luaconf.h:143-145` includes it only under `LUA_USE_ASSERT` or `LUA_USE_APICHECK`, and `jni.h` includes only stdio and stdarg. `git log -S"assert(false)"` shows that the line was added in **505b1f9 (2025-03-14, "Hmmmh. Yeah, we're probably in for a fair bit of pain here, huh...")**, the newest WIP commit. The 1.14.4_CCT version of main.cpp passes `-fsyntax-only` cleanly (`$V/native/syntax-1.14.4.log`).
- With `-include cassert` it links against my static LuaJIT (`mingw32-make BUILDMODE=static`):
  - `$V/native/cclj.dll` imports only `KERNEL32.dll` and `msvcrt.dll`.
  - It exports exactly `JNI_OnLoad`, `JNI_OnUnload` and `Java_gay_vereena_cclj_computer_LuaJITMachine_{abortMachine,deinitMachine,handleEvent,initMachine,loadBios,registerAPI}`.
  - The same set is exported by my Linux build `$V/linux/out-ubuntu/libcclj-maincpp.so`.
- `JNI_OnLoad` looks up the stub's fields `luaState`, `mainRoutine`, `aborted` and `yieldRequested`, and the static `decodeString([B)`. These do exist in the 1.20.1 stub. The CC:T classes it uses (`ILuaAPI.getNames`, `IDynamicLuaObject.getMethodNames`/`callMethod`, `MethodResult`) exist with the same signatures in both 1.115.0 and 1.121.0. So `JNI_OnLoad` itself would probably succeed, but no Java `native` method is declared to call into the library.

### Conclusion

Confirmed. The severity is high for "the branch has no native build or packaging pipeline", because a usable release needs one. The `<cassert>` compile error is a trivial (low) fix that the last WIP commit introduced.

---

## PM-3: the glibc baseline

### My own builds (not the investigator's binaries)

I cloned LuaJIT at the vendor commit `19878ec05c239ccaf5f3d17af27670a963e25b8b` from GitHub inside WSL (LF line endings), into `$V/linux/luajit-src`. Scripts are in `$V/scripts/`:

- `build-linux.sh` copies the tree, runs `make BUILDMODE=static CFLAGS=-fPIC`, and links a minimal C JNI probe (`$V/linux/probe/probe.c`) as `libprobe.so` with `-fvisibility=hidden -Wl,--exclude-libs,ALL -static-libgcc`.
- `inspect-so.sh` reports `objdump -T` symbol versions and `readelf -d` NEEDED entries.
- `run-probe.sh` loads the library in a JVM using `vpm.Probe`, compiled with `--release 8` so one class file runs on Java 8 and Java 17.

| Library | Built on | Max GLIBC | Symbols >= 2.29 |
|---|---|---|---|
| `$V/linux/out-ubuntu/libprobe.so` (686 KB) | Ubuntu 26.04.1, glibc 2.43, gcc 15.2 | **2.38** | `__isoc23_fscanf`, `__isoc23_strtoul`, `fmod` @2.38; `_dl_find_object` @2.35; `dlopen`/`dlsym`/`dlerror`/`dlclose`/`pthread_once` @2.34; `exp`/`log`/`log2`/`pow` @2.29 |
| `$V/linux/out-ubuntu/libcclj-maincpp.so` (real main.cpp, C++, `-static-libstdc++`) | same | **2.38** | same set minus `__isoc23_fscanf`; no GLIBCXX or CXXABI dependencies |
| `$V/linux/out-el8/libprobe.so` (644 KB) | AlmaLinux 8.10, glibc 2.28, gcc 8.5 | **2.14** | none (NEEDED adds `libdl.so.2`) |

Inspecting the investigator's two `.so` files gives the same picture (`$V/linux/inspect-investigator-so.log`): 2.38 for the Ubuntu build and 2.14 for the el8 build.

### Load tests

AlmaLinux 8, glibc 2.28, OpenJDK 1.8.0_504 (`$V/linux/alma-all.log`):
```
LOAD OK (java 1.8.0_504): .../out-el8/libprobe.so          version: LuaJIT 2.1.1732813678 Linux x64 jit=true ; 1+2: 3 ; error: ERR ... boom ; loop 1e8 in 0.199 s
LOAD FAILED: java.lang.UnsatisfiedLinkError: .../out-ubuntu/libprobe.so: /lib64/libc.so.6: version `GLIBC_2.35' not found
LOAD FAILED: java.lang.UnsatisfiedLinkError: .../out-ubuntu/libcclj-maincpp.so: /lib64/libc.so.6: version `GLIBC_2.38' not found
LOAD FAILED: ... port-mechanics/natives/out/linux-x86_64/libcclj.so: version `GLIBC_2.35' not found
```

Ubuntu 26.04, OpenJDK 17.0.20.1 (`$V/linux/el8-lib-on-ubuntu.log`):
```
LOAD OK (java 17.0.20.1): .../out-el8/libprobe.so          LuaJIT ... jit=true ; 3 ; ERR ... boom ; loop 1e8 in 0.225 s
```

### Conclusion

Confirmed, with stronger evidence: an actual JVM load failure, not only symbol inspection.

- A modern-distro build needs glibc >= 2.38. That excludes Debian 12 (2.36), Ubuntu 22.04 (2.35), RHEL 9 (2.34) and RHEL 8 (2.28).
- The 2.38 floor is **not** caused by a single avoidable flag. `fmod@2.38` and `exp`/`pow`@2.29 bind to the newest symbol versions at link time, so building on an old baseline is the right fix. Note that GitHub's `ubuntu-24.04` runner without a container would also produce a 2.38+ binary.
- Caveats:
  - (a) manylinux_2_28 itself was not run. AlmaLinux 8.10 is its base image, so it is a close proxy.
  - (b) The investigator's el8 proof and mine are both **C** probes, because the AlmaLinux 8 image has no g++. The real C++ bridge was not built on glibc 2.28. In manylinux_2_28 that needs the image's gcc-toolset g++, which links libstdc++ in a way that keeps the 2.28 floor. That step is unverified, so treat it as the first CI checkpoint.
- **Severity:** medium rather than high. It is a release-engineering pitfall with a standard fix, not a defect in existing code. If missed, though, the natives would fail to load on most LTS Linux servers.

---

## Issues in this lane the investigator missed or understated

1. **[info] The JDK 25 requirement exists only because of CC:T 1.121.0, released 4 days ago.** CC:T 1.120.0 and 1.120.2 were built with Loom 1.17.11 (Gradle 9.5.0, JDK 21 class). A dev setup on CC:T 1.120.2 builds with Loom 1.17.21 on JDK 21 (verified). Compiling against 1.121.0's core/common jars without a `mod*` CC:T jar also builds on JDK 21 with byte-identical output (verified).
2. **[low] The `assert` compile break is a regression from the abandoned WIP commit 505b1f9**, not a long-standing defect. The 1.14.4 main.cpp compiles. This supports treating 505b1f9 as unfinished work rather than a baseline.
3. **[low] The C++ bridge has not been verified on the old-glibc baseline.** Only C probes were built on glibc 2.28. A C++ build in manylinux_2_28 needs gcc-toolset, and its result (GLIBC <= 2.28, no GLIBCXX dependencies) should be checked in CI with `objdump -T`.
4. **[info] Loom's version check fires only for jars with `Fabric-Loom-Mixin-Remap-Type: static`, and it compares major.minor only.** There is no opt-out property.

## Files produced

- Build projects: `$V/{v17-mod,v17-nomod,v17-cct1202,v19-jdk21,v19-jdk25}`, and `$V/src-1.20.1` (a git-archive copy).
- Build logs: `$V/build-*.log`.
- Metadata: `$V/meta/loom-*.module`, `$V/meta/loom-metadata.xml`, `$V/meta/cct-fabric-*.jar`, `$V/meta/cct-fabric-meta.xml`, `$V/meta/cct-fabric-1.121.0.module`.
- Loom check disassembly: `$V/loom1.17/ArtifactMetadata.javap.txt`.
- Native, Windows: `$V/native/{syntax-asis.log,syntax-1.14.4.log,link-with-cassert.log,cclj.dll,main.cpp,main-1.14.4.cpp}`, plus a static LuaJIT in `$V/native/luajit/src/libluajit.a`.
- Native, Linux:
  - scripts in `$V/scripts/{build-linux.sh,inspect-so.sh,run-probe.sh,ubuntu-all.sh,alma-all.sh}`
  - probe in `$V/linux/probe/{probe.c,vpm/Probe.java,classes/}`
  - outputs in `$V/linux/out-ubuntu/`, `$V/linux/out-el8/`
  - logs in `$V/linux/{ubuntu-all.log,alma-all.log,el8-lib-on-ubuntu.log,inspect-investigator-so.log}`
- `$V/gradle-home-fresh` (539 MB) is a throwaway Gradle home and can be deleted.
