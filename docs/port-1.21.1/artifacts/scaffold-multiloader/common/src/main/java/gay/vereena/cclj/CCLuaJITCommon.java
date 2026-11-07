package gay.vereena.cclj;

import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.shared.computer.core.ServerContext;
import gay.vereena.cclj.computer.LuaJITMachine;
import gay.vereena.cclj.natives.NativeLoader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.nio.file.Path;

/** Called from each loader entrypoint. */
public final class CCLuaJITCommon {
    public static final String MOD_ID = "ccluajit";
    private static final Logger LOG = LoggerFactory.getLogger(MOD_ID);

    private static volatile Path nativeCache;
    private static volatile Boolean nativesOk;

    private CCLuaJITCommon() {
    }

    /**
     * @param gameDir FabricLoader#getGameDir() / FMLPaths.GAMEDIR - the natives are extracted below it, not into
     *                java.io.tmpdir (often noexec on Linux servers).
     */
    public static void init(Path gameDir) {
        nativeCache = gameDir.resolve(".ccluajit").resolve("natives");
        // CC:T reads this field in the ServerContext constructor, i.e. on server start (dedicated and integrated),
        // which is always after mod construction / onInitialize. No Mixin or access widener is needed: the field
        // is public, static and non-final (annotated @VisibleForTesting).
        ServerContext.luaMachine = (environment, bios) -> nativesAvailable()
            ? new LuaJITMachine(environment, bios)
            : new CobaltLuaMachine(environment, bios); // unsupported platform / load failure: keep computers working
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
                } catch (Throwable e) {
                    LOG.error("Could not load LuaJIT natives for this platform; falling back to Cobalt", e);
                    nativesOk = false;
                }
            }
            return nativesOk;
        }
    }
}
