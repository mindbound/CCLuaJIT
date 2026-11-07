package io.github.mindbound.cclj.computer;

import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.lua.MachineException;
import dan200.computercraft.core.lua.MachineResult;
import org.jspecify.annotations.Nullable;

import java.io.IOException;
import java.io.InputStream;

/** Port-mechanics stub: proves the CC:T 1.121.0 ILuaMachine contract compiles. The real VM bridge goes here. */
public final class LuaJITMachine implements ILuaMachine {
    private final MachineEnvironment environment;

    public LuaJITMachine(MachineEnvironment environment, InputStream bios) throws IOException, MachineException {
        this.environment = environment;
        bios.readAllBytes(); // the real implementation loads this chunk into LuaJIT
    }

    @Override
    public MachineResult handleEvent(@Nullable String eventName, @Nullable Object @Nullable [] arguments) {
        return MachineResult.error("CCLuaJIT: LuaJIT machine not implemented yet (" + environment.hostString() + ")");
    }

    @Override
    public void printExecutionState(StringBuilder out) {
        out.append("CCLuaJIT stub machine\n");
    }

    @Override
    public void close() {
    }
}
