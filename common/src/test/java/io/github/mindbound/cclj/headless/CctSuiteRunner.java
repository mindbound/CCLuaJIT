// SPDX-FileCopyrightText: 2019 The CC: Tweaked Developers
// SPDX-FileCopyrightText: 2026 Arets Paeglis
//
// SPDX-License-Identifier: MPL-2.0
//
// FakeModem, FakePeripheralHub and the startup line are copied from CC:T's
// projects/core/src/test/java/dan200/computercraft/core/ComputerTestDelegate.java (MPL-2.0), so this file is MPL-2.0
// rather than MIT. It is test code and is not part of the mod jar.

package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.filesystem.Mount;
import dan200.computercraft.api.filesystem.WritableMount;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.api.peripheral.IPeripheral;
import dan200.computercraft.core.ComputerContext;
import dan200.computercraft.core.computer.Computer;
import dan200.computercraft.core.computer.ComputerEnvironment;
import dan200.computercraft.core.computer.ComputerSide;
import dan200.computercraft.core.computer.GlobalEnvironment;
import dan200.computercraft.core.computer.mainthread.MainThread;
import dan200.computercraft.core.filesystem.FileMount;
import dan200.computercraft.core.filesystem.FileSystemException;
import dan200.computercraft.core.filesystem.JarMount;
import dan200.computercraft.core.filesystem.WritableFileMount;
import dan200.computercraft.core.metrics.MetricsObserver;
import dan200.computercraft.core.terminal.Terminal;
import org.jspecify.annotations.Nullable;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipFile;

/**
 * Runs CC:T's own Lua test-suite (test-rom/mcfly.lua + spec/, from the CC:T git tag v1.21.1-1.121.0,
 * projects/core/src/test/resources/test-rom) on a REAL headless CC:T Computer with a pluggable machine factory.
 * This is a JUnit-free re-implementation of CC:T's ComputerTestDelegate (same cct_test protocol). The fake peripherals
 * and the startup line are copied from it; see the licence header.
 *
 * Usage: CctSuiteRunner --core-jar <core jar> --test-rom <dir> [--factory cobalt|<fqcn>] [--timeout <s>] [--report <file>]
 *                       [--http] [--expected-failures <file>]
 * Gradle: ./gradlew :common:headlessSuite [-Pfactory=...] [-Phttp] [-PexpectedFailures=<file>|none]
 * Prints one line per test (status, name, message) and a summary. With --report, the per-test lines go to that file
 * (TSV) and only the non-"pass" lines are echoed to stdout.
 * --http enables CC:T's HTTP API (NettyHttp, default rules; needs network). Without it HTTP is disabled, so the
 * http_spec tests that use the http global error.
 * --expected-failures: a UTF-8 text file with one test name per line, in the report's form ("a -> b -> c"); blank
 * lines and lines starting with '#' are ignored. Those tests may fail or error.
 * Exit code 0 iff the suite finished, every registered test reported, every "fail"/"error" is listed, and every
 * listed test did fail or error (a listed test that passes, is pending or never ran makes the list stale: exit 1).
 */
public final class CctSuiteRunner {
    static final List<String> names = new ArrayList<>();
    static final Map<String, Integer> counts = new TreeMap<>();
    static final Map<String, String> statusByName = new LinkedHashMap<>();
    static int next = 0;
    static final AtomicBoolean finished = new AtomicBoolean();
    static volatile @Nullable Computer computer;
    static PrintStream report = System.out;

    public static final class FakeModem implements IPeripheral {
        @Override public String getType() { return "modem"; }
        @Override public boolean equals(@Nullable IPeripheral other) { return this == other; }
        @LuaFunction public final boolean isOpen(int channel) { return false; }
    }

    public static final class FakePeripheralHub implements IPeripheral {
        @Override public String getType() { return "peripheral_hub"; }
        @Override public boolean equals(@Nullable IPeripheral other) { return this == other; }
        @LuaFunction public final Collection<String> getNamesRemote() { return List.of("remote_1"); }
        @LuaFunction public final boolean isPresentRemote(String name) { return name.equals("remote_1"); }
        @LuaFunction public final Object[] getTypeRemote(String name) { return name.equals("remote_1") ? new Object[]{ "remote", "other_type" } : null; }
        @LuaFunction public final Object[] hasTypeRemote(String name, String type) { return name.equals("remote_1") ? new Object[]{ type.equals("remote") || type.equals("other_type") } : null; }
        @LuaFunction public final Object[] getMethodsRemote(String name) { return name.equals("remote_1") ? new Object[]{ List.of("func") } : null; }
    }

    public static final class CctTestAPI implements ILuaAPI {
        final Path testRom;
        CctTestAPI(Path testRom) { this.testRom = testRom; }

        @Override public String[] getNames() { return new String[]{ "cct_test" }; }

        @Override public void startup() {
            try {
                computer.getAPIEnvironment().getFileSystem().mount("test-rom", "test-rom", new FileMount(testRom));
            } catch (FileSystemException e) { throw new IllegalStateException(e); }
        }

        @LuaFunction public final void start(Map<?, ?> tests) {
            for (var k : tests.keySet()) names.add((String) k);
            Collections.sort(names);
            System.out.println("suite: " + names.size() + " tests registered");
            queueNext();
        }

        @LuaFunction public final void submit(Map<?, ?> tbl) {
            var status = String.valueOf(tbl.get("status"));
            counts.merge(status, 1, Integer::sum);
            var name = String.valueOf(tbl.get("name")).replace("\0", " -> ");
            statusByName.put(name, status);
            var msg = tbl.get("message") == null ? "" : String.valueOf(tbl.get("message")).replace('\n', ' ');
            if (msg.length() > 300) msg = msg.substring(0, 300) + "...";
            var line = status + "\t" + name + (msg.isEmpty() ? "" : "\t" + msg);
            report.println(line);
            if (report != System.out && !status.equals("pass")) System.out.println(line);
            queueNext();
        }

        @LuaFunction public final void finish(Optional<Map<?, ?>> lineCounts) { finished.set(true); }

        private static void queueNext() {
            // One test at a time (the event queue is bounded); a nil name makes mcfly leave its loop.
            if (next < names.size()) computer.queueEvent("cct_test_run", new Object[]{ names.get(next++) });
            else computer.queueEvent("cct_test_run", null);
        }
    }

    public static void main(String[] args) throws Exception {
        Path coreJar = null, testRom = null, expectedFile = null;
        String factoryName = "cobalt";
        double timeoutS = 300;
        boolean http = false;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--core-jar" -> coreJar = Path.of(args[++i]);
                case "--test-rom" -> testRom = Path.of(args[++i]);
                case "--factory" -> factoryName = args[++i];
                case "--timeout" -> timeoutS = Double.parseDouble(args[++i]);
                case "--report" -> {
                    var p = Path.of(args[++i]).toAbsolutePath();
                    Files.createDirectories(p.getParent());
                    report = new PrintStream(Files.newOutputStream(p), true, StandardCharsets.UTF_8);
                }
                case "--http" -> http = true;
                case "--expected-failures" -> expectedFile = Path.of(args[++i]);
                default -> throw new IllegalArgumentException("unknown arg " + args[i]);
            }
        }
        Set<String> expected = new LinkedHashSet<>();
        if (expectedFile != null) {
            for (var line : Files.readAllLines(expectedFile, StandardCharsets.UTF_8)) {
                var t = line.strip();
                if (!t.isEmpty() && !t.startsWith("#")) expected.add(t);
            }
        }
        System.out.println("factory=" + factoryName + " http=" + (http ? "enabled" : "disabled") + " expected failures="
            + (expectedFile == null ? "none" : expected.size() + " (" + expectedFile + ")"));
        final File coreJarFile = Objects.requireNonNull(coreJar).toFile();
        GlobalEnvironment global = new GlobalEnvironment() {
            @Override public String getHostString() { return "ComputerCraft " + HeadlessRunner.CCT_VERSION + " (headless test)"; }
            @Override public String getUserAgent() { return "computercraft/" + HeadlessRunner.CCT_VERSION; }
            @Override public Mount getRomMount() {
                try { return new JarMount(coreJarFile, "data/computercraft/lua/rom"); } catch (IOException e) { throw new UncheckedIOException(e); }
            }
            @Override public InputStream getLuaBios() {
                try {
                    var zip = new ZipFile(coreJarFile);
                    return new java.io.FilterInputStream(zip.getInputStream(zip.getEntry("data/computercraft/lua/bios.lua"))) {
                        @Override public void close() throws IOException { super.close(); zip.close(); }
                    };
                } catch (IOException e) { throw new UncheckedIOException(e); }
            }
        };
        Path root = Files.createTempDirectory("cc-suite-");
        Files.writeString(root.resolve("startup.lua"), "loadfile('test-rom/mcfly.lua', nil, _ENV)('test-rom/spec') cct_test.finish()");
        ComputerEnvironment cenv = new ComputerEnvironment() {
            @Override public int getDay() { return 0; }
            @Override public double getTimeOfDay() { return 0; }
            @Override public MetricsObserver getMetrics() { return MetricsObserver.discard(); }
            @Override public WritableMount createRootMount() { return new WritableFileMount(root.toFile(), 10_000_000); }
        };
        var mainThread = new MainThread();
        var builder = ComputerContext.builder(global).computerThreads(1).mainThreadScheduler(mainThread)
            .luaFactory(HeadlessRunner.factoryFor(factoryName));
        if (!http) builder.disableHttp();   // otherwise the builder's default: NettyHttp with its default config
        var ctx = builder.build();
        var term = new Terminal(80, 100, true);   // same size as ComputerTestDelegate
        var c = new Computer(ctx, cenv, term, 0);
        computer = c;
        c.getEnvironment().setPeripheral(ComputerSide.TOP, new FakeModem());
        c.getEnvironment().setPeripheral(ComputerSide.BOTTOM, new FakePeripheralHub());
        c.addApi(new CctTestAPI(Objects.requireNonNull(testRom)));
        c.turnOn();

        long t0 = System.nanoTime(), deadline = t0 + (long) (timeoutS * 1e9);
        boolean wasOn = false;
        while (!finished.get() && System.nanoTime() < deadline) {
            c.tick();
            mainThread.tick();
            if (c.isOn()) wasOn = true; else if (wasOn) break;
            // A machine that fails at boot never turns on: give up after the boot timeout, not the suite timeout.
            if (!wasOn && System.nanoTime() - t0 > HeadlessRunner.BOOT_TIMEOUT_S * 1_000_000_000L) break;
            Thread.sleep(50);
        }
        report.flush();
        System.out.printf("suite done=%s after %.1f s; registered=%d run=%d counts=%s%n", finished.get(),
            (System.nanoTime() - t0) / 1e9, names.size(), counts.values().stream().mapToInt(x -> x).sum(), counts);
        if (!finished.get()) {
            System.out.println("---- terminal (last non-empty lines) ----");
            for (int y = 0; y < term.getHeight(); y++) { var l = term.getLine(y).toString().stripTrailing(); if (!l.isEmpty()) System.out.println(l); }
        }
        c.shutdown();
        for (int i = 0; i < 20 && c.isOn(); i++) { c.tick(); Thread.sleep(50); }
        System.out.println("context closed cleanly: " + ctx.close(2, TimeUnit.SECONDS));

        // Verdict: every fail/error must be listed, and every listed test must have failed or errored.
        int run = counts.values().stream().mapToInt(x -> x).sum();
        var unexpected = new ArrayList<String>();
        for (var e : statusByName.entrySet()) {
            if (isFailure(e.getValue()) && !expected.contains(e.getKey())) unexpected.add(e.getKey());
        }
        var stale = new ArrayList<String>();
        for (var name : expected) {
            var s = statusByName.get(name);
            if (!isFailure(s)) stale.add(name + " (" + (s == null ? "not run" : s) + ")");
        }
        System.out.printf("verdict: %d unexpected failure(s), %d expected failure(s), %d stale expected-failure entr%s%n",
            unexpected.size(), expected.size() - stale.size(), stale.size(), stale.size() == 1 ? "y" : "ies");
        for (var n : unexpected) System.out.println("  UNEXPECTED FAILURE: " + n);
        for (var n : stale) System.out.println("  STALE ENTRY (listed, but did not fail): " + n);
        if (finished.get() && run != names.size()) System.out.println("  only " + run + " of " + names.size() + " registered tests reported");
        if (!finished.get()) {
            System.out.println("  suite did not finish: " + (!wasOn ? "the computer did not turn on within " + HeadlessRunner.BOOT_TIMEOUT_S
                    + " s (machine failed at boot? -PbootTimeout raises the limit)"
                : c.isOn() ? "timed out after " + timeoutS + " s" : "the computer turned off before the suite finished"));
        }
        if (finished.get() && names.isEmpty()) System.out.println("  the suite registered no tests");
        boolean ok = finished.get() && !names.isEmpty() && run == names.size() && unexpected.isEmpty() && stale.isEmpty();
        System.out.println(ok ? "RESULT: PASS" : "RESULT: FAIL");
        System.exit(ok ? 0 : 1);
    }

    private static boolean isFailure(@Nullable String status) {
        return "fail".equals(status) || "error".equals(status);
    }
}
