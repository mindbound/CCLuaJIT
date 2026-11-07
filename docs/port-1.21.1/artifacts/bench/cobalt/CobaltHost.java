import dan200.computercraft.core.asm.LuaMethodSupplier;
import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.metrics.MetricsObserver;
import org.squiddev.cobalt.*;
import org.squiddev.cobalt.compiler.LoadState;
import org.squiddev.cobalt.function.LibFunction;
import org.squiddev.cobalt.interrupt.InterruptAction;
import org.squiddev.cobalt.lib.Bit32Lib;
import org.squiddev.cobalt.lib.CoreLibraries;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** Host-call overhead in Cobalt: (1) real CC:T CobaltLuaMachine + ILuaAPI path, (2) raw Cobalt LibFunctions. */
public class CobaltHost {
    static long sink;
    static final LuaString S100 = ValueFactory.valueOf("x".repeat(100));

    public static void main(String[] args) throws Exception {
        String script = Files.readString(Path.of(args[0]));

        // (1) raw Cobalt state built like CobaltLuaMachine (interrupt handler, debugGlobals, bit32)
        var state = LuaState.builder().interruptHandler(() -> InterruptAction.CONTINUE).build();
        CoreLibraries.debugGlobals(state);
        Bit32Lib.add(state);
        var g = state.globals();
        var bench = new LuaTable();
        bench.rawset("clock", LibFunction.create(s -> ValueFactory.valueOf(System.nanoTime() / 1e9)));
        bench.rawset("print", LibFunction.create((s, a) -> { System.out.println(a.checkString()); return Constants.NIL; }));
        g.rawset("bench", bench);
        var raw = new LuaTable();
        raw.rawset("nop", LibFunction.create(s -> Constants.NIL));
        raw.rawset("num", LibFunction.create((s, a) -> ValueFactory.valueOf(a.checkDouble() + 1)));
        raw.rawset("strin", LibFunction.create((s, a) -> { sink += a.checkLuaString().length(); return Constants.NIL; }));
        raw.rawset("strout", LibFunction.create(s -> S100));
        g.rawset("raw", raw);
        g.rawset("MODE", ValueFactory.valueOf("cobalt-raw"));
        String rawScript = script;
        var fn = LoadState.load(state, new ByteArrayInputStream(rawScript.getBytes(StandardCharsets.ISO_8859_1)), "@hostcall_raw", g);
        LuaThread.runMain(state, fn);

        // (2) real CC:T machine; `bench` is CobaltBench.BenchAPI dispatched via generated LuaMethods
        var env = new MachineEnvironment(CobaltBench.CONTEXT, MetricsObserver.discard(), new CobaltBench.NoTimeout(),
            List.of(new CobaltBench.BenchAPI()), LuaMethodSupplier.create(List.of()), "bench");
        byte[] src = ("MODE = 'cobalt-cct'\n" + script).getBytes(StandardCharsets.ISO_8859_1);
        var machine = new CobaltLuaMachine(env, new ByteArrayInputStream(src));
        var result = machine.handleEvent(null, null);
        if (result.getMessage() != null) System.out.println("MACHINE_ERROR\t" + result.getMessage());
        System.out.println("SINK " + sink + " " + CobaltBench.sink);
    }
}
