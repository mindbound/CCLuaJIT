package io.github.mindbound.cclj.natives;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * One native machine: a capped LuaJIT state that runs the bios in a coroutine (natives/src/machine.c), reached
 * through the native methods in natives/src/jni_machine.c (docs/port-1.21.1/M1.md sections 3.9 and 3.10).
 * JDK-only, like the whole {@code natives} package. The library must have been loaded by
 * {@link NativeLibrary#ensureLoaded} first.
 * <p>
 * Lifecycle. The monitor {@code lock} guards {@code handle}, {@code running} and {@code closed} (the constructor
 * writes {@code handle} under it too), and is never held across a native call, an upcall or a call into CC:T:
 * <ul>
 *   <li>{@link #resume} sets {@code running} under the lock, then calls the native code without it. One resume at
 *       a time: a second one, also from the host during an upcall, throws {@link IllegalStateException}.</li>
 *   <li>{@link #close} may be called from any thread, any number of times. While a resume runs, it only marks the
 *       machine closed; that resume frees it when it returns (except after {@link #ST_BUSY}, below).</li>
 *   <li>A resume that ends the machine (any status but {@link #ST_YIELD}) closes and frees it; after
 *       {@link #ST_BUSY} it closes it and leaves the handle unfreed (see below).</li>
 *   <li>A handle is freed at most once, by the thread that set it to 0 under the lock, outside the lock (I6).
 *       The native busy flag is a second line of defence against a broken lock: a native call that finds the
 *       machine busy returns {@link #ST_BUSY} and changes nothing, and a free that finds it busy frees nothing and
 *       is logged as a leak. A resume that gets {@link #ST_BUSY} does not free the handle, because another call
 *       owns it: it counts and logs it as a leak. The flag cannot protect a machine that is freed while another
 *       thread uses it: that is the lock's job alone.</li>
 * </ul>
 */
public final class NativeMachine {
    /** Serves the host calls of a machine: the API methods that the spec describes. */
    @FunctionalInterface
    public interface Host {
        /**
         * Runs host function {@code function} (its index in the spec) with the arguments in {@code request} (a wire
         * request, {@link Wire#decodeArgs}) and returns the reply ({@link Wire#values} or {@link Wire#error}).
         * Called on the thread that runs {@link #resume}, while the Lua code waits.
         * <p>
         * Must not throw: an exception becomes the Lua error {@code CCLuaJIT: Java exception in host call}, and the
         * native side prints its stack trace to stderr. A null reply becomes {@code CCLuaJIT: host call returned
         * null}. Must not keep {@code request}.
         */
        byte[] call(int function, byte[] request);
    }

    /**
     * The result of {@link #resume}.
     *
     * @param status             one of the {@code ST_} codes
     * @param message            the error message for {@link #ST_ERROR}, {@link #ST_INTERNAL} and {@link #ST_BUSY}
     *                           (also for {@link #ST_RETURNED}, where it is empty); null for {@link #ST_YIELD}
     * @param closedWhileRunning {@link #close} was called while this resume ran; the machine is closed and freed
     *                           (not freed after {@link #ST_BUSY})
     */
    public record Outcome(int status, Wire.Message message, boolean closedWhileRunning) {
    }

    /** A native call's status and message, as {@link #ended} makes them. */
    record Ended(int status, Wire.Message message) {
    }

    /** The native ABI version this class needs ({@code CCLJ_ABI} in cclj_machine.h). */
    public static final int ABI = 2;

    /**
     * Status codes of the native calls (cclj_machine.h). {@link #ST_BUSY}: another call owned the native machine,
     * which only a broken lock allows; nothing was done, the native message was not read (it is the owner's), and
     * {@link #resume} does not free the handle either.
     */
    public static final int ST_OK = 0, ST_YIELD = 1, ST_RETURNED = 2, ST_ERROR = 3, ST_INTERNAL = 4, ST_BUSY = 5;

    /** The message of an {@link #ST_BUSY} outcome. */
    static final String BUSY_TEXT = "machine is busy";

    private static final System.Logger LOG = System.getLogger(NativeMachine.class.getName());
    // Machines that create counted and that were not given to nFree yet. A free that the native side refuses, and a
    // handle that resume leaves unfreed after ST_BUSY, count as given: they are counted in LEAKED instead.
    private static final AtomicInteger LIVE = new AtomicInteger();
    private static final AtomicInteger LEAKED = new AtomicInteger();

    private final Host host;
    private final Object lock = new Object();
    private long handle;                // guarded by lock; 0 once taken for the free
    private boolean running, closed;    // guarded by lock

    private NativeMachine(Host host, long handle) {
        this.host = host;
        synchronized (lock) {
            this.handle = handle;   // under the lock like every other access, so no reader relies on safe publication
        }
    }

    /**
     * Creates a machine: a LuaJIT state capped at {@code memLimit} bytes, with the API tables of {@code spec}
     * ({@link Wire.SpecWriter}) and {@code bios} compiled (as {@code @bios.lua}, text only) into the bios coroutine,
     * which does not run yet. Every failure frees the native state.
     *
     * @param idKey keys the {@code "<type>: %08x"} rendering of error values (use a random value per machine)
     * @throws NativeMachineException out of memory (also for {@code memLimit <= 0}), or a bios or API description
     *                                that did not load; the message says why
     */
    public static NativeMachine create(Host host, long memLimit, long idKey, byte[] spec, byte[] bios)
        throws NativeMachineException {
        Objects.requireNonNull(host, "host");
        Objects.requireNonNull(spec, "spec");
        Objects.requireNonNull(bios, "bios");
        // LIVE counts the machine before nNew, so that everything after nNew runs inside the try: the finally frees
        // the handle if one was made, which takes it off LIVE, and takes it off LIVE itself if none was made.
        LIVE.incrementAndGet();
        long h = 0;
        boolean ok = false;
        try {
            h = nNew(memLimit, idKey);
            if (h == 0) throw new NativeMachineException("not enough memory");
            int st = nLoad(h, spec, bios);
            if (st != ST_OK) {
                String text = st == ST_BUSY ? BUSY_TEXT : ended(st, nMessage(h)).message().text();
                // A failed copy of the arrays (out of memory, or more than 16 MiB) leaves the message empty.
                throw new NativeMachineException(text.isEmpty() ? "CCLuaJIT: the machine could not be loaded "
                    + "(status " + st + ")" : text);
            }
            NativeMachine vm = new NativeMachine(host, h);
            ok = true;
            return vm;
        } finally {
            if (!ok) {
                if (h != 0) free(h);
                else LIVE.decrementAndGet();
            }
        }
    }

    /**
     * Resumes the bios coroutine on this thread. Its host calls run {@link Host#call} on this thread too. With
     * any status but {@link #ST_YIELD}, the machine is closed and freed before this returns, and so it is when
     * {@link #close} was called meanwhile ({@link Outcome#closedWhileRunning()}). After {@link #ST_BUSY} it is
     * closed but not freed: another call owns the native machine, so freeing it here could free it under that
     * call. The handle is counted and logged as a leak instead.
     *
     * @param event S1: must be null. Anything else gives {@link #ST_INTERNAL} with the message
     *              {@code events are not implemented yet (S2)}, or with {@code CCLuaJIT: the machine could not be
     *              resumed (status 4)} if the array cannot be copied (out of memory, or more than 16 MiB)
     * @throws IllegalStateException if the machine is closed or already running, and for nothing else
     */
    public Outcome resume(byte[] event) {
        long h;
        synchronized (lock) {
            if (closed) throw new IllegalStateException("Machine has been closed");
            if (running) throw new IllegalStateException("Machine is already running");
            running = true;
            h = handle;
        }
        int status = ST_INTERNAL;
        Wire.Message msg = null;
        long toFree = 0, toAbandon = 0;
        boolean closedDuring;
        try {
            status = nResume(h, event);
            if (status == ST_BUSY) {
                msg = new Wire.Message(BUSY_TEXT, -1);   // the native message is the owner's: not read
            } else if (status != ST_YIELD) {
                Ended e = ended(status, nMessage(h));   // read while running is still true
                status = e.status();
                msg = e.message();
                // nResume's own failures (an event that cannot be copied, upcall not found) leave the message empty,
                // as nLoad's do in create.
                if (status == ST_INTERNAL && msg.text().isEmpty()) {
                    msg = new Wire.Message("CCLuaJIT: the machine could not be resumed (status " + status + ")", -1);
                }
            }
        } finally {
            synchronized (lock) {
                running = false;
                closedDuring = closed;
                if (closed || status != ST_YIELD) {
                    closed = true;
                    if (status == ST_BUSY) toAbandon = handle;
                    else toFree = handle;
                    handle = 0;
                }
            }
            if (toFree != 0) free(toFree);   // outside the lock
            else if (toAbandon != 0) abandon();
        }
        return new Outcome(status, msg, closedDuring);
    }

    /**
     * Closes the machine: frees it now if it is idle, or marks it closed so that the running {@link #resume} frees
     * it when it returns (or, after {@link #ST_BUSY}, leaves it unfreed as a leak; S6a will also interrupt the Lua
     * code). Any thread, any number of times.
     */
    public void close() {
        long toFree = 0;
        synchronized (lock) {
            if (closed) return;
            closed = true;
            if (!running) {
                toFree = handle;
                handle = 0;
            }
        }
        if (toFree != 0) free(toFree);
    }

    /** @throws IllegalStateException if the machine has been closed */
    public void checkOpen() {
        synchronized (lock) {
            if (closed) throw new IllegalStateException("Machine has been closed");
        }
    }

    /** {@code idle}, {@code running} or {@code closed}, for diagnostics. Never calls native code. */
    public String stateName() {
        synchronized (lock) {
            return closed ? "closed" : running ? "running" : "idle";
        }
    }

    /**
     * Machines that {@link #create} counted and that were not given to the native free yet, in this JVM (for tests).
     * A free that the native side refused, and a handle that {@link #resume} left unfreed after {@link #ST_BUSY},
     * count as given; {@link #leaked} counts them. So {@code live() == 0 && leaked() == 0} means that every machine
     * was freed.
     */
    public static int live() {
        return LIVE.get();
    }

    /**
     * Frees that the native side refused ({@link #ST_BUSY}, or {@link #ST_INTERNAL} for a wrong magic), and handles
     * that {@link #resume} left unfreed after {@link #ST_BUSY}; each is logged as a leak (for tests).
     */
    public static int leaked() {
        return LEAKED.get();
    }

    /**
     * The status and message of a native call that ended with {@code status} and left {@code raw} (an nMessage
     * array) as its message. A malformed array, which jni_machine.c never makes, gives {@link #ST_INTERNAL} with the
     * text {@code CCLuaJIT: malformed native message (<reason>)} and no traceback: so {@link #resume} throws
     * {@link IllegalStateException} only for "closed" and "already running" (M1.md section 3.10). Package-private
     * for BridgeProbe.
     */
    static Ended ended(int status, byte[] raw) {
        try {
            return new Ended(status, Wire.message(raw));
        } catch (IllegalStateException e) {
            return new Ended(ST_INTERNAL, new Wire.Message("CCLuaJIT: malformed native message (" + e.getMessage()
                + ")", -1));
        }
    }

    /** Called by jni_machine.c only, during {@link #nResume}, on the thread that runs it. */
    @SuppressWarnings("unused")
    private byte[] upcall(int fn, byte[] request) {
        return host.call(fn, request);
    }

    /** A handle that resume does not free after ST_BUSY: another call owns it (a broken lock). */
    private static void abandon() {
        LIVE.decrementAndGet();
        LEAKED.incrementAndGet();
        try {
            LOG.log(System.Logger.Level.ERROR, "CCLuaJIT: a native machine was busy when it was resumed (a broken "
                + "lock); it is not freed, and its memory leaks");
        } catch (Throwable ignored) {
            // The counters above already record the leak.
        }
    }

    private static void free(long h) {
        int st = nFree(h);
        LIVE.decrementAndGet();
        if (st != ST_OK) {
            LEAKED.incrementAndGet();
            try {
                LOG.log(System.Logger.Level.ERROR, "CCLuaJIT: a native machine could not be freed (status " + st
                    + (st == ST_BUSY ? ": busy" : "") + "); its memory leaks");
            } catch (Throwable ignored) {
                // Out of memory while logging, say: the counters above already record the leak.
            }
        }
    }

    // natives/src/jni_machine.c. h is the native pointer; Java passes it only while it owns the machine.

    static native int nAbi();

    private static native long nNew(long memLimit, long idKey);

    private static native int nLoad(long h, byte[] spec, byte[] bios);

    private native int nResume(long h, byte[] event);

    private static native byte[] nMessage(long h);

    private static native int nFree(long h);
}
