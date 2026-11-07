import dan200.computercraft.api.lua.Coerced;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.ILuaContext;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.api.lua.LuaTask;
import dan200.computercraft.core.asm.LuaMethodSupplier;
import dan200.computercraft.core.computer.TimeoutState;
import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.lua.MachineResult;
import dan200.computercraft.core.metrics.MetricsObserver;

import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Runs Lua benchmark scripts inside CC:T 1.121.0's real CobaltLuaMachine (Cobalt 0.9.9), i.e. with exactly the
 * same LuaState construction (interrupt handler, CoreLibraries.debugGlobals, Bit32Lib, ErrorInfoLib) and the same
 * ILuaAPI -> LuaMethod -> ResultInterpreterFunction call path that CC:T uses for term/fs/peripheral APIs.
 *
 * The script is passed as the "bios" and run with handleEvent(null, null).
 *
 * Usage: java -cp ... CobaltBench [--prefix "lua code"] file1.lua [file2.lua ...]
 */
public class CobaltBench {
    static long sink;
    static final String STR100 = "x".repeat(100);

    /** Exposed to Lua as the global table "bench". Methods are dispatched by CC:T's generated LuaMethod. */
    public static class BenchAPI implements ILuaAPI {
        @Override
        public String[] getNames() {
            return new String[]{ "bench" };
        }

        @LuaFunction
        public final double clock() {
            return System.nanoTime() / 1e9;
        }

        @LuaFunction
        public final void print(String s) {
            System.out.println(s);
            System.out.flush();
        }

        /** zero-arg, no result */
        @LuaFunction
        public final void nop() {
        }

        /** number in, number out (boxed via MethodResult.of(Object)) */
        @LuaFunction
        public final double num(double x) {
            return x + 1;
        }

        /** string in, like term.write(Coerced<String>) */
        @LuaFunction
        public final void strin(Coerced<String> s) {
            sink += s.value().length();
        }

        /** bytes in, like term.blit(ByteBuffer...) */
        @LuaFunction
        public final void bytesin(ByteBuffer b) {
            sink += b.remaining();
        }

        /** 100-char string out */
        @LuaFunction
        public final String strout() {
            return STR100;
        }

        /** like term.blit: three ByteBuffers */
        @LuaFunction
        public final void blit3(ByteBuffer a, ByteBuffer b, ByteBuffer c) throws LuaException {
            if (a.remaining() != b.remaining() || b.remaining() != c.remaining()) throw new LuaException("len");
            sink += a.remaining();
        }
    }

    static final class NoTimeout extends TimeoutState {
        @Override
        public void refresh() {
        }
    }

    static final ILuaContext CONTEXT = new ILuaContext() {
        @Override
        public long issueMainThreadTask(LuaTask task) throws LuaException {
            throw new LuaException("main thread tasks not supported in benchmark");
        }
    };

    public static void main(String[] args) throws Exception {
        String prefix = "";
        int rounds = 1;
        List<Path> files = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--prefix" -> prefix = args[++i];
                case "--rounds" -> rounds = Integer.parseInt(args[++i]);
                default -> files.add(Path.of(args[i]));
            }
        }
        var methods = LuaMethodSupplier.create(List.of());
        for (int round = 1; round <= rounds; round++) {
            for (var file : files) {
                byte[] src = (prefix + "\n" + new String(Files.readAllBytes(file), java.nio.charset.StandardCharsets.ISO_8859_1))
                    .getBytes(java.nio.charset.StandardCharsets.ISO_8859_1);
                var env = new MachineEnvironment(CONTEXT, MetricsObserver.discard(), new NoTimeout(),
                    List.of(new BenchAPI()), methods, "ComputerCraft 1.121.0 (bench)");
                System.out.println("ROUND\t" + round + "\t" + file.getFileName());
                long t0 = System.nanoTime();
                var machine = new CobaltLuaMachine(env, new ByteArrayInputStream(src));
                MachineResult result = machine.handleEvent(null, null);
                long t1 = System.nanoTime();
                // A finished bios returns GENERIC_ERROR with a null message; a Lua error carries a message.
                if (result.getMessage() != null) System.out.println("MACHINE_ERROR\t" + result.getMessage());
                System.out.printf("MACHINE_TOTAL\t%s\t%.3f%n", file.getFileName(), (t1 - t0) / 1e9);
                machine.close();
            }
        }
        System.out.println("SINK " + sink);
    }
}
