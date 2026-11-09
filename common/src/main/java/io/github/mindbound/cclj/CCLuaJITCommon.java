package io.github.mindbound.cclj;

import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.shared.computer.core.ServerContext;
import io.github.mindbound.cclj.computer.LuaJITMachine;
import io.github.mindbound.cclj.natives.NativeLibrary;
import io.github.mindbound.cclj.natives.NativeLoader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.nio.file.Path;

/** Called from each loader entrypoint. */
public final class CCLuaJITCommon {
    public static final String MOD_ID = "ccluajit";
    private static final Logger LOG = LoggerFactory.getLogger(MOD_ID);

    private static volatile Path nativeCache;
    private static volatile Boolean nativesOk;
    /** The factory that {@link #init} installed (guarded by the class lock), so a second init call is a no-op. */
    private static ILuaMachine.Factory installed;

    private CCLuaJITCommon() {
    }

    /**
     * @param gameDir FMLPaths.GAMEDIR (or FabricLoader#getGameDir() later) - the natives are extracted below it,
     *                not into java.io.tmpdir (often noexec on Linux servers).
     */
    public static synchronized void init(Path gameDir) {
        // CC:T reads this field in the ServerContext constructor, i.e. on server start (dedicated and integrated),
        // which is always after mod construction / onInitialize. No Mixin or access widener is needed: the field
        // is public, static and non-final (annotated @VisibleForTesting).
        var previous = ServerContext.luaMachine;
        if (previous != null && previous == installed) return;   // the first call's cache directory stays too
        nativeCache = gameDir.resolve(".ccluajit").resolve("natives");
        // Delegate to whatever factory was installed before ours (normally CC:T's default, Cobalt), so that an
        // unsupported platform or a failed native load keeps computers working.
        ILuaMachine.Factory fallback = previous != null ? previous : CobaltLuaMachine::new;
        ILuaMachine.Factory factory = (environment, bios) -> nativesAvailable()
            ? new LuaJITMachine(environment, bios)
            : fallback.create(environment, bios);
        installed = factory;
        ServerContext.luaMachine = factory;
    }

    /**
     * Loads the natives lazily, on the first computer boot (so clients that never host never touch them), through
     * {@link NativeLibrary}: once per JVM, with the ABI check. A library whose ABI does not match fails with
     * {@link UnsatisfiedLinkError}, so computers use the fallback.
     * <p>
     * Never throws: an unchecked error from a factory would wedge the computer (M1.md F15). Every failure gives the
     * fallback for good, except a {@link VirtualMachineError} (out of memory, say), which gives the fallback for this
     * boot only: the next boot tries again, as {@link NativeLibrary} does not remember it either. The result is set
     * before anything is logged, and no log call can throw out of here (under memory pressure, logging can fail too).
     */
    private static boolean nativesAvailable() {
        var ok = nativesOk;
        if (ok != null) return ok;
        synchronized (CCLuaJITCommon.class) {
            if (nativesOk == null) {
                Path loaded;
                try {
                    loaded = NativeLibrary.ensureLoaded(nativeCache);
                } catch (NativeLoader.NotBundledException | UnsupportedOperationException e) {
                    nativesOk = false;
                    // Expected on platforms this build does not ship natives for: one line, no stack trace.
                    logQuietly(false, "LuaJIT is not available on this platform; computers use the previously "
                        + "installed Lua machine (normally Cobalt)", e);
                    return false;
                } catch (VirtualMachineError e) {
                    logQuietly(true, "Could not load LuaJIT natives this time; this computer uses the previously "
                        + "installed Lua machine (normally Cobalt), and the next boot tries again", e);
                    return false;
                } catch (Throwable e) {
                    nativesOk = false;
                    logQuietly(true, "Could not load LuaJIT natives; falling back to the previously installed Lua "
                        + "machine (normally Cobalt)", e);
                    return false;
                }
                nativesOk = true;
                try {
                    LOG.info("Loaded LuaJIT natives from {}", loaded);
                } catch (Throwable ignored) {
                    // The natives are loaded; a lost log line does not change that.
                }
            }
            return nativesOk;
        }
    }

    /**
     * Logs a failure to load the natives, and swallows anything the logging throws: the fallback matters more than
     * the log line. With {@code withTrace} false, only the exception's message is logged (no stack trace).
     */
    private static void logQuietly(boolean withTrace, String message, Throwable e) {
        try {
            if (withTrace) LOG.error(message, e);
            else LOG.warn("{} ({})", message, e.getMessage());
        } catch (Throwable ignored) {
            // Out of memory again while logging.
        }
    }
}
