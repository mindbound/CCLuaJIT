package gay.vereena.cclj.fabric;

import gay.vereena.cclj.CCLuaJITCommon;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.loader.api.FabricLoader;

public final class CCLuaJITFabric implements ModInitializer {
    @Override
    public void onInitialize() {
        CCLuaJITCommon.init(FabricLoader.getInstance().getGameDir());
    }
}
