package io.github.mindbound.cclj.computer;

import dan200.computercraft.api.lua.IDynamicLuaObject;
import dan200.computercraft.api.lua.ILuaAPI;
import dan200.computercraft.api.lua.ILuaFunction;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.api.lua.MethodResult;
import dan200.computercraft.api.lua.ObjectArguments;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.lua.MachineException;
import dan200.computercraft.core.lua.MachineResult;
import dan200.computercraft.core.methods.LuaMethod;
import io.github.mindbound.cclj.natives.NativeLibrary;
import io.github.mindbound.cclj.natives.NativeMachine;
import io.github.mindbound.cclj.natives.NativeMachineException;
import io.github.mindbound.cclj.natives.Wire;
import org.jspecify.annotations.Nullable;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.InputStream;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A CC:T Lua machine on LuaJIT (docs/port-1.21.1/M1.md sections 3.10 to 3.12). It holds one
 * {@link NativeMachine}: a LuaJIT state capped at 64 MiB with one table per API, whose methods call back into
 * {@link #call}, and the bios compiled into a coroutine.
 * <p>
 * S1 runs the bios up to its first yield and reports how it ended. Events (S2), aborts (S6a), tables (S3) and
 * yielding methods (S5) are not implemented yet: an event ends the machine with an error, and a table, a method
 * object or a yielding method gives a Lua error.
 * <p>
 * Threads: CC:T calls the constructor and {@link #handleEvent} on its worker threads, one call at a time, and
 * {@link #close} from any thread. {@link NativeMachine} keeps the lifecycle; this class keeps no state of its own
 * that could race.
 */
public final class LuaJITMachine implements ILuaMachine {
    private static final Logger LOG = LoggerFactory.getLogger(LuaJITMachine.class);

    /** The Lua heap cap of one computer (decision 8). */
    static final long MEMORY_LIMIT = 64L << 20;
    /** How much of an error message the WARN log keeps: the error value, and the traceback after it (D7). */
    static final int LOG_MESSAGE_MAX = 1024, LOG_TRACEBACK_MAX = 8192;
    /** How many classes {@link #unknown} remembers; past this, values of a class not seen yet are not logged. */
    static final int MAX_WARNED = 256;

    private static final SecureRandom SECURE = new SecureRandom();
    /** Classes of method results that became nil and were logged, in this JVM (D9: one WARN per class). */
    private static final Set<Class<?>> WARNED = ConcurrentHashMap.newKeySet();

    /** One API method: function index i of the spec is {@code entries[i]}. */
    private record Entry(Object target, LuaMethod method, String name) {
    }

    private final MachineEnvironment env;
    private final Entry[] entries;
    private final NativeMachine vm;

    /**
     * Creates the machine: one Lua table per API of {@code environment}, and the bios compiled (not run).
     *
     * @throws IOException      if reading the bios fails
     * @throws MachineException if the natives are not loaded, the bios does not compile, or anything else fails;
     *                          the computer shows the message, and turning it on again retries (F15)
     */
    public LuaJITMachine(MachineEnvironment environment, InputStream bios) throws IOException, MachineException {
        env = environment;
        byte[] biosBytes = bios.readAllBytes();
        if (!NativeLibrary.isLoaded()) throw new MachineException("CCLuaJIT natives are not loaded");
        try {
            // The spec numbers the methods across all APIs in the order they are added; entries uses the same order.
            List<Entry> list = new ArrayList<>();
            Wire.SpecWriter spec = new Wire.SpecWriter();
            for (ILuaAPI api : environment.apis()) {
                spec.api(Arrays.asList(api.getNames()), api.getModuleName());
                boolean any = environment.luaMethods().forEachMethod(api, (target, name, method, info) -> {
                    spec.method(name, info == null || !info.nonYielding());
                    list.add(new Entry(target, method, name));
                });
                // The API still gets its (empty) table, as in Cobalt.
                if (!any) LOG.warn("API {} does not provide any methods", api);
            }
            entries = list.toArray(new Entry[0]);
            vm = NativeMachine.create(this::call, MEMORY_LIMIT, SECURE.nextLong(), spec.toByteArray(), biosBytes);
        } catch (NativeMachineException e) {
            // The bios did not compile (its message, e.g. "bios.lua:1: unexpected symbol near '='"), or out of memory.
            throw new MachineException(e.getMessage());
        } catch (RuntimeException | Error e) {
            // An unchecked exception from a factory would wedge the computer until it is recreated (F15).
            LOG.error("CCLuaJIT could not start a machine", e);
            throw new MachineException("CCLuaJIT could not start: " + e);
        }
    }

    /**
     * S1: {@code eventName == null} resumes the bios (M1.md section 3.12). Any event closes the machine and returns an error
     * (events come in S2).
     *
     * @throws IllegalStateException if the machine has been closed
     */
    @Override
    public MachineResult handleEvent(@Nullable String eventName, @Nullable Object @Nullable [] arguments) {
        if (eventName != null) {
            vm.checkOpen();
            close();
            return MachineResult.error("CCLuaJIT: events are not implemented yet (S2)");
        }
        NativeMachine.Outcome outcome;
        try {
            outcome = vm.resume(null);
        } catch (IllegalStateException e) {
            throw e;   // closed, or already running: CC:T's contract
        } catch (Throwable t) {
            LOG.error("CCLuaJIT internal error", t);
            close();
            return MachineResult.error("CCLuaJIT internal error: " + t);
        }
        // close() during the resume: report what Cobalt reports when close() interrupts it. The machine is freed.
        if (outcome.closedWhileRunning()) return MachineResult.TIMEOUT;
        Wire.Message message = outcome.message() != null ? outcome.message() : new Wire.Message(Wire.NO_MESSAGE, -1);
        switch (outcome.status()) {
            case NativeMachine.ST_YIELD:
                return MachineResult.OK;
            case NativeMachine.ST_RETURNED:
                return MachineResult.GENERIC_ERROR;
            case NativeMachine.ST_ERROR:
                // The screen shows only the first 51 columns, so the log keeps the whole traceback (F21, D7).
                if (LOG.isWarnEnabled()) LOG.warn("Top level coroutine errored: {}", forLog(message));
                return MachineResult.error(message.text());
            default:
                LOG.error("CCLuaJIT internal error (status {}): {}", outcome.status(), forLog(message));
                return MachineResult.error("CCLuaJIT internal error: " + message.text());
        }
    }

    /** Pure Java: never touches native code, so it is safe from any thread at any time. */
    @Override
    public void printExecutionState(StringBuilder out) {
        out.append("CCLuaJIT machine: ").append(vm.stateName()).append('\n');
    }

    /** Any thread, any number of times. During a resume, the resume frees the machine when it returns. */
    @Override
    public void close() {
        vm.close();
    }

    /**
     * Runs host function {@code k} with the arguments in {@code request}, on the thread that runs the resume
     * ({@link NativeMachine.Host}). Never throws: every failure becomes an ERROR reply.
     */
    private byte[] call(int k, byte[] request) {
        if (k < 0 || k >= entries.length) return Wire.error("CCLuaJIT: no host function " + k, 0);
        Entry e = entries[k];
        try {
            MethodResult r = e.method().apply(e.target(), env.context(), new ObjectArguments(Wire.decodeArgs(request)));
            if (r.getCallback() != null) {
                return Wire.error("CCLuaJIT: " + e.name() + " yields; not supported yet (S5)", 0);
            }
            return Wire.values(r.getResult(), value -> unknown(e, value));
        } catch (LuaException x) {
            return Wire.error(x.getMessage(), x.hasLevel() ? x.getLevel() : 1);
        } catch (Wire.UnsupportedValue x) {
            return Wire.error(x.getMessage(), 0);
        } catch (Throwable t) {
            LOG.error("Error calling {} on {}", e.name(), e.target(), t);
            return Wire.error("Java Exception Thrown: " + t, 0);
        }
    }

    /**
     * A method result of a type that {@link Wire#values} does not know. A method object (S3) is refused; any other
     * value becomes nil, as in Cobalt, with one WARN per class (D9).
     */
    private void unknown(Entry e, Object value) {
        if (value instanceof ILuaFunction || value instanceof IDynamicLuaObject
            || env.luaMethods().forEachMethod(value, (target, name, method, info) -> { })) {
            throw new Wire.UnsupportedValue("CCLuaJIT: " + e.name() + " returned a Lua object; not supported yet (S3)");
        }
        Class<?> c = value.getClass();
        // Concurrent machines may pass the size check together, so the set can end a few classes over the limit.
        if (WARNED.size() < MAX_WARNED && WARNED.add(c)) {
            LOG.warn("Method {} returned a value of unknown type {}, which becomes nil (logged once per type)",
                e.name(), c.getName());
        }
    }

    /**
     * The message for the log, on one line: the error value cut to {@value #LOG_MESSAGE_MAX} characters and the
     * traceback to {@value #LOG_TRACEBACK_MAX}, each after escaping. A Lua program writes the error value, so control
     * characters are escaped.
     */
    static String forLog(Wire.Message message) {
        String text = message.text();
        int at = message.tracebackAt();
        if (at < 0) return escape(text, LOG_MESSAGE_MAX);
        return escape(text.substring(0, at), LOG_MESSAGE_MAX) + escape(text.substring(at), LOG_TRACEBACK_MAX);
    }

    /**
     * {@code s} with {@code \} doubled, newline, tab and CR as {@code \n}, {@code \t} and {@code \r}, and the other
     * C0 and C1 control characters and DEL as {@code \xHH}. Cut to at most {@code max} characters, the last three
     * of them {@code ...}, if it is longer.
     */
    static String escape(String s, int max) {
        StringBuilder b = new StringBuilder(Math.min(s.length() + 16, max + 4));
        int cut = 0;   // the last character boundary in b at or below max - 3; an escape is never split
        for (int i = 0; i < s.length(); i++) {
            if (b.length() <= max - 3) cut = b.length();
            char c = s.charAt(i);
            if (c == '\\') b.append("\\\\");
            else if (c == '\n') b.append("\\n");
            else if (c == '\t') b.append("\\t");
            else if (c == '\r') b.append("\\r");
            else if (c < 0x20 || (c >= 0x7f && c < 0xa0)) b.append(String.format("\\x%02x", (int) c));
            else b.append(c);
            if (b.length() > max) {
                b.setLength(cut);
                return b.append("...").toString();
            }
        }
        return b.toString();
    }
}
