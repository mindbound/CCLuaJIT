import dan200.computercraft.api.lua.Coerced;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.ILuaContext;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.api.lua.LuaTask;
import dan200.computercraft.api.lua.ObjectArguments;
import dan200.computercraft.core.asm.LuaMethodSupplier;
import dan200.computercraft.core.methods.LuaMethod;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * JVM host for the LuaJIT -> JNI -> Java host-call benchmark. Loads lua51.dll + jnibench.dll and runs hostcall.lua.
 * Usage: java -cp ... JniBench <dir-with-dlls> <hostcall.lua> [jit|joff|leak]...
 */
public class JniBench {
    static long sink;
    static final String STR100 = "x".repeat(100);
    static final byte[] BYTES100 = STR100.getBytes(StandardCharsets.ISO_8859_1);

    // Field read by native code after every call, like LuaJITMachine.yieldRequested
    volatile boolean yieldRequested;

    // ---- CCLuaJIT-style string decoding (1.14.4 LuaJITMachine.decodeString uses US_ASCII) ----
    public static String decodeString(byte[] bytes) {
        return new String(bytes, StandardCharsets.US_ASCII);
    }

    // ---- CCLuaJIT-style dispatch: Object[] in, Object[] out (like ILuaObject.callMethod in CC:T 1.86) ----
    public Object[] callMethod(Object machine, int index, Object[] args) {
        switch (index) {
            case 0: return new Object[]{ ((Number) args[0]).doubleValue() + 1 };
            case 1: return args;
            case 2: return new Object[]{ STR100 };
            case 3: sink += ((String) args[0]).length(); return null;
            case 4: return null;
            case 5: {
                byte[] b = new byte[256];
                for (int i = 0; i < 256; i++) b[i] = (byte) i;
                return new Object[]{ b };
            }
            case 6: { // term.blit(text, fg, bg) shape: three strings in, nothing out
                String a = (String) args[0], b = (String) args[1], c = (String) args[2];
                if (a.length() != b.length() || b.length() != c.length()) throw new IllegalArgumentException("len");
                sink += a.length();
                return null;
            }
            default: throw new IllegalArgumentException("index");
        }
    }

    // ---- What a 1.21.1 port would plausibly do: CC:T LuaMethod + ObjectArguments + MethodResult ----
    public static class BenchAPI implements ILuaAPI {
        @Override public String[] getNames() { return new String[]{ "bench" }; }
        @LuaFunction public final double num(double x) { return x + 1; }
        @LuaFunction public final String strout() { return STR100; }
        @LuaFunction public final void strin(Coerced<String> s) { sink += s.value().length(); }
        @LuaFunction public final void nop() { }
        @LuaFunction public final void blit3(ByteBuffer a, ByteBuffer b, ByteBuffer c) throws LuaException {
            if (a.remaining() != b.remaining() || b.remaining() != c.remaining()) throw new LuaException("len");
            sink += a.remaining();
        }
    }

    static final BenchAPI API = new BenchAPI();
    static final LuaMethod[] PORT = new LuaMethod[7];
    static final ILuaContext CONTEXT = new ILuaContext() {
        @Override public long issueMainThreadTask(LuaTask task) throws LuaException { throw new LuaException("n/a"); }
    };

    static {
        Map<String, LuaMethod> m = new HashMap<>();
        LuaMethodSupplier.create(List.of()).forEachMethod(API, (target, name, method, info) -> m.put(name, method));
        PORT[0] = m.get("num");
        PORT[2] = m.get("strout");
        PORT[3] = m.get("strin");
        PORT[4] = m.get("nop");
        PORT[6] = m.get("blit3");
    }

    public Object[] callPort(Object machine, int index, Object[] args) throws LuaException {
        var result = PORT[index].apply(API, CONTEXT, new ObjectArguments(args));
        return result.getResult();
    }

    // ---- lean static callees ----
    public static void leanNop() { }
    public static double leanNum(double x) { return x + 1; }
    public static void leanStr(String s) { sink += s.length(); }
    public static void leanBytes(byte[] b) { sink += b.length; }
    public static void leanBuf(ByteBuffer b) { sink += b.remaining(); }
    public static String leanStrOut() { return STR100; }
    public static byte[] leanBytesOut() { return BYTES100; }
    public static void leanBlit3(byte[] a, byte[] b, byte[] c) {
        if (a.length != b.length || b.length != c.length) throw new IllegalArgumentException("len");
        sink += a.length;
    }

    static native String run(JniBench machine, String script, boolean jitOn);

    public static void main(String[] args) throws Exception {
        String dir = args[0];
        System.load(dir + "/lua51.dll");
        System.load(dir + "/jnibench.dll");
        String script = Files.readString(Path.of(args[1]));
        var machine = new JniBench();
        for (int i = 2; i < args.length; i++) {
            String mode = args[i];
            long t0 = System.nanoTime();
            String pre = "SECTION = '" + mode + "'\nJNIBENCH_DLL = '" + dir + "/jnibench.dll'\n";
            String r = run(machine, pre + script, !mode.endsWith("joff"));
            System.out.printf("RUN\t%s\t%s\t%.2fs%n", mode, r, (System.nanoTime() - t0) / 1e9);
        }
        System.out.println("SINK " + sink);
    }
}
