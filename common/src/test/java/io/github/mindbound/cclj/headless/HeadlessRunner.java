package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.filesystem.Mount;
import dan200.computercraft.api.filesystem.WritableMount;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.core.ComputerContext;
import dan200.computercraft.core.computer.Computer;
import dan200.computercraft.core.computer.ComputerEnvironment;
import dan200.computercraft.core.computer.GlobalEnvironment;
import dan200.computercraft.core.computer.mainthread.MainThread;
import dan200.computercraft.core.filesystem.JarMount;
import dan200.computercraft.core.filesystem.WritableFileMount;
import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.metrics.MetricsObserver;
import dan200.computercraft.core.terminal.Terminal;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.ZipFile;

/**
 * Headless CC:T 1.121.0 runner: boots a REAL Computer (real ComputerThread scheduler, real bios.lua + ROM from the
 * core jar, real term/fs/os/peripheral/redstone APIs) with a pluggable ILuaMachine.Factory. No Minecraft classes.
 *
 * Usage:
 *   java -cp "<classes>:<core jar>:<core-api jar>:cobalt:guava:failureaccess:slf4j-api:slf4j-simple:fastutil[:ccluajit-common.jar]"
 *        (use ";" instead of ":" on Windows; the Gradle task headlessSmoke builds this for you)
 *        HeadlessRunner --core-jar <cc-tweaked-1.21.1-core-1.121.0.jar> [--factory cobalt|<fqcn>]
 *                       [--startup <file.lua>] [--type <line>]... [--timeout <s>] [--colour]
 * Gradle: ./gradlew :common:headlessSmoke runs src/test/resources/headless/smoke.lua under Cobalt.
 *
 * <fqcn> must have a public (MachineEnvironment, InputStream) constructor (= ILuaMachine.Factory shape).
 * The startup file is copied to /startup.lua of a fresh writable root mount, so the bios -> shell runs it on boot.
 * Lua can call headless.log(s), headless.exit(code) and headless.mainAdd(a, b) (a mainThread=true method, which
 * goes through MethodResult/LuaContext.issueMainThreadTask/"task_complete": exactly the turtle/peripheral path).
 * Exit code = the Lua exit code, 124 on timeout, 125 if the computer turned itself off without headless.exit.
 */
public final class HeadlessRunner {
    /** CC:T version for the host strings; the Gradle tasks pass -Dcct.version from gradle.properties. */
    static final String CCT_VERSION = System.getProperty("cct.version", "unknown");
    static final AtomicInteger exitCode = new AtomicInteger(-1);

    /** Test API exposed to Lua as the global `headless`. */
    public static final class HeadlessAPI implements ILuaAPI {
        @Override
        public String[] getNames() {
            return new String[]{ "headless" };
        }

        @LuaFunction
        public final void log(String s) {
            System.out.println("[lua] " + s);
            System.out.flush();
        }

        @LuaFunction
        public final void exit(int code) {
            exitCode.compareAndSet(-1, code);
        }

        /** Runs on the (simulated) server thread: the yield/trampoline path that turtles and inventories use. */
        @LuaFunction(mainThread = true)
        public final double mainAdd(double a, double b) throws LuaException {
            if (!Thread.currentThread().getName().equals("main")) throw new LuaException("not on main thread");
            return a + b;
        }
    }

    public static void main(String[] args) throws Exception {
        Path coreJar = null;
        String factoryName = "cobalt";
        Path startup = null;
        List<String> typed = new ArrayList<>();
        double timeoutS = 20;
        boolean colour = false;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--core-jar" -> coreJar = Path.of(args[++i]);
                case "--factory" -> factoryName = args[++i];
                case "--startup" -> startup = Path.of(args[++i]);
                case "--type" -> typed.add(args[++i]);
                case "--timeout" -> timeoutS = Double.parseDouble(args[++i]);
                case "--colour" -> colour = true;
                default -> throw new IllegalArgumentException("unknown arg " + args[i]);
            }
        }
        if (coreJar == null) throw new IllegalArgumentException("--core-jar is required");
        final File coreJarFile = coreJar.toFile();

        // 1. Global environment: ROM and bios come straight from the CC:T core jar (where they ship).
        GlobalEnvironment global = new GlobalEnvironment() {
            @Override public String getHostString() { return "ComputerCraft " + CCT_VERSION + " (headless)"; }
            @Override public String getUserAgent() { return "computercraft/" + CCT_VERSION + " headless"; }
            @Override public Mount getRomMount() {
                try { return new JarMount(coreJarFile, "data/computercraft/lua/rom"); }
                catch (IOException e) { throw new UncheckedIOException(e); }
            }
            @Override public InputStream getLuaBios() {
                try {
                    var zip = new ZipFile(coreJarFile);
                    var in = zip.getInputStream(zip.getEntry("data/computercraft/lua/bios.lua"));
                    return new java.io.FilterInputStream(in) {
                        @Override public void close() throws IOException { super.close(); zip.close(); }
                    };
                } catch (IOException e) { throw new UncheckedIOException(e); }
            }
        };

        // 2. Per-computer environment: a throwaway writable root mount (= the computer's own disk).
        Path root = Files.createTempDirectory("cc-headless-");
        if (startup != null) Files.copy(startup, root.resolve("startup.lua"));
        ComputerEnvironment computerEnv = new ComputerEnvironment() {
            @Override public int getDay() { return 1; }
            @Override public double getTimeOfDay() { return 6.0; }
            @Override public MetricsObserver getMetrics() { return MetricsObserver.discard(); }
            @Override public WritableMount createRootMount() { return new WritableFileMount(root.toFile(), 1_000_000); }
        };

        // 3. Context: real ComputerThread (1 worker, like the default config), a real MainThread we tick ourselves,
        //    no HTTP (avoids Netty), and the machine factory under test.
        ILuaMachine.Factory factory = factoryFor(factoryName);
        MainThread mainThread = new MainThread();
        ComputerContext ctx = ComputerContext.builder(global)
            .computerThreads(1)
            .mainThreadScheduler(mainThread)
            .luaFactory(factory)
            .disableHttp()
            .build();

        Terminal terminal = new Terminal(51, 19, colour);
        Computer computer = new Computer(ctx, computerEnv, terminal, 0);
        computer.addApi(new HeadlessAPI());

        // 4. Drive it like the server does: tick every 50 ms (timers/os.clock/events), tick the main thread.
        computer.turnOn();
        long t0 = System.nanoTime();
        long deadline = t0 + (long) (timeoutS * 1e9);
        boolean wasOn = false;
        int typedIdx = 0;
        long nextTypeAt = t0 + TimeUnit.MILLISECONDS.toNanos(1500);
        int result;
        while (true) {
            computer.tick();
            mainThread.tick();
            if (computer.isOn()) wasOn = true;
            if (typedIdx < typed.size() && System.nanoTime() > nextTypeAt && computer.isOn()) {
                for (char c : typed.get(typedIdx).toCharArray()) computer.queueEvent("char", new Object[]{ String.valueOf(c) });
                computer.queueEvent("key", new Object[]{ 257, false }); // keys.enter
                computer.queueEvent("key_up", new Object[]{ 257 });
                typedIdx++;
                nextTypeAt = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(500);
            }
            if (exitCode.get() >= 0) { result = exitCode.get(); break; }
            if (wasOn && !computer.isOn()) { result = 125; break; }
            // Computer.tick() only starts the executor; a machine that fails at once may never be seen "on".
            if (!wasOn && !computer.isOn() && System.nanoTime() - t0 > 2_000_000_000L) { result = 126; break; }
            if (System.nanoTime() > deadline) { result = 124; break; }
            Thread.sleep(50);
        }

        System.out.println("---- terminal (" + terminal.getWidth() + "x" + terminal.getHeight() + ") after "
            + String.format("%.2f", (System.nanoTime() - t0) / 1e9) + " s ----");
        for (int y = 0; y < terminal.getHeight(); y++) System.out.println("|" + terminal.getLine(y) + "|");
        System.out.println("---- exit " + result + " ----");

        computer.shutdown();
        for (int i = 0; i < 20 && computer.isOn(); i++) { computer.tick(); Thread.sleep(50); }
        boolean closed = ctx.close(2, TimeUnit.SECONDS);
        System.out.println("context closed cleanly: " + closed);
        System.exit(result);
    }

    static ILuaMachine.Factory factoryFor(String name) throws ReflectiveOperationException {
        if (name.equals("cobalt")) return CobaltLuaMachine::new;
        var ctor = Class.forName(name).getConstructor(MachineEnvironment.class, InputStream.class);
        return (env, bios) -> {
            try {
                return (ILuaMachine) ctor.newInstance(env, bios);
            } catch (java.lang.reflect.InvocationTargetException e) {
                if (e.getCause() instanceof dan200.computercraft.core.lua.MachineException me) throw me;
                if (e.getCause() instanceof IOException io) throw io;
                throw new RuntimeException(e.getCause());
            } catch (ReflectiveOperationException e) {
                throw new RuntimeException(e);
            }
        };
    }
}
