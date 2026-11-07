package io.github.mindbound.cclj.natives;

import java.nio.file.Path;

/** Loads the single statically-linked JNI+LuaJIT library and runs Lua through it. */
public final class NativeProbe {
    static native String eval(String chunk);
    static native String version();

    public static void main(String[] args) {
        Path lib = Path.of(args[0]).toAbsolutePath();
        long t0 = System.nanoTime();
        System.load(lib.toString());
        System.out.printf("System.load(%s) ok in %.1f ms%n", lib.getFileName(), (System.nanoTime() - t0) / 1e6);
        System.out.println("java.version = " + System.getProperty("java.version") + ", os.arch = " + System.getProperty("os.arch"));
        System.out.println("version()    = " + version());
        System.out.println("eval(1+2)    = " + eval("return 1 + 2"));
        System.out.println("eval(err)    = " + eval("error('boom')"));
        // A tiny JIT-able loop, timed in Lua with os.clock. Not a benchmark, just proof that traces compile and run.
        String loop = "local t=os.clock() local s=0 for i=1,100000000 do s=s+i%7 end "
                    + "return string.format('sum=%d in %.3f s', s, os.clock()-t)";
        System.out.println("eval(loop)   = " + eval(loop));
        System.out.println("eval(-joff)  = " + eval("jit.off() " + loop));
    }
}
