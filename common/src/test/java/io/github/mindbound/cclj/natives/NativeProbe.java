package io.github.mindbound.cclj.natives;

import java.net.URL;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Self-test of the native library (libcclj). Run by {@code ./gradlew :common:nativeProbe}.
 * <p>
 * Loads the library for this platform the way the mod does, through {@link NativeLoader} from
 * {@code natives/<platform>/<lib>} on the classpath, and calls the native self-test in natives/src/cclj.c. That
 * creates a lua_State and checks the JIT, a computation, that {@code luaJIT_interrupt} from a second thread stops a
 * JIT-compiled loop (patch 01), the C-stack guard (patch 08) and the bytecode refusal (patch 03). It reports one
 * {@code key=value} per line and {@code ok=true} or {@code ok=false} at the end.
 * <p>
 * This class lives in the test source set, so it is not in the mod jar, but the JNI name of {@link #selfTest()}
 * binds to it. Usage: {@code NativeProbe <cache-dir>}. Exit code 0 if {@code ok=true}; 1 if a check failed; 2 if
 * no library is bundled for this platform; 3 if the library could not be loaded.
 */
public final class NativeProbe {
    private NativeProbe() {
    }

    /** Implemented in natives/src/cclj.c. */
    static native String selfTest();

    public static void main(String[] args) {
        if (args.length != 1) {
            System.err.println("usage: NativeProbe <cache-dir>");
            System.exit(64);
        }
        Path cache = Path.of(args[0]).toAbsolutePath();
        System.out.println("java " + System.getProperty("java.version") + " (" + System.getProperty("java.vm.name")
            + ") on " + System.getProperty("os.name") + " " + System.getProperty("os.arch"));

        String platform;
        try {
            platform = NativeLoader.platformId();
        } catch (UnsupportedOperationException e) {
            System.err.println("nativeProbe: " + e.getMessage());
            System.exit(2);
            return;
        }
        String resource = "/natives/" + platform + "/" + NativeLoader.libraryFileName(platform);
        URL source = NativeLoader.class.getResource(resource);
        System.out.println("resource " + resource + ": " + (source != null ? source : "not on the classpath"));

        Path lib;
        long t0 = System.nanoTime();
        try {
            lib = NativeLoader.load(cache);
        } catch (NativeLoader.NotBundledException e) {
            System.err.println("nativeProbe: " + e.getMessage() + ". Build it first: bash natives/build-natives.sh "
                + platform);
            System.exit(2);
            return;
        } catch (Exception | LinkageError e) {
            System.err.println("nativeProbe: could not load the native library: " + e);
            System.exit(3);
            return;
        }
        System.out.printf("loaded %s in %.1f ms%n", lib, (System.nanoTime() - t0) / 1e6);

        t0 = System.nanoTime();
        String report = selfTest();
        System.out.printf("selfTest() returned in %.1f ms:%n", (System.nanoTime() - t0) / 1e6);
        Map<String, String> values = new LinkedHashMap<>();
        for (String line : report.split("\n")) {
            if (line.isEmpty()) continue;
            System.out.println("  " + line);
            int eq = line.indexOf('=');
            if (eq > 0) values.put(line.substring(0, eq), line.substring(eq + 1));
        }
        if (!"true".equals(values.get("ok"))) {
            System.err.println("nativeProbe: FAILED (ok=" + values.get("ok") + ")");
            System.exit(1);
        }
        System.out.println("nativeProbe: OK");
    }
}
