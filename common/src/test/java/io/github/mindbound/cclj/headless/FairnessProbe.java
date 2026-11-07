package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.filesystem.Mount;
import dan200.computercraft.api.filesystem.WritableMount;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.core.ComputerContext;
import dan200.computercraft.core.computer.Computer;
import dan200.computercraft.core.computer.ComputerEnvironment;
import dan200.computercraft.core.computer.GlobalEnvironment;
import dan200.computercraft.core.filesystem.JarMount;
import dan200.computercraft.core.filesystem.WritableFileMount;
import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineResult;
import dan200.computercraft.core.metrics.MetricsObserver;
import dan200.computercraft.core.terminal.Terminal;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipFile;

/**
 * Time-slicing (PAUSE) acceptance probe: 2 computers on CC:T's default single worker thread.
 * Computer A runs 3 s of CPU without yielding; computer B measures how late its 50 ms timers fire meanwhile.
 * --nopause wraps the machine so it never returns PAUSE (loops handleEvent(null,null) internally), which is
 * exactly the behaviour of a LuaJIT machine without the pause-token protocol.
 * Usage: FairnessProbe --core-jar <jar> [--nopause]   (worker threads: -Dthreads=N, default 1)
 * Gradle: ./gradlew :common:fairnessProbe [-Pnopause] [-Pthreads=N]. Informational: always exits 0.
 */
public final class FairnessProbe {
    public static final class LogAPI implements ILuaAPI {
        final String who;
        LogAPI(String who) { this.who = who; }
        @Override public String[] getNames() { return new String[]{ "probe" }; }
        @LuaFunction public final void log(String s) { System.out.println("[" + who + "] " + s); System.out.flush(); }
    }

    public static void main(String[] args) throws Exception {
        File core = new File(args[1]);
        boolean noPause = args.length > 2 && args[2].equals("--nopause");
        GlobalEnvironment g = new GlobalEnvironment() {
            @Override public String getHostString() { return "ComputerCraft " + HeadlessRunner.CCT_VERSION + " (fairness)"; }
            @Override public String getUserAgent() { return "x"; }
            @Override public Mount getRomMount() {
                try { return new JarMount(core, "data/computercraft/lua/rom"); } catch (IOException e) { throw new UncheckedIOException(e); }
            }
            @Override public InputStream getLuaBios() {
                try {
                    var z = new ZipFile(core);
                    return new java.io.FilterInputStream(z.getInputStream(z.getEntry("data/computercraft/lua/bios.lua"))) {
                        @Override public void close() throws IOException { super.close(); z.close(); }
                    };
                } catch (IOException e) { throw new UncheckedIOException(e); }
            }
        };
        ILuaMachine.Factory factory = CobaltLuaMachine::new;
        if (noPause) {
            factory = (env, bios) -> {
                var inner = new CobaltLuaMachine(env, bios);
                return new ILuaMachine() {
                    @Override public MachineResult handleEvent(String n, Object[] a) {
                        var r = inner.handleEvent(n, a);
                        while (r.isPause()) r = inner.handleEvent(null, null);
                        return r;
                    }
                    @Override public void printExecutionState(StringBuilder out) { inner.printExecutionState(out); }
                    @Override public void close() { inner.close(); }
                };
            };
        }
        var ctx = ComputerContext.builder(g).computerThreads(Integer.getInteger("threads", 1)).luaFactory(factory).disableHttp().build();
        Computer a = make(ctx, "A", "sleep(1) probe.log('busy loop start') local t = os.clock() local x = 0 while os.clock() - t < 3 do x = x + 1 end probe.log('busy loop done (3 s CPU, no yield)') ");
        Computer b = make(ctx, "B",
            "sleep(0.3) local worst, sum = 0, 0 for i = 1, 60 do local t0 = os.epoch('utc') sleep(0.05) " +
            "local late = os.epoch('utc') - t0 - 50 worst = math.max(worst, late) sum = sum + late end " +
            "probe.log(('timer lateness over 60 sleeps: worst %d ms, mean %.0f ms'):format(worst, sum / 60))");
        a.turnOn(); b.turnOn();
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(14);
        while (System.nanoTime() < end) { a.tick(); b.tick(); Thread.sleep(50); }
        a.shutdown(); b.shutdown();
        for (int i = 0; i < 20; i++) { a.tick(); b.tick(); Thread.sleep(50); }
        System.out.println("mode=" + (noPause ? "never-PAUSE" : "cobalt-PAUSE") + " closed=" + ctx.close(2, TimeUnit.SECONDS));
        System.exit(0);
    }

    static Computer make(ComputerContext ctx, String who, String startup) throws IOException {
        Path root = Files.createTempDirectory("cc-fair-" + who);
        Files.writeString(root.resolve("startup.lua"), startup);
        ComputerEnvironment env = new ComputerEnvironment() {
            @Override public int getDay() { return 0; }
            @Override public double getTimeOfDay() { return 0; }
            @Override public MetricsObserver getMetrics() { return MetricsObserver.discard(); }
            @Override public WritableMount createRootMount() { return new WritableFileMount(root.toFile(), 1_000_000); }
        };
        var c = new Computer(ctx, env, new Terminal(51, 19, false), who.equals("A") ? 1 : 2);
        c.addApi(new LogAPI(who));
        return c;
    }
}
