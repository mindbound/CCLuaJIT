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
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A CC:T Lua machine on LuaJIT (docs/port-1.21.1/M1.md sections 3.10 to 3.12). It holds one
 * {@link NativeMachine}: a LuaJIT state capped at 64 MiB with one table per API, whose methods call back into
 * {@link #call}, and the bios compiled into a coroutine.
 * <p>
 * It delivers events (S2, docs/port-1.21.1/S2.md section 4.5): {@link #handleEvent} encodes the name and the
 * arguments with {@link Wire#event} and resumes the bios with them, and keeps the event filter that each YIELD gives.
 * Aborts (S6a), tables and method objects (S3) and yielding methods (S5) are not implemented yet: in a method result,
 * a table, a method object or a yielding method gives a Lua error; in an event, a table or a method object becomes
 * nil, with one WARN per class.
 * <p>
 * Threads: CC:T calls the constructor and {@link #handleEvent} on its worker threads, one call at a time, and
 * {@link #close} from any thread. {@link NativeMachine} keeps the lifecycle. The one piece of per-machine mutable
 * state is {@link #filter}: only a {@code handleEvent} writes it, after a YIELD, and the next one reads it, maybe on
 * another worker, so it is volatile. The static {@code WARNED} and {@code WARNED_EVENTS} sets, which all machines
 * share, are concurrent sets.
 */
public final class LuaJITMachine implements ILuaMachine {
    private static final Logger LOG = LoggerFactory.getLogger(LuaJITMachine.class);

    /** The Lua heap cap of one computer (decision 8). */
    static final long MEMORY_LIMIT = 64L << 20;
    /** How much of an error message the WARN log keeps: the error value, and the traceback after it (D7). */
    static final int LOG_MESSAGE_MAX = 1024, LOG_TRACEBACK_MAX = 8192;
    /**
     * How many classes {@link #unknown} and {@link #unsupported} each remember; past this, values of a class not seen
     * yet are not logged.
     */
    static final int MAX_WARNED = 256;
    /** How much of an event name the log keeps. Names come from Lua ({@code os.queueEvent}), so they are escaped. */
    static final int LOG_NAME_MAX = 256;

    private static final SecureRandom SECURE = new SecureRandom();
    /** Classes of method results that became nil and were logged, in this JVM (D9: one WARN per class). */
    private static final Set<Class<?>> WARNED = ConcurrentHashMap.newKeySet();
    /** Classes of event values that became nil and were logged, in this JVM (D28: one WARN per class). */
    private static final Set<Class<?>> WARNED_EVENTS = ConcurrentHashMap.newKeySet();

    /** One API method: function index i of the spec is {@code entries[i]}. */
    private record Entry(Object target, LuaMethod method, String name) {
    }

    private final MachineEnvironment env;
    private final Entry[] entries;
    private final NativeMachine vm;
    /**
     * The event filter of the last YIELD, or null for none. Written only after a YIELD, by the {@code handleEvent}
     * that ran the resume; CC:T makes one call at a time, maybe each on another worker: volatile makes the write
     * visible to the next one.
     */
    private volatile Wire.@Nullable Filter filter;

    /**
     * Creates the machine: one Lua table per API of {@code environment}, and the bios compiled (not run).
     * <p>
     * Throws nothing but {@link IOException} and {@link MachineException}: anything else from a factory would wedge
     * the computer until it is recreated (F15). Only an allocation failure while the {@code MachineException} itself
     * is built can still escape.
     *
     * @throws IOException      if reading the bios fails (or addon code throws one without declaring it)
     * @throws MachineException if the natives are not loaded, the bios does not compile, or anything else fails;
     *                          the computer shows the message, and turning it on again retries (F15)
     */
    public LuaJITMachine(MachineEnvironment environment, InputStream bios) throws IOException, MachineException {
        env = environment;
        if (!NativeLibrary.isLoaded()) throw new MachineException("CCLuaJIT natives are not loaded");
        try {
            byte[] biosBytes = bios.readAllBytes();
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
                if (!any) {
                    try {
                        LOG.warn("API {} does not provide any methods", api);
                    } catch (Throwable ignored) {
                        // The machine matters more than the log line.
                    }
                }
            }
            entries = list.toArray(new Entry[0]);
            vm = NativeMachine.create(this::call, MEMORY_LIMIT, SECURE.nextLong(), spec.toByteArray(), biosBytes);
        } catch (IOException e) {
            // Reading the bios failed, or addon code threw one without declaring it: CC:T shows its own message
            // (M1.md section 3.10).
            throw e;
        } catch (NativeMachineException e) {
            // The bios did not compile (its message, e.g. "bios.lua:1: unexpected symbol near '='"), or out of memory.
            throw new MachineException(e.getMessage());
        } catch (Throwable e) {
            // Anything else, also a checked exception that addon code throws without declaring it. On every path
            // here, the native machine either does not exist yet or create has freed it.
            MachineException failure = new MachineException("CCLuaJIT could not start: " + describe(e));
            try {
                LOG.error("CCLuaJIT could not start a machine", e);
            } catch (Throwable ignored) {
                // Out of memory, or a throwable whose toString throws: the MachineException matters more.
            }
            throw failure;
        }
    }

    /**
     * Resumes the bios with an event, in Cobalt's order (docs/port-1.21.1/S2.md section 4.5, F24):
     * <ol>
     *   <li>a closed machine throws, also for an event the filter would skip;</li>
     *   <li>an event that the filter of the last YIELD does not pass returns {@code OK}: nothing is converted and
     *       the bios does not run ({@code "terminate"} always passes);</li>
     *   <li>the name and the arguments are encoded ({@link Wire#event}). An event with more than
     *       {@value Wire#MAX_EVENT_ARGS} arguments, one over 16 MiB, or one whose arguments throw while they are read
     *       is dropped: one WARN, {@code OK}, and the filter stays (D19, D30). A table or a method object becomes
     *       nil, with one WARN per class (D27, D28);</li>
     *   <li>the bios resumes with the event; a YIELD sets the filter.</li>
     * </ol>
     * {@code eventName == null} resumes the bios with no values and ignores {@code arguments}, as Cobalt does. A
     * {@link #close} while the arguments are converted (addon code can run there) gives {@code TIMEOUT}, as Cobalt
     * gives when {@code close()} interrupts it, unless the event is then dropped: a dropped event gives {@code OK}
     * even then, and the next call throws. Log calls are best effort: one that throws changes neither the result nor
     * the close.
     *
     * @throws IllegalStateException if the machine has been closed, or if it is running (which CC:T never does) and
     *                               the event is neither skipped by the filter nor dropped
     */
    @Override
    public MachineResult handleEvent(@Nullable String eventName, @Nullable Object @Nullable [] arguments) {
        byte[] event = null;
        if (eventName != null) {
            vm.checkOpen();   // closed: throws, also for an event that the filter would skip (F24)
            Wire.Filter f = filter;
            if (f != null && !f.passes(eventName)) return MachineResult.OK;   // no conversion, no native call (I9)
            try {
                event = Wire.event(eventName, arguments, value -> unsupported(eventName, value));
            } catch (Throwable t) {
                // Wire.Dropped, or addon code threw while the arguments were read (D19, D30): the filter stays.
                dropped(eventName, t);
                return MachineResult.OK;
            }
        }
        NativeMachine.Outcome outcome;
        try {
            outcome = vm.resume(event);
        } catch (IllegalStateException e) {
            // close() after checkOpen, while the arguments were converted: report what Cobalt reports when close()
            // interrupts it. Anything else (closed without an event, or already running) is CC:T's contract.
            if (eventName != null && vm.stateName().equals("closed")) return MachineResult.TIMEOUT;
            throw e;
        } catch (Throwable t) {
            // Out of memory, say, which can leave the machine open (after a YIELD). Close it first, so that neither
            // the log call nor the text below can stop that.
            try {
                close();
            } catch (Throwable ignored) {
                // CC:T closes the machine again when it shuts the computer down.
            }
            try {
                LOG.error("CCLuaJIT internal error", t);
            } catch (Throwable ignored) {
                // The result below matters more than the log line.
            }
            return MachineResult.error("CCLuaJIT internal error: " + describe(t));
        }
        // close() during the resume: report what Cobalt reports when close() interrupts it. The machine is closed:
        // freed, or after ST_BUSY left unfreed and counted as a leak (NativeMachine.resume).
        if (outcome.closedWhileRunning()) return MachineResult.TIMEOUT;
        // YIELD keeps the machine open: return before anything below allocates (the filter is a reference write).
        // Every other status has closed it, and freed it except ST_BUSY, after which NativeMachine.resume leaves the
        // handle unfreed as a leak.
        if (outcome.status() == NativeMachine.ST_YIELD) {
            filter = outcome.filter();
            return MachineResult.OK;
        }
        if (outcome.status() == NativeMachine.ST_RETURNED) return MachineResult.GENERIC_ERROR;
        Wire.Message message = outcome.message() != null ? outcome.message() : new Wire.Message(Wire.NO_MESSAGE, -1);
        if (outcome.status() == NativeMachine.ST_ERROR) {
            // The screen shows only the first 51 columns, so the log keeps the whole traceback (F21, D7).
            try {
                if (LOG.isWarnEnabled()) LOG.warn("Top level coroutine errored: {}", forLog(message));
            } catch (Throwable ignored) {
                // Out of memory while escaping a 64 KiB message, say: the Lua error matters more than the log line.
            }
            return MachineResult.error(message.text());
        }
        // ST_INTERNAL, ST_BUSY: a bug or a broken lock, never a Lua program.
        try {
            LOG.error("CCLuaJIT internal error (status {}): {}", outcome.status(), forLog(message));
        } catch (Throwable ignored) {
            // As above.
        }
        return MachineResult.error("CCLuaJIT internal error: " + message.text());
    }

    /** Pure Java: never touches native code, so it is safe from any thread at any time. */
    @Override
    public void printExecutionState(StringBuilder out) {
        out.append("CCLuaJIT machine: ").append(vm.stateName()).append('\n');
    }

    /**
     * Any thread, any number of times. During a resume, the resume frees the machine when it returns (after
     * {@code ST_BUSY} it leaves it unfreed, see {@link NativeMachine#resume}).
     */
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
            try {
                LOG.error("Error calling {} on {}", e.name(), e.target(), t);
            } catch (Throwable ignored) {
                // Never throws, as NativeMachine.Host requires: the Lua error below matters more than the log line.
            }
            return Wire.error("Java Exception Thrown: " + describe(t), 0);
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
            try {
                LOG.warn("Method {} returned a value of unknown type {}, which becomes nil (logged once per type)",
                    e.name(), c.getName());
            } catch (Throwable ignored) {
                // The value still becomes nil.
            }
        }
    }

    /**
     * An event value that {@link Wire#event} cannot send yet: it becomes nil, with one WARN per class (D27, D28). A
     * table or a method object comes in S3; any other type stays nil, as in Cobalt. It throws only when CC:T's method
     * lookup does, for a class not logged yet: the event is then dropped (D30).
     */
    private void unsupported(String eventName, Object value) {
        Class<?> c = value.getClass();
        // Concurrent machines may pass the size check together, so the set can end a few classes over the limit.
        if (WARNED_EVENTS.size() >= MAX_WARNED || WARNED_EVENTS.contains(c)) return;
        boolean s3 = value instanceof Map<?, ?> || value instanceof Collection<?> || value instanceof Object[]
            || value instanceof ILuaFunction || value instanceof IDynamicLuaObject
            || env.luaMethods().forEachMethod(value, (target, name, method, info) -> { });
        if (!WARNED_EVENTS.add(c)) return;   // another machine logged it meanwhile
        try {
            LOG.warn(s3 ? "Event {} carries a value of type {}, which becomes nil until S3 (logged once per type)"
                    : "Event {} carries a value of unknown type {}, which becomes nil (logged once per type)",
                escape(eventName, LOG_NAME_MAX), c.getName());
        } catch (Throwable ignored) {
            // The value still becomes nil.
        }
    }

    /** Logs an event that is dropped before the resume (D19, D30). Never throws. */
    private static void dropped(String eventName, Throwable t) {
        try {
            String reason = t instanceof Wire.Dropped ? t.getMessage() : "reading its arguments threw " + describe(t);
            LOG.warn("Event {} was dropped: {}", escape(eventName, LOG_NAME_MAX), escape(reason, LOG_MESSAGE_MAX));
        } catch (Throwable ignored) {
            // The event is dropped either way.
        }
    }

    /**
     * {@code String.valueOf(t)}, or the class name of {@code t} if its {@code toString} throws (addon code may
     * override it). Package-private for the tests.
     */
    static String describe(Throwable t) {
        try {
            return String.valueOf(t);
        } catch (Throwable x) {
            return t.getClass().getName();
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
