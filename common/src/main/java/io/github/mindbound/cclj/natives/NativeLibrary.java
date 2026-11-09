package io.github.mindbound.cclj.natives;

import java.io.IOException;
import java.nio.file.Path;

/**
 * Loads libcclj at most once per JVM and checks its ABI (docs/port-1.21.1/M1.md section 3.10).
 * <p>
 * {@link NativeLoader#load} does not remember what it loaded, and a second cache root would load a second copy of
 * the library, with its own copy of every native method. So all code that needs the natives goes through
 * {@link #ensureLoaded}: the first call loads the library and compares {@code NativeMachine.nAbi()} with
 * {@link NativeMachine#ABI}; every later call returns that result. Success and failure are both sticky: a failure is
 * rethrown, the same exception object, on every later call.
 * <p>
 * A library from an older build without {@code nAbi} fails with {@link UnsatisfiedLinkError}, like a library whose
 * ABI does not match.
 */
public final class NativeLibrary {
    private static volatile Path loaded;   // written once, under the class lock
    private static Throwable failure;      // guarded by the class lock

    private NativeLibrary() {
    }

    /**
     * Loads the library for this platform through {@link NativeLoader#load} (extracting it below
     * {@code cacheRoot}), once per JVM, and checks its ABI.
     *
     * @return the path the library was loaded from (by the first successful call; later calls ignore
     *         {@code cacheRoot})
     * @throws NativeLoader.NotBundledException if this build has no library for this platform
     * @throws UnsupportedOperationException    on a platform NativeLoader does not know
     * @throws UnsatisfiedLinkError             if the library could not be loaded or its ABI does not match
     * @throws IOException                      if extracting it failed
     */
    public static synchronized Path ensureLoaded(Path cacheRoot) throws IOException {
        Path p = loaded;
        if (p != null) return p;
        if (failure != null) throw rethrow(failure);
        try {
            p = NativeLoader.load(cacheRoot);
            int abi = NativeMachine.nAbi();
            if (abi != NativeMachine.ABI) {
                throw new UnsatisfiedLinkError("native ABI " + abi + " of " + p + " does not match the Java ABI "
                    + NativeMachine.ABI + " (a library from another build of CCLuaJIT?)");
            }
        } catch (IOException | RuntimeException | Error e) {
            failure = e;
            throw e;
        }
        loaded = p;
        return p;
    }

    /** True once {@link #ensureLoaded} has succeeded. */
    public static boolean isLoaded() {
        return loaded != null;
    }

    private static IOException rethrow(Throwable t) {
        if (t instanceof IOException e) return e;
        if (t instanceof RuntimeException e) throw e;
        if (t instanceof Error e) throw e;
        throw new IllegalStateException(t);
    }
}
