package io.github.mindbound.cclj.neoforge;

import io.github.mindbound.cclj.CCLuaJITCommon;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.ModContainer;
import net.neoforged.fml.common.Mod;
import net.neoforged.fml.loading.FMLPaths;

@Mod(CCLuaJITCommon.MOD_ID)
public final class CCLuaJITNeoForge {
    public CCLuaJITNeoForge(IEventBus modBus, ModContainer container) {
        CCLuaJITCommon.init(FMLPaths.GAMEDIR.get());
    }
}
