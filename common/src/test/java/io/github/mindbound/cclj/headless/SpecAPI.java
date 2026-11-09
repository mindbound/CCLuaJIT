package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.lua.IArguments;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineResult;

import java.util.StringJoiner;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The test API of {@link MachineSpecs}, the Lua global {@code t} (docs/port-1.21.1/M1.md section 3.15, Tier 2).
 * One instance per machine, so the counters and latches belong to one scenario.
 */
public final class SpecAPI implements ILuaAPI {
    /** Calls of {@code t.mark()}. */
    final AtomicInteger marks = new AtomicInteger();
    /** {@code t.block()} counts this down when it starts, then waits for {@link #release} (at most 10 s). */
    final CountDownLatch blocked = new CountDownLatch(1), release = new CountDownLatch(1);
    /** Set only when {@code t.block()} ended because the test counted {@link #release} down (no timeout, no interrupt). */
    volatile boolean released;
    /** The machine that {@code t.reenter()} resumes from inside its own host call. */
    volatile ILuaMachine machine;

    /** A result type that CC:T cannot convert: no {@code @LuaFunction} methods, not a table or a function. */
    static final class Weird {
        @Override
        public String toString() {
            return "SpecAPI.Weird";
        }
    }

    @Override
    public String[] getNames() {
        return new String[]{ "t" };
    }

    /** Returns its arguments unchanged. */
    @LuaFunction
    public final Object[] echo(IArguments args) throws LuaException {
        return args.getAll();
    }

    /** Throws {@code LuaException("lvl<level>", level)}. */
    @LuaFunction
    public final void lvl(int level) throws LuaException {
        throw new LuaException("lvl" + level, level);
    }

    /** Throws a {@code LuaException} with a null message: a nil error value. */
    @LuaFunction
    public final void nullmsg() throws LuaException {
        throw new LuaException(null);
    }

    /** Throws a Java exception: {@code Java Exception Thrown: java.lang.RuntimeException: kaboom}. */
    @LuaFunction
    public final void boom() {
        throw new RuntimeException("kaboom");
    }

    /** A Java string with chars of 256 and more: e-acute, the euro sign and a surrogate pair (4 chars). */
    @LuaFunction
    public final String str() {
        return "\u00e9\u20ac\ud83d\ude00";
    }

    /** A value of a type that CC:T cannot convert: nil in Lua, plus a WARN (D9). */
    @LuaFunction
    public final Object weird() {
        return new Weird();
    }

    /** {@code getStringCoerced} of each argument, joined with {@code |}. */
    @LuaFunction
    public final String coerce(IArguments args) throws LuaException {
        StringJoiner j = new StringJoiner("|");
        for (int i = 0; i < args.count(); i++) j.add(args.getStringCoerced(i));
        return j.toString();
    }

    /** Blocks the resume until the test counts {@link #release} down (or 10 s pass). */
    @LuaFunction
    public final void block() {
        blocked.countDown();
        try {
            if (release.await(10, TimeUnit.SECONDS)) released = true;
            else System.out.println("SpecAPI.block: not released in 10 s");
        } catch (InterruptedException e) {
            System.out.println("SpecAPI.block: interrupted");
            Thread.currentThread().interrupt();
        }
    }

    /** Resumes {@link #machine} from inside this host call: the machine must refuse. */
    @LuaFunction
    public final String reenter() {
        MachineResult r = machine.handleEvent(null, null);
        return "re-entry returned " + (r.isError() ? "an error: " + r.getMessage() : "a result");
    }

    /** Does nothing (for the call cost). */
    @LuaFunction
    public final void nop() {
    }

    /** Counts its calls ({@link #marks}). */
    @LuaFunction
    public final void mark() {
        marks.incrementAndGet();
    }
}
