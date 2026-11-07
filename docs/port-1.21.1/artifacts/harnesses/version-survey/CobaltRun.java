import org.squiddev.cobalt.*;
import org.squiddev.cobalt.compiler.LoadState;
import org.squiddev.cobalt.function.RegisteredFunction;
import org.squiddev.cobalt.lib.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/**
 * Runs a Lua file under Cobalt with the same core globals CC:T 1.121.0's CobaltLuaMachine installs
 * (CoreLibraries.debugGlobals + Bit32Lib, _HOST), plus a raw __write(str) primitive, __clock(), and a
 * print() built on tostring. Used for differential tests against LuaJIT.
 * Usage: java -cp cobalt-0.9.9.jar;. CobaltRun file1.lua [file2.lua ...]  (run in order, same globals)
 */
public class CobaltRun {
    public static void main(String[] args) throws Exception {
        var state = LuaState.builder().build();
        var globals = state.globals();
        CoreLibraries.debugGlobals(state);
        Bit32Lib.add(state);
        globals.rawset("_HOST", ValueFactory.valueOf("ComputerCraft 1.121.0 (Minecraft 1.21.1)"));
        var out = new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16);
        RegisteredFunction.bind(globals, new RegisteredFunction[]{
            RegisteredFunction.ofV("__write", (s, a) -> {
                for (int i = 1; i <= a.count(); i++) {
                    ByteBuffer b = a.arg(i).checkLuaString().toBuffer();
                    byte[] bytes = new byte[b.remaining()];
                    b.get(bytes);
                    try {
                        out.write(bytes);
                    } catch (IOException e) {
                        throw new LuaError(e.toString());
                    }
                }
                return Constants.NONE;
            }),
            RegisteredFunction.ofV("__clock", (s, a) -> ValueFactory.valueOf(System.nanoTime() / 1e9)),
            RegisteredFunction.ofV("__readfile", (s, a) -> {
                try {
                    return ValueFactory.valueOf(Files.readAllBytes(Paths.get(a.arg(1).checkString())));
                } catch (IOException e) {
                    return Constants.NIL;
                }
            }),
        });
        var prelude = "print = function(...) local n = select('#', ...) local t = {...} "
            + "for i = 1, n do t[i] = tostring(t[i]) end "
            + "__write(table.concat(t, string.char(9), 1, n) .. string.char(10)) end";
        LuaThread.runMain(state, LoadState.load(state,
            new ByteArrayInputStream(prelude.getBytes(StandardCharsets.UTF_8)), "=prelude", globals));
        int rc = 0;
        for (var arg : args) {
            var path = Paths.get(arg);
            try {
                var fn = LoadState.load(state, new ByteArrayInputStream(Files.readAllBytes(path)),
                    "@" + path.getFileName(), globals);
                LuaThread.runMain(state, fn);
            } catch (Exception e) {
                out.flush();
                System.err.println("COBALT ERROR: " + e.getMessage());
                rc = 1;
                break;
            }
        }
        out.flush();
        System.exit(rc);
    }
}
