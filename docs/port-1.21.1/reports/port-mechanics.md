# CCLuaJIT 1.20.1 to 1.21.1: build-system and mod-loader port (lane: port-mechanics)

Work dir: `<scratch>\work\port-mechanics` (written as `$W` below).
I never wrote to the repo. I made a private `git clone` at `$W/repo` and built only there and in fresh project dirs.
Date of all version checks: 2026-11-07.

## TL;DR

| Question | Answer | Proven? |
|---|---|---|
| A. Does 1.20.1_CCT compile? | **Yes.** `gradlew build` succeeds (Loom 1.9-SNAPSHOT resolves to 1.9.2, Gradle 8.11.1, JDK 17), 1m54s. But the jar holds only 2 classes and **no natives**, so the build proves nothing about the mod working. The repo's `main.cpp` **does not compile** as-is (missing `#include <cassert>`). | yes |
| B. Fabric 1.21.1 scaffold | Builds: Loom **1.19.4** (`net.fabricmc.fabric-loom-remap`), Gradle **9.8.2** on **JDK 25**, `--release 21`, MC 1.21.1, loader 0.20.1, Fabric API 0.116.17+1.21.1, **Mojang mappings**, CC:T 1.121.0. CC:T 1.121.0's fabric jar was built with Loom 1.18.1, so **Loom ≥ 1.18 is mandatory, and that means Gradle on JDK 25**. Loom 1.17.21 on JDK 21 fails. | yes |
| C. NeoForge | CC:T's `cc-tweaked-1.21.1-forge` **is the NeoForge mod** (`META-INF/neoforge.mods.toml`, requires neoforge `[21.1.265,21.2)`). A stub built with **ModDevGradle 2.0.148** and NeoForge **21.1.265** compiles. A **multiloader** build (common + fabric + neoforge) also compiles and packages the natives into both jars. **No Mixin is needed**: `ServerContext.luaMachine` is `public static` and non-final, and CC:T reads it only in `ServerContext`'s constructor, which runs at server start. | builds yes; runtime timing from source only |
| D. Natives | **One self-contained library per platform, with LuaJIT linked statically, is proven.** Windows DLL (582 KB) imports only `KERNEL32.dll` and `msvcrt.dll` and exports only `Java_*`. It loads in JDK 21 and 25 and runs Lua with JIT on. Linux .so built in WSL loads too. The **glibc baseline matters**: a build on Ubuntu (glibc 2.43) needs GLIBC_2.38, while a build on AlmaLinux 8 needs only GLIBC_2.14. A content-hash cache-dir loader was tested with 12 concurrent JVMs, and with loading from the built mod jars. | yes (win-x64, linux-x64); macOS/arm64 not run |
| E. CI | Use **native GitHub runners**: `windows-2025`, manylinux_2_28 containers on `ubuntu-24.04` and `ubuntu-24.04-arm`, `macos-15-intel`, `macos-15`. Each one smoke-tests the library in a real JVM. Do not cross-compile. Draft: `$W/ci/build.yml`. | YAML parses; not run on GitHub |
| F. Effort (mechanical only) | Experienced modder about **3-5 person-days** using these scaffolds (5-8 from scratch). Newcomer about **8-14** (12-20 from scratch). | estimate |

## A. The 1.20.1_CCT branch as it is

Command (in the clone; the submodules were not initialised and Java does not need them):
```
cd $W/repo && JAVA_HOME="C:/Program Files/Eclipse Adoptium/jdk-17.0.20.101-hotspot" ./gradlew build --no-daemon --stacktrace
```
Log: `$W/build-1.20.1.log`. It prints `Fabric Loom: 1.9.2`, remaps 54+1+43 mods, and ends with `BUILD SUCCESSFUL in 1m 54s`.

Output: `build/libs/ccluajit-0.1.0.jar` (5507 B). It contains only `CCLuaJIT.class`, `LuaJITMachine.class`, `fabric.mod.json`, an empty `ccluajit.mixins.json` and the icon. **No natives are packaged**, and no Gradle task builds them. `build.sh` and the Makefile are standalone and still reference Docker paths (`OUT_DIR=/root/out`).

Other build-mechanics problems I saw on this branch (the 1.21.1 scaffold fixes all of them):
- `id 'fabric-loom' version '1.9-SNAPSHOT'`: a SNAPSHOT plugin gives builds that cannot be reproduced. Pin an exact version.
- `fabric.mod.json` (`src/main/resources/fabric.mod.json`) has no `"computercraft"` entry in `depends`. Without CC:T the mod would crash with `NoClassDefFoundError` instead of giving a clean dependency error. `contact` still holds the Fabric example-mod URLs.
- `splitEnvironmentSourceSets()` with an empty client source set, and an empty Mixin config: both are dead weight. No Mixin is needed (see C).
- The native side. I compiled `src/main/cpp/main.cpp` against static LuaJIT with MinGW g++ 16.2:
  `main.cpp:291:9: error: 'assert' was not declared in this scope` (log `$W/natives/main-cpp-check/compile.log`).
  With `-include cassert` it compiles into one DLL (`$W/natives/main-cpp-check/compile2.log`). That DLL imports only KERNEL32 and msvcrt and exports `JNI_OnLoad`, `JNI_OnUnload` and `Java_gay_vereena_cclj_computer_LuaJITMachine_{abortMachine,deinitMachine,handleEvent,initMachine,loadBios,registerAPI}`. None of those natives is declared in the 1.20.1 Java stub.

## B. Fabric 1.21.1 scaffold (proven)

Files (all in `$W/fabric-1.21.1/`): `settings.gradle`, `build.gradle`, `gradle.properties`, `gradle/wrapper/gradle-wrapper.properties` (Gradle 9.8.2), `src/main/resources/fabric.mod.json`, `src/main/java/gay/vereena/cclj/CCLuaJIT.java`, `src/main/java/gay/vereena/cclj/computer/LuaJITMachine.java`.

Command: `JAVA_HOME=.../jdk-25.0.4.101-hotspot ./gradlew build --no-daemon`. Result: `BUILD SUCCESSFUL in 1m 21s` (`$W/build-fabric-1.21.1.log`). Output: `build/libs/ccluajit-0.2.0.jar`, remapped to intermediary.

### Versions and why

| Item | Version | Source / reason |
|---|---|---|
| Minecraft | 1.21.1 | target |
| Fabric Loom | **1.19.4**, plugin id **`net.fabricmc.fabric-loom-remap`** | latest release on maven.fabricmc.net. In Loom ≥ 1.14 the plugin ids split: `fabric-loom-remap` is for obfuscated MC (≤ 1.21.11) and `net.fabricmc.fabric-loom` is for the unobfuscated 26.x line. Checked in the jar: `META-INF/gradle-plugins/net.fabricmc.fabric-loom-remap.properties -> LoomRemapGradlePlugin`. |
| Gradle | **9.8.2** | Loom 1.19.4's module metadata requires `org.gradle.plugin.api-version 9.8.0` and `org.gradle.jvm.version 25`. 9.9.0 had been out for one day, so I chose 9.8.2 (2026-10-19). |
| JDK running Gradle | **25** | required by Loom ≥ 1.18. The compile target stays at `options.release = 21`. |
| fabric-loader | 0.20.1 | latest. CC:T needs ≥ 0.15.10. |
| Fabric API | 0.116.17+1.21.1 | latest for 1.21.1. CC:T needs ≥ 0.116.12. Only `modRuntimeOnly`, because this mod never calls Fabric API. |
| Mappings | **Mojang (`loom.officialMojangMappings()`)** | the mod uses almost no MC classes, so the choice is nearly free. Mojmap matches CC:T's `common` jar and NeoForge, and Yarn ends after 1.21.11. |
| CC:T | 1.121.0 (latest for 1.21.1, metadata lastUpdated 2026-11-03) | |

**Why Loom ≥ 1.18 is mandatory (verified).** With Loom 1.17.21 on JDK 21, configuration fails with:
`IllegalStateException: Mod was built with a newer version of Loom (1.18.1), you are using Loom (1.17.21)`
(`$W/build-fabric-1.21.1-loom1.17-jdk21.log`). The offending artifact is `cc-tweaked-1.21.1-fabric-1.121.0.jar`, whose manifest contains `Fabric-Loom-Version: 1.18.1`. Fabric API 0.116.17 was built with Loom ≤ 1.6.11 and loader 0.20.1 carries no tag. Loom ≥ 1.18 requires JDK 25 to run Gradle. So **you need JDK 25 installed to build, even though the mod targets Java 21.**

Loom JVM and Gradle requirements I extracted from the `.module` files: 1.10.5 needs Gradle 8.12 on JDK 17. 1.11 to 1.13 need 8.14 on JDK 21. 1.14 and 1.15 need 9.2 on JDK 21. 1.16 needs 9.4 on JDK 21. 1.17 needs 9.5 on JDK 21. 1.18.4 needs 9.7 on JDK 25. 1.19.4 needs 9.8.0 on JDK 25.

### Dependency configurations (CC:T 1.121.0, from its Gradle module metadata)
```groovy
compileOnly    "cc.tweaked:cc-tweaked-1.21.1-core-api:1.121.0"   // ILuaAPI etc; pure Java; pulls jspecify/jetbrains annotations
compileOnly    "cc.tweaked:cc-tweaked-1.21.1-core:1.121.0"       // ILuaMachine, MachineEnvironment, MachineResult, CobaltLuaMachine
compileOnly    "cc.tweaked:cc-tweaked-1.21.1-common-api:1.121.0"
compileOnly    "cc.tweaked:cc-tweaked-1.21.1-common:1.121.0"     // ServerContext; Mojang-named, not a Fabric mod -> no mod* prefix
modRuntimeOnly "cc.tweaked:cc-tweaked-1.21.1-fabric:1.121.0"     // the real mod (intermediary), embeds core+common; Cobalt etc via jar-in-jar
```
- core and core-api contain no Minecraft classes, so they need no remapping and plain `compileOnly` is right. The `apiElements` variant of `core` only depends on core-api. junit, kotlin and jqwik appear only in its `testFixtures*` variants, so they do not leak onto the compile classpath, even though the plain POM lists them as `compile`.
- **Cobalt is not needed at compile time.** At runtime the fabric artifact's `runtimeElements` adds `cc.tweaked:cobalt:0.9.9`, and the jar embeds `META-INF/jars/cobalt-0.9.9.jar`.
- `cc-tweaked-1.21.1-fabric` already contains `ServerContext`, `ILuaMachine` and the rest, so `modCompileOnly` on it alone would also work, at the cost of remapping a larger jar.

### API delta 1.115.0 to 1.121.0 for this mod's surface
I diffed the extracted sources. `ILuaMachine`, `MachineEnvironment`, `MachineResult` and `ILuaAPI` changed only from `javax.annotation.Nullable` to `org.jspecify.annotations.Nullable`, and `handleEvent` became `(@Nullable String, @Nullable Object @Nullable [])`. Compiled signatures are identical. **The 1.20.1 Java code ports with zero API changes.** `Factory.create` still `throws IOException, MachineException`.
`ServerContext.java` (1.121.0) line 54-55: `@VisibleForTesting public static ILuaMachine.Factory luaMachine = CobaltLuaMachine::new;`. The field is read only at line 77 (`.luaFactory(luaMachine)`), inside the private constructor.

## C. NeoForge, timing and multiloader layout

### Is the "forge" artifact NeoForge? Yes.
`cc-tweaked-1.21.1-forge-1.121.0.jar` contains `META-INF/neoforge.mods.toml` (`modLoader="javafml"`, dependency `neoforge` `[21.1.265,21.2)`), an `accesstransformer.cfg`, and `META-INF/jarjar/{cobalt-0.9.9,jzlib-1.1.3}.jar`. It also embeds core and common (`ServerContext.class`, `CobaltLuaMachine.class`). Its Gradle module lists no dependencies. Matching `cc-tweaked-1.21.1-forge-api` and `-fabric-api` artifacts also exist.
The entrypoint is `@Mod(ComputerCraftAPI.MOD_ID) public final class ComputerCraft { public ComputerCraft(IEventBus eventBus, ModContainer container) ... }` (sources jar, `dan200/computercraft/ComputerCraft.java:77-82`).

### NeoForge stub (proven)
`$W/neoforge-1.21.1/`: build plugin **ModDevGradle `net.neoforged.moddev` 2.0.148**, the current NeoForge MDK default. I did not use NeoGradle (userdev 7.1.39), because MDG is simpler and NeoForge recommends it. NeoForge 21.1.265, Gradle 9.8.2 on JDK 25, Java toolchain 21. CC:T as `compileOnly` core-api/core/common-api/common plus `runtimeOnly cc-tweaked-1.21.1-forge`, with no remapping because NeoForge 1.21.1 uses Mojang names everywhere.
`@Mod("ccluajit") public CCLuaJIT(IEventBus modBus, ModContainer container) { ServerContext.luaMachine = LuaJITMachine::new; }`.
Result: `BUILD SUCCESSFUL in 1m 15s`, which includes NFRT decompile/recompile (`$W/build-neoforge-1.21.1.log`).

### Is mod construction or onInitialize early enough? Yes, with no Mixin.
- `CommonHooks.onServerStarting(server)` calls `ServerContext.create(server)` (`src-1.21.1-1.121.0/dan200/computercraft/shared/CommonHooks.java:82-90`). The constructor reads `luaMachine` and passes it to `ComputerContext.builder(...).luaFactory(...)`. Every `ComputerExecutor` later calls `luaFactory.create(...)` (`ComputerExecutor.java:152,370`).
- On Fabric this runs from `ServerLifecycleEvents.SERVER_STARTING` (CC:T fabric `ComputerCraft.java:94-100`). On NeoForge it runs from `ServerStartingEvent` (`ForgeCommonHooks.java:47-50`). Both fire per server start, for the integrated and the dedicated server, long after all `ModInitializer.onInitialize()` calls and all `@Mod` constructors have run.
- Triggering `ServerContext`'s static init early is harmless. Its statics are a logger, `new LevelResource("computercraft")`, the lambda, and a `MainThreadConfig` that reads `ConfigSpec` lazily (`ServerContext.java:50-55, 243-253`).
- Nothing else reads the field, so no Mixin and no access widener are needed. **Fragility:** the field is a test hook (`@VisibleForTesting`). If a future CC:T makes it private or final, a small `@Mixin(ServerContext.class)` `@Redirect` or accessor would be needed. On 1.21.1 that cannot happen, because 1.121.0 is the last 1.21.1 release.
- Not run in-game. A `runServer` needs `eula=true`, and accepting the Minecraft EULA is the user's call. The timing claim rests on the source references above.

### Multiloader layout (proven to compile)
`$W/multiloader-1.21.1/` builds with one command and produces `fabric/build/libs/ccluajit-fabric-0.2.0.jar` and `neoforge/build/libs/ccluajit-neoforge-0.2.0.jar`. Both contain the common classes plus `natives/<platform>/<lib>`. Clean build `BUILD SUCCESSFUL in 45s`, with no deprecations from our scripts under `--warning-mode all`.
```
settings.gradle          pluginManagement: Fabric + NeoForged repos; plugin versions (loom-remap 1.19.4, moddev 2.0.148)
gradle.properties        all versions in one place
build.gradle             subprojects{} : java-library, toolchain 21, release 21, SquidDev maven (cc.tweaked only)
common/                  PLAIN java-library, no Loom/MDG/Minecraft at all:
  build.gradle           compileOnly CC:T core-api/core/common + slf4j-api 2.0.9 (MC 1.21.1 ships 2.0.9, verified)
                         collectNatives (Sync) : natives/out/<platform>/<lib> -> jar:natives/<platform>/<lib>;
                         -Pcclj.requireAllNatives=true fails the build if any of the 5 is missing
  CCLuaJITCommon.java    init(gameDir): ServerContext.luaMachine = (env,bios) -> nativesAvailable()
                                         ? new LuaJITMachine(env,bios) : new CobaltLuaMachine(env,bios)
                         (lazy native load on first boot; Cobalt fallback on unsupported platform)
  computer/LuaJITMachine.java   (stub; the real rewrite goes here)
  natives/NativeLoader.java     (extraction/loading, see D)
  natives/NativeProbe.java      (self-test natives: version(), eval())
fabric/                  Loom remap, Mojmap; compiles common sources into its own jar (MultiLoader-Template style)
  CCLuaJITFabric.java    onInitialize -> CCLuaJITCommon.init(FabricLoader.getInstance().getGameDir())
neoforge/                MDG; same source-inclusion trick
  CCLuaJITNeoForge.java  @Mod ctor -> CCLuaJITCommon.init(FMLPaths.GAMEDIR.get())
natives/build-natives.sh, natives/src/cclj_selftest.cpp, .github/workflows/build.yml
```
Key point: the common module compiles `ServerContext.luaMachine = ...` **without Minecraft on the classpath**. javac never needs to resolve the MC types in ServerContext's other signatures. So "common" stays a trivial `java-library`, with no VanillaGradle or MDG-legacy needed.

## D. Natives: packaging and loading on Java 21

### Recommendation: static-link LuaJIT and ship one file per platform

Jar layout (implemented in `common/build.gradle` and `NativeLoader.platformId()`):
```
natives/windows-x86_64/cclj.dll
natives/linux-x86_64/libcclj.so      (built in manylinux_2_28 -> glibc >= 2.28)
natives/linux-aarch64/libcclj.so
natives/macos-x86_64/libcclj.dylib   (MACOSX_DEPLOYMENT_TARGET 11.0)
natives/macos-aarch64/libcclj.dylib
```
I skipped universal macOS binaries on purpose. Two separate files are simpler and keep `lipo` out of CI. Size is about 0.57-0.6 MB per platform, so all five add roughly 3 MB raw, or about 1.4 MB compressed in the jar.

### Static-link proof on Windows
1. `cp -r .../scratchpad/luajit-win $W/luajit`, then `cd $W/luajit/src && mingw32-make clean && mingw32-make -j16 BUILDMODE=static`. This gives `libluajit.a` (813,812 B) in about 7 s (`$W/luajit/build-static.log`).
2. Link (stub `$W/native-static/src/cclj_probe.cpp`, JNI headers from the JDK):
   ```
   g++ -O2 -std=c++17 -shared -o out/cclj-windows-x86_64.dll src/cclj_probe.cpp \
       -I"$JDK21/include" -I"$JDK21/include/win32" -I$W/luajit/src $W/luajit/src/libluajit.a \
       -static -static-libgcc -static-libstdc++ -Wl,--no-undefined -s
   ```
   `-Wl,--kill-at` is unnecessary on x64, which has no stdcall decoration. `JNIEXPORT` is `__declspec(dllexport)` on win32, which disables ld's export-all, so `lua_*` stays internal.
3. `objdump -p` shows `DLL Name: KERNEL32.dll` and `DLL Name: msvcrt.dll` only. The export table has exactly `Java_gay_vereena_cclj_natives_NativeProbe_eval` and `..._version`.
4. `java -cp out/classes gay.vereena.cclj.natives.NativeProbe out/cclj-windows-x86_64.dll` (`$W/native-static/out/probe-run-jdk21.log` and `probe-run-jdk25.log`):
   ```
   System.load(cclj-windows-x86_64.dll) ok in 0.9 ms
   java.version = 21.0.13, os.arch = amd64
   version()    = LuaJIT 2.1.ROLLING | Windows x64 | jit=true | SSE3 SSE4.1 BMI2 fold cse dce fwd dse narrow loop abc sink fuse
   eval(1+2)    = 3
   eval(err)    = ERR: probe:1: boom
   eval(loop)   = sum=299999997 in 0.125 s      (1e8-iteration loop; same with jit.off(): 0.283 s)
   ```
   JDK 25 gives the same result plus the JEP 472 "restricted method" warning. **Java 21, which MC 1.21.1 uses, prints no warning.**

### Linux proof (WSL)
- I took LuaJIT at the vendor commit 19878ec0 with LF line endings (`$W/luajit-lf`). The Windows copy has CRLF endings.
- Ubuntu (glibc 2.43, JDK 17): `build-natives.sh` gives `libcclj.so` 609,088 B. NEEDED is only `libm.so.6`, `libc.so.6` and `ld-linux-x86-64.so.2`. Dynamic exports are only the two `Java_*` symbols (`-fvisibility=hidden -Wl,--exclude-libs,ALL`). The probe runs with `jit=true`, and the loop takes 0.234 s against 0.331 s with the JIT off. **But the library needs GLIBC_2.38** (`__isoc23_fscanf`, `__isoc23_strtoul`, `fmod@2.38`, `_dl_find_object@2.35`, `dlopen@2.34`). It would fail on Debian 12 (2.36), Ubuntu 22.04 (2.35) and RHEL 9 (2.34).
- AlmaLinux 8 (glibc 2.28, gcc 8.5, C twin `cclj_probe.c` because that image has no g++): `libcclj.so` 568,824 B with **max symbol version GLIBC_2.14**. It loads and runs on the Ubuntu JDK 17 (`$W/natives/probe-run-linux-el8-lib-on-ubuntu.log`). That is the case for building in `manylinux_2_28` containers in CI.
- musl/Alpine is not covered by glibc builds. It would be an optional sixth target if anyone asks.

### Extraction strategy (implemented in `NativeLoader.java`, tested)
- **Cache dir**: `<gameDir>/.ccluajit/natives/<first 16 hex of sha256(lib)>/<libname>`. I avoided `java.io.tmpdir` because it is often `noexec` on Linux servers, and because the old 1.14.4 code used `createTempDirectory` plus `deleteOnExit`, which leaks one DLL copy per launch on Windows.
- **Hash-named directory**: a new mod version never overwrites a file another process has loaded, which matters because **Windows cannot delete or replace a loaded DLL**. Verified: `delete-while-loaded: FAILED as expected on Windows: AccessDeniedException`. On Linux the delete succeeds, as POSIX allows.
- **Verify before reuse**: an existing file is re-hashed and reused only if it matches, so a truncated or corrupted extraction is detected.
- **Concurrency**: each JVM writes to `Files.createTempFile(dir, ...)` and publishes with `ATOMIC_MOVE`. If the move fails because the target is loaded elsewhere, the identical existing file is accepted. Test: 12 JVMs started simultaneously on an empty cache. All 12 loaded the library and returned 42, and exactly one file remained with no temp leftovers (`$W/native-static/out/conc-*.log`).
- **Stale versions**: other hash dirs are deleted best-effort after loading. A failure means another process is still using that dir, so it is ignored.
- **Resource lookup**: `NativeLoader.class.getResourceAsStream("/natives/...")` resolves against the mod's own jar or module, so NeoForge's JPMS layer cannot hide it. The `windows-x86_64` segment is not a legal package name in any case.
- **Load lazily**: load on the first computer boot, not at mod init, so clients that never host never touch natives. If loading fails, fall back to `CobaltLuaMachine::new`, whose class and constructor are public (`CobaltLuaMachine.java:35,53`).
- **End to end**: `E2E.java` (`$W/e2e/`) uses only the classes from the built `ccluajit-fabric-0.2.0.jar`. It extracts `natives/windows-x86_64/cclj.dll` from the jar into the cache, loads it, and gets `jit=true` and `6*7 = 42`. The same flow with the NeoForge jar on Linux gives `/…/94ec272ef705331c/libcclj.so -> 42`.

Other loading notes:
- With a two-file layout (`cclj` plus `lua51.dll` / `libluajit-5.1.so`, as on 1.14.4), the load order matters and the second library is resolved by name or SONAME. Static linking removes that problem, removes clashes with any other Lua in the JVM, and drops the `vendor/jni-headers` submodule, because native runners use `$JAVA_HOME/include`.
- macOS: Temurin's hardened runtime has `allow-jit` and `disable-library-validation`, so unsigned or ad-hoc-signed dylibs and LuaJIT's `MAP_JIT` should work. The arm64 linker ad-hoc signs automatically. **Not verified here** (no Mac). The first CI run is the test.
- LuaJIT is MIT-licensed. Static linking is fine, but ship `vendor/luajit/COPYRIGHT` in the jar as the licence requires.

## E. CI

| | Native runner matrix (recommended) | Cross-compiling (zig cc / osxcross / mingw from Linux) |
|---|---|---|
| Correctness check | Each library is loaded into a real JVM on its own OS and arch by the smoke-test step | Cannot run the result. Windows could run under wine; macOS cannot run at all |
| LuaJIT build | Plain `make`, the well-trodden path | LuaJIT cross builds need a matching-bitness `HOST_CC` and `TARGET_SYS`/`CROSS` plumbing. zig cc works only with extra effort |
| macOS | Real Xcode CLT, real SDK | osxcross needs an Xcode SDK tarball (licence restricted to Apple hardware) |
| glibc baseline | manylinux_2_28 container on the x64 and arm64 runners | zig can pin a glibc version, which is the one advantage |
| Cost | Free for public repos (arm64 Linux GA for public repos; `macos-15-intel` may bill as a larger runner on private repos) | One Linux job |
| Maintenance | Runner labels churn: `macos-13` was retired Dec 2025, and `macos-15-intel` is the last x86_64 image (about fall 2027) | Toolchain churn |

**Recommendation: native runners.** Draft workflow: `$W/ci/build.yml` (copy at `$W/multiloader-1.21.1/.github/workflows/build.yml`). It parses as valid YAML (PyYAML) but has not run on GitHub.
- `natives-linux`: matrix `ubuntu-24.04` with `quay.io/pypa/manylinux_2_28_x86_64` and `ubuntu-24.04-arm` with `manylinux_2_28_aarch64`. Steps: `dnf install java-21-openjdk-devel`, then `build-natives.sh`, a glibc and exports check, a smoke test, and an upload.
- `natives-host`: `windows-2025` (msys2/setup-msys2 MINGW64 with `path-type: inherit`), `macos-15-intel` and `macos-15`, each with setup-java 21, `build-natives.sh`, a dependency check (only system DLLs on Windows; `otool -L` on macOS), a smoke test and an upload.
- `jar`: setup-java 21 and 25 (Gradle runs on 25 because of Loom), download `natives-*` with `merge-multiple` into `natives/out`, then `./gradlew build -Pcclj.requireAllNatives=true`, then upload both jars.
- If `macos-15-intel` disappears: build x86_64 on arm64 with `clang -arch x86_64` (a 64-to-64-bit LuaJIT cross build) and smoke-test under Rosetta.
- Action versions are `checkout@v5`, `setup-java@v5`, `upload/download-artifact@v4`, `setup-msys2@v2` and `gradle/actions/setup-gradle@v4`. Let Dependabot bump them.

`$W/natives/build-natives.sh` was run on Windows (Git Bash + WinLibs MinGW; log `$W/natives/build-natives-windows.log`) and on Linux (WSL Ubuntu; log `$W/natives/build-natives-linux-ubuntu.log`). Its macOS branch has not been run.

## F. Effort for the purely mechanical port (excluding the LuaJITMachine and native bridge rewrite)

| Task | Experienced modder | Modding newcomer | Notes |
|---|---|---|---|
| Gradle/Loom bump to 1.21.1 Fabric (JDK 25 for Gradle, Mojmap, CC:T deps) | 0.25 d (scaffold exists) / 0.5-1 d from scratch | 0.5-1 d / 2-3 d | The Loom ≥ 1.18 / JDK 25 trap is the main surprise |
| NeoForge plus multiloader structure | 0.5 d / 1 d | 1-2 d / 2-4 d | proven scaffold in `$W/multiloader-1.21.1` |
| Native build script, static linking, symbol hiding, glibc baseline | 0.5-1 d | 2-3 d | Windows and Linux proven; macOS untested |
| NativeLoader (extract, cache, concurrency, fallback) | 0.25 d / 0.5 d | 0.5-1 d / 1-2 d | prototype done and tested |
| CI matrix (5 natives, manylinux, msys2, artifact merge), first-run debugging, especially macOS | 1-1.5 d | 2-4 d | needs a GitHub push; each iteration takes about 10 min |
| In-game verification on both loaders, client plus dedicated server, Windows plus Linux | 0.5-1 d | 1-2 d | needs the user to accept the EULA for dev runs |
| Release plumbing (versioning, mod-page uploads, LICENSE/COPYRIGHT in jar) | 0.25-0.5 d | 0.5-1 d | |
| **Total** | **about 3-5 d with these scaffolds (5-8 d from scratch)** | **about 8-14 d (12-20 d from scratch)** | |

Reasoning: the Java side of the port is nearly trivial. The API is unchanged, and the mod touches one CC:T field and no Minecraft classes. The cost sits in the toolchain (JDK 25 for Gradle, two loader plugins) and above all in native packaging and CI across five platforms, where every macOS or arm64 fix costs a CI round-trip. A newcomer will also spend time learning Gradle, Loom, MDG and JNI linking.

## Findings

1. **[high] CC:T 1.121.0 forces Loom ≥ 1.18, so Gradle must run on JDK 25.** Verified by the Loom 1.17.21 failure (`Mod was built with a newer version of Loom (1.18.1)`). Fix: Loom 1.19.4 with Gradle 9.8.2 on JDK 25, `release 21`.
2. **[high] The 1.20.1 jar contains no natives, and `main.cpp` does not compile** (missing `<cassert>`). The Java build "succeeding" proves nothing about a working mod.
3. **[high] The Linux glibc baseline.** A build on a current distro needs GLIBC_2.38, which breaks Debian 12, Ubuntu 22.04 and RHEL 9 servers. Build in manylinux_2_28 (verified max GLIBC_2.14).
4. **[info] Static single-library natives work.** Windows DLL proven: system DLL imports only, `Java_*` exports only, loads on JDK 21 and 25, JIT on. Linux .so proven as well.
5. **[info] No Mixin is needed.** Setting `ServerContext.luaMachine` in `onInitialize()` or the `@Mod` constructor happens before `ServerContext.create` at server start (source-verified, not run in-game).
6. **[info] CC:T "forge" = NeoForge.** MDG 2.0.148 with NeoForge 21.1.265 compiles. A multiloader common module can be a plain java-library.
7. **[medium] Native extraction design.** Use a hash-named cache under the game dir, an atomic publish, re-verification, lazy loading and a Cobalt fallback. Verified with 12 concurrent JVMs and the Windows delete-lock behaviour. This replaces 1.14.4's temp-dir and `deleteOnExit` approach, which leaks files on Windows.
8. **[medium] Use native runners for CI, not cross-compiling.** `macos-13` is gone, and `macos-15-intel` is the last x64 image.
9. **[low] Metadata hygiene**: SNAPSHOT plugin, missing `computercraft` dependency, template contact URLs, empty mixin config. Also ship LuaJIT's COPYRIGHT.
10. **[low] Java 24+ users** will see JEP 472 native-access warnings. That is harmless on Java 21, which MC 1.21.1 uses.

## Artifacts (all under `$W`)
- Fabric scaffold: `fabric-1.21.1/{settings.gradle,build.gradle,gradle.properties,gradle/wrapper/gradle-wrapper.properties,src/main/resources/fabric.mod.json,src/main/java/gay/vereena/cclj/CCLuaJIT.java,src/main/java/gay/vereena/cclj/computer/LuaJITMachine.java}`
- NeoForge scaffold: `neoforge-1.21.1/{settings.gradle,build.gradle,gradle.properties,src/main/resources/META-INF/neoforge.mods.toml,src/main/java/gay/vereena/cclj/CCLuaJIT.java,...}`
- Multiloader: `multiloader-1.21.1/` (tree above), including `.github/workflows/build.yml`, `natives/build-natives.sh` and `natives/src/cclj_selftest.cpp`
- Native: `natives/build-natives.sh`, `native-static/src/cclj_probe.cpp`, `native-static/src/cclj_probe.c`, `native-static/java/gay/vereena/cclj/natives/{NativeLoader,NativeProbe}.java`, `native-static/out/cclj-windows-x86_64.dll`, `natives/out/{windows-x86_64/cclj.dll,linux-x86_64/libcclj.so}`, `natives/out-el8/linux-x86_64/libcclj.so`, `e2e/src/.../E2E.java`
- CI: `ci/build.yml`
- Logs: `build-1.20.1.log`, `build-fabric-1.21.1.log`, `build-fabric-1.21.1-loom1.17-jdk21.log` (the expected failure), `build-neoforge-1.21.1.log`, `build-multiloader-1.21.1*.log`, `natives/*.log`, `native-static/out/*.log`, `natives/main-cpp-check/compile{,2}.log`, `e2e/e2e-windows-final-fabric-jar.log`
- LuaJIT trees: `luajit/` (Windows static build), `luajit-lf/` (clean clone at 19878ec0), `luajit-linux-x64/`, `luajit-linux-el8/`
- Metadata snapshots: `meta/` (Loom, loader, Fabric API, Yarn, MDG metadata; CC:T `.module` files)
