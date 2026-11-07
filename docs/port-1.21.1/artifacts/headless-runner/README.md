# Headless CC:T runner (WP 0)

Plain Java programs that boot a **real** CC:Tweaked 1.21.1-1.121.0 computer without Minecraft: the real `Computer`, `ComputerThread` scheduler, `MainThread`, bios and ROM (loaded from the CC:T core jar), and the real term/fs/os/peripheral/redstone APIs. Each one takes a pluggable `ILuaMachine.Factory`, so the same run can use Cobalt or our `LuaJITMachine`. No Loom, no Minecraft jar, no EULA. A boot to the shell takes about 1.6 s.

| File | What it does |
|---|---|
| `src/HeadlessRunner.java` | Boots one computer, runs a `startup.lua`, types input, prints the terminal. Lua can call `headless.log(s)`, `headless.exit(code)` and `headless.mainAdd(a, b)`. `mainAdd` is a `mainThread = true` method, so it goes through the same `MethodResult` / `issueMainThreadTask` / `"task_complete"` path as turtle and peripheral calls. |
| `src/CctSuiteRunner.java` | Runs CC:T's own Lua test suite (`mcfly.lua` + `spec/`) and prints one line per test. It is a JUnit-free equivalent of CC:T's `ComputerTestDelegate`, using the same `cct_test` protocol. Its fake peripherals and startup line are copied from that MPL-2.0 file, so this file carries an MPL-2.0 header (an earlier version of this README wrongly said nothing was copied). |
| `src/FairnessProbe.java` | Measures time-slicing. Computer A burns 3 s of CPU without yielding (the class Javadoc says 4 s; the code runs 3 s), while computer B measures how late its 50 ms timers fire. `--nopause` wraps the machine so it never returns `PAUSE`, which is how a LuaJIT machine without the pause protocol behaves. |
| `lua/smoke.lua` | Smoke test used with `HeadlessRunner --startup`. |
| `results/` | Reference results under Cobalt (2026-11-07), see below. `cp.txt` is the classpath that was used. |

## Classpath
`cc-tweaked-1.21.1-core-1.121.0.jar`, `cc-tweaked-1.21.1-core-api-1.121.0.jar`, `cobalt-0.9.9.jar`, `guava-32.1.2-jre.jar`, `failureaccess-1.0.1.jar`, `fastutil-8.5.12.jar`, `slf4j-api-2.0.9.jar`, `slf4j-simple`. In the real project, put these as `testImplementation` dependencies of the `common` module (CC:T's core POM brings most of them) and add a `JavaExec` task. PLAN.md WP 0 has the wiring.

```
java -cp "<classes>;<jars above>" HeadlessRunner --core-jar <core jar> --factory cobalt --startup lua/smoke.lua
java -cp "<classes>;<jars above>" CctSuiteRunner --core-jar <core jar> --test-rom <test-rom dir> --factory cobalt
java -cp "<classes>;<jars above>" FairnessProbe  --core-jar <core jar> [--nopause]
```
`--factory <fully.qualified.Class>` takes any class with a public `(MachineEnvironment, InputStream)` constructor.

## The test suite
`test-rom` is **not** included here. It is MPL-2.0 CC:T code. Get it from CC:T tag `v1.21.1-1.121.0` (commit `380c6fc6`), path `projects/core/src/test/resources/test-rom`, for example with a sparse checkout. When it is vendored into the project, keep its MPL-2.0 headers and put the licence text in `LICENSES/`.

## Reference results (Cobalt, 2026-11-07)
- `results/out-cobalt-smoke.txt`: boot to shell in 1.60 s. `version`, `sleep`, `mainAdd(2,3)=5` (main-thread task), `read()` fed by queued char/key events, binary fs round-trip, `bit32`, and `ls rom` all work.
- `results/suite-cobalt.tsv`: 1407 tests; 1390 pass, 12 pending, 5 errors. All 5 errors are in `http_spec` and come from `.disableHttp()`. With HTTP enabled, expect 1395.
- `results/fair-*.txt`, the worst lateness of computer B's 50 ms timers:

  | Configuration | Worst lateness |
  |---|---|
  | Cobalt with PAUSE | 76 ms |
  | never PAUSE, 1 worker thread (the CC:T default) | 3,717 ms |
  | never PAUSE, `computer_threads = 2` | 14 ms |
- The scaffold's stub `LuaJITMachine` shows "LuaJIT machine not implemented yet" and exits with code 126.
