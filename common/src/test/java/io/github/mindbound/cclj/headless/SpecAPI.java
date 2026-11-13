package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.lua.IArguments;
import dan200.computercraft.api.lua.IDynamicLuaObject;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.ILuaContext;
import dan200.computercraft.api.lua.ILuaFunction;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.LuaFunction;
import dan200.computercraft.api.lua.MethodResult;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineResult;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
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
    /**
     * Set only when {@code t.block()} ended because the test counted {@link #release} down (no timeout, no
     * interrupt).
     */
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

    /**
     * As {@link #reenter}, with the named event {@code "x"} (scenario 50): an event that passes the filter reaches
     * the resume, which must refuse with the same exception.
     */
    @LuaFunction
    public final String reenterEvent() {
        MachineResult r = machine.handleEvent("x", null);
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

    /** A result type that no scenario returns before scenario 27: its WARN (D9) is the first for the class. */
    @LuaFunction
    public final Object unseen() {
        return new Unseen();
    }

    /** The result of {@link #unseen}. */
    static final class Unseen {
    }

    /** A yielding result ({@code pullEvent}): LuaJIT refuses it until S5. */
    @LuaFunction
    public final MethodResult pull() {
        return MethodResult.pullEvent("spec", args -> MethodResult.of());
    }

    /** A table result: LuaJIT refuses it until S3. */
    @LuaFunction
    public final Object tbl() {
        return Map.of("a", 1);
    }

    /** A {@link ByteBuffer} result: LuaJIT refuses it until S3. */
    @LuaFunction
    public final Object buf() {
        return ByteBuffer.allocate(1);
    }

    /** An object with {@code @LuaFunction} methods: LuaJIT refuses it until S3. */
    @LuaFunction
    public final Object obj() {
        return new Obj();
    }

    /** An {@link ILuaFunction}: LuaJIT refuses it until S3. */
    @LuaFunction
    public final Object fn() {
        return (ILuaFunction) args -> MethodResult.of();
    }

    /** The result of {@link #obj}. */
    public static final class Obj {
        @LuaFunction
        public final int one() {
            return 1;
        }
    }

    // ---- Event values of the S2 scenarios (docs/port-1.21.1/S2.md section 6.3). Each class is used by one scenario
    // only: the WARN sets are JVM-wide, so a class seen before would not be logged again.

    /** Scenario 29: an argument of an event without a name, which is never converted. */
    static final class EvNullArg {
    }

    /** Scenario 41: a table in an event. */
    static final class EvMap extends HashMap<String, Object> {
    }

    /** Scenario 41: a list in an event. */
    static final class EvList extends ArrayList<Object> {
    }

    /** Scenario 42: a value of a type that neither machine knows. */
    static final class EvUnseen {
    }

    /** Scenario 40: a value in an event whose name the log must escape and cut. */
    static final class EvNamed {
    }

    /** Scenario 43: a dynamic Lua object in an event. */
    static final class EvDyn implements IDynamicLuaObject {
        @Override
        public String[] getMethodNames() {
            return new String[]{ "one" };
        }

        @Override
        public MethodResult callMethod(ILuaContext context, int method, IArguments arguments) {
            return MethodResult.of(1);
        }
    }

    /** Scenario 43: a function in an event. */
    static final class EvFn implements ILuaFunction {
        @Override
        public MethodResult call(IArguments arguments) {
            return MethodResult.of();
        }
    }

    /** Scenario 43: an object with a {@code @LuaFunction} method in an event. Public, so CC:T finds the method. */
    public static final class EvObj {
        @LuaFunction
        public final int one() {
            return 1;
        }
    }

    /** Scenario 40: a number whose conversion throws, as addon code can. */
    static final class EvBoom extends Number {
        @Override
        public int intValue() {
            return (int) doubleValue();
        }

        @Override
        public long longValue() {
            return (long) doubleValue();
        }

        @Override
        public float floatValue() {
            return (float) doubleValue();
        }

        @Override
        public double doubleValue() {
            throw new RuntimeException("boom");
        }
    }

    /**
     * Scenario 48: a number whose conversion closes the machine, as CC:T's monitor thread can while an event's
     * arguments are converted.
     */
    static final class EvCloser extends Number {
        private final SpecAPI api;

        EvCloser(SpecAPI api) {
            this.api = api;
        }

        @Override
        public int intValue() {
            return (int) doubleValue();
        }

        @Override
        public long longValue() {
            return (long) doubleValue();
        }

        @Override
        public float floatValue() {
            return (float) doubleValue();
        }

        @Override
        public double doubleValue() {
            api.machine.close();
            return 1;
        }
    }
}
