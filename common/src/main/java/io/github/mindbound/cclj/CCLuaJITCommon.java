package io.github.mindbound.cclj;

import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.shared.computer.core.ServerContext;
import io.github.mindbound.cclj.computer.LuaJITMachine;
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
        nativeCache = gameDir.resolve(".ccluajit").resolve("natives");
        // CC:T reads this field in the ServerContext constructor, i.e. on server start (dedicated and integrated),
        // which is always after mod construction / onInitialize. No Mixin or access widener is needed: the field
        // is public, static and non-final (annotated @VisibleForTesting).
        var previous = ServerContext.luaMachine;
        if (previous != null && previous == installed) return;
        // Delegate to whatever factory was installed before ours (normally CC:T's default, Cobalt), so that an
        // unsupported platform or a failed native load keeps computers working.
        ILuaMachine.Factory fallback = previous != null ? previous : CobaltLuaMachine::new;
        ILuaMachine.Factory factory = (environment, bios) -> nativesAvailable()
            ? new LuaJITMachine(environment, bios)
            : fallback.create(environment, bios);
        installed = factory;
        ServerContext.luaMachine = factory;
    }

    /** Loads the natives lazily, on the first computer boot (so clients that never host never touch them). */
    private static boolean nativesAvailable() {
        var ok = nativesOk;
        if (ok != null) return ok;
        synchronized (CCLuaJITCommon.class) {
            if (nativesOk == null) {
                try {
                    LOG.info("Loaded LuaJIT natives from {}", NativeLoader.load(nativeCache));
                    nativesOk = true;
                } catch (NativeLoader.NotBundledException | UnsupportedOperationException e) {
                    // Expected on platforms this build does not ship natives for: one line, no stack trace.
                    LOG.warn("LuaJIT is not available on this platform ({}); computers use the previously installed "
                        + "Lua machine (normally Cobalt)", e.getMessage());
                    nativesOk = false;
                } catch (Exception | LinkageError e) {
                    LOG.error("Could not load LuaJIT natives; falling back to the previously installed Lua machine "
                        + "(normally Cobalt)", e);
                    nativesOk = false;
                }
            }
            return nativesOk;
        }
    }
}
