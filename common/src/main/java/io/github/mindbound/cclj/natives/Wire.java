package io.github.mindbound.cclj.natives;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Consumer;

/**
 * The Java side of wire format v1 (docs/port-1.21.1/M1.md section 3.6, and natives/src/cclj_machine.h), which
 * carries values between Java and the native machine as one {@code byte[]} per call. JDK-only, like the whole
 * {@code natives} package.
 * <pre>
 *   value   := 00 NIL | 01 FALSE | 02 TRUE | 03 NUMBER f64 | 04 STRING u32 len, bytes   (tags 05..0F: S3)
 *   request := u32 n, value*n                                    C -&gt; Java, per host call: {@link #decodeArgs}
 *   reply   := 00 u32 n, value*n                                 VALUES: {@link #values}
 *            | 01 i32 level, value (NIL or STRING)               ERROR: {@link #error}   (kind 02: S5)
 *   spec    := u32 napi, api*                                    {@link SpecWriter}
 *   api     := u32 nnames, str*nnames, u8 hasModule, [str module], u32 nmethods, method*
 *   method  := str name, u8 flags                                bit 0 = MAY_YIELD
 *   str     := u32 len, bytes
 *   event   := u32 n, value*n                                    Java -&gt; C, nResume: {@link #event}
 *                                                                n &gt;= 1, the first value is a STRING (the name)
 *   filter  := (no bytes) | 04 u32 len, bytes | 01               the message after a YIELD: {@link #filter}
 *                                                                no filter | STRING | FALSE (only "terminate")
 * </pre>
 * Integers are little-endian, numbers IEEE-754 binary64. Both encoders of numbers ({@link #values} and
 * {@link #event}) send -0.0 as +0.0, as Cobalt does (docs/port-1.21.1/S2.md, D29); every other number keeps its
 * bits, NaN payloads included. Strings are raw Lua bytes, read and written as Latin-1: byte b is char b, and a char
 * of 256 or more is written as {@code '?'} (a surrogate pair as two).
 * <p>
 * Every decoder checks each length against the bytes that remain, rejects unknown tags, requires that no byte is
 * left over, and throws {@link IllegalStateException} on any malformed input.
 */
public final class Wire {
    private Wire() {
    }

    /** Value tags. */
    static final int NIL = 0x00, FALSE = 0x01, TRUE = 0x02, NUMBER = 0x03, STRING = 0x04;
    /** Reply kinds. */
    static final int VALUES = 0x00, ERROR = 0x01;
    /** Method flag: the method may yield ({@code info == null || !info.nonYielding()}). */
    static final int MAY_YIELD = 0x01;

    /** Limits of a spec, as the native checker applies them (cclj_machine.h). */
    static final int MAX_FUNCTIONS = 65536, MAX_NAME = 256;

    /** The text {@link #message} gives for a null array (the native side could not make one). */
    public static final String NO_MESSAGE = "(no message)";

    /**
     * The arguments that can go with an event name: {@code CM_EVENT_MAX - 1} (cclj_machine.h), so that the name and
     * the arguments fit the 8,000 slots of a Lua C call after a YIELD (docs/port-1.21.1/S2.md, F23).
     */
    public static final int MAX_EVENT_ARGS = 7998;
    /** The largest record the native side copies ({@code CM_WIRE_MAX}): an {@link #event} is at most this long. */
    public static final int MAX_BYTES = 16 << 20;

    private static final VarHandle INT = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle LONG = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);

    /**
     * A value that a method result may not hold yet: a {@link Map}, {@link Collection}, {@code Object[]} or
     * {@link ByteBuffer} (tables and byte buffers come in S3), or whatever the {@code onUnknown} callback of
     * {@link #values} refuses. Its message is the Lua error text. It has no stack trace.
     */
    public static final class UnsupportedValue extends RuntimeException {
        public UnsupportedValue(String message) {
            super(message, null, false, false);
        }
    }

    /**
     * The message of the last native call, split as {@code nMessage} returns it.
     *
     * @param text        the message, Latin-1 decoded
     * @param tracebackAt the offset in {@code text} where {@code "\nstack traceback:"} starts, or -1 if the message
     *                    has no traceback. Search for the header text would be wrong: a Lua program writes the
     *                    part before it.
     */
    public record Message(String text, int tracebackAt) {
    }

    /**
     * An event that {@link #event} drops before it reaches the native side (docs/port-1.21.1/S2.md, D19 and D30):
     * too many arguments, or a record over {@value #MAX_BYTES} bytes. Its message is the reason, for the log. It
     * has no stack trace.
     */
    public static final class Dropped extends RuntimeException {
        public Dropped(String message) {
            super(message, null, false, false);
        }
    }

    /**
     * The event filter of a YIELD: the first value the bios yielded, as a string. A filter longer than the native
     * side can send ({@code CM_FILTER_MAX}) comes as {@code onlyTerminate} (docs/port-1.21.1/S2.md, D26).
     *
     * @param text          the event name that passes; empty when {@code onlyTerminate}
     * @param onlyTerminate only {@code "terminate"} passes
     */
    public record Filter(String text, boolean onlyTerminate) {
        public Filter {
            Objects.requireNonNull(text, "text");
        }

        /** Whether an event of this name reaches the bios: {@code "terminate"} always does, as in Cobalt. */
        public boolean passes(String name) {
            return name.equals("terminate") || (!onlyTerminate && name.equals(text));
        }
    }

    // ---- Decoders ------------------------------------------------------------------------------------------------

    /**
     * Decodes a request ({@code u32 n, value*n}) into its arguments: {@code null}, {@link Boolean}, {@link Double}
     * or {@link String}.
     *
     * @throws IllegalStateException if the array is null or malformed
     */
    public static Object[] decodeArgs(byte[] request) {
        if (request == null) throw new IllegalStateException("wire: no request");
        Reader r = new Reader(request);
        long n = r.u32();
        if (n > r.left()) throw new IllegalStateException("wire: " + n + " values in " + r.left() + " bytes");
        Object[] out = new Object[(int) n];
        for (int i = 0; i < out.length; i++) out[i] = r.value();
        r.end();
        return out;
    }

    /**
     * Splits an {@code nMessage} array (a little-endian u32 traceback offset, 0xFFFFFFFF for none, then the
     * message bytes) into the Latin-1 text and the offset. A null array gives {@value #NO_MESSAGE} without a
     * traceback.
     *
     * @throws IllegalStateException if the array is shorter than 4 bytes or the offset is past the text
     */
    public static Message message(byte[] raw) {
        if (raw == null) return new Message(NO_MESSAGE, -1);
        if (raw.length < 4) throw new IllegalStateException("wire: message of " + raw.length + " bytes");
        int off = (int) INT.get(raw, 0);
        String text = new String(raw, 4, raw.length - 4, StandardCharsets.ISO_8859_1);
        if (off != -1 && (off < 0 || off > text.length())) {
            throw new IllegalStateException("wire: traceback offset " + Integer.toUnsignedString(off)
                + " in a message of " + text.length() + " bytes");
        }
        return new Message(text, off);
    }

    /**
     * Decodes the {@code nMessage} array of a YIELD into its event filter: the offset 0xFFFFFFFF (no traceback),
     * then the filter record. No record bytes give null (no filter), a STRING gives the filter text (Latin-1), and
     * FALSE gives the filter that only {@code "terminate"} passes.
     *
     * @throws IllegalStateException for a null array (the native side could not make one), a short array, a
     *                               traceback offset, any other value, or bytes left over
     */
    public static Filter filter(byte[] raw) {
        if (raw == null) throw new IllegalStateException("wire: no filter message");
        if (raw.length < 4) throw new IllegalStateException("wire: filter message of " + raw.length + " bytes");
        int off = (int) INT.get(raw, 0);
        if (off != -1) {
            throw new IllegalStateException("wire: filter message with traceback offset "
                + Integer.toUnsignedString(off));
        }
        if (raw.length == 4) return null;
        Reader r = new Reader(raw, 4);
        Object v = r.value();
        r.end();
        if (v instanceof String s) return new Filter(s, false);
        if (Boolean.FALSE.equals(v)) return new Filter("", true);
        throw new IllegalStateException("wire: filter of " + (v == null ? "nil" : v.getClass().getSimpleName()));
    }

    // ---- Encoders ------------------------------------------------------------------------------------------------

    /**
     * Encodes a VALUES reply: {@code 00 u32 n, value*n}. A null array gives no values (CC:T's
     * {@code MethodResult.of().getResult()} is null).
     * <ul>
     *   <li>{@code null}, {@link Boolean} and {@link Number} ({@code doubleValue()}, -0.0 sent as +0.0: D29) map
     *       directly;</li>
     *   <li>a {@link String} is written char by char, {@code c < 256 ? c : '?'};</li>
     *   <li>a {@code byte[]} is copied as it is;</li>
     *   <li>a {@link Map}, {@link Collection}, {@code Object[]} or {@link ByteBuffer} throws
     *       {@link UnsupportedValue} (until S3);</li>
     *   <li>anything else is passed to {@code onUnknown}, which may throw (S1: for a method object); if it returns,
     *       the value becomes NIL, as in Cobalt (D9).</li>
     * </ul>
     *
     * @throws UnsupportedValue for a value that cannot be sent yet
     */
    public static byte[] values(Object[] values, Consumer<Object> onUnknown) {
        Objects.requireNonNull(onUnknown, "onUnknown");
        int n = values == null ? 0 : values.length;
        Writer w = new Writer(16 + 9 * n);
        w.u8(VALUES);
        w.u32(n);
        for (int i = 0; i < n; i++) {
            Object v = values[i];
            if (v == null) {
                w.u8(NIL);
            } else if (v instanceof Boolean b) {
                w.u8(b ? TRUE : FALSE);
            } else if (v instanceof Number num) {
                w.number(num.doubleValue());
            } else if (v instanceof String s) {
                w.u8(STRING);
                w.latin1(s);
            } else if (v instanceof byte[] bytes) {
                w.string(bytes);
            } else if (v instanceof Map<?, ?> || v instanceof Collection<?> || v instanceof Object[]) {
                throw new UnsupportedValue("CCLuaJIT: tables are not supported yet (S3)");
            } else if (v instanceof ByteBuffer) {
                throw new UnsupportedValue("CCLuaJIT: ByteBuffer values are not supported yet (S3)");
            } else {
                onUnknown.accept(v);
                w.u8(NIL);
            }
        }
        return w.toByteArray();
    }

    /**
     * Encodes an event record ({@code u32 n, value*n}): the name, then the arguments (docs/port-1.21.1/S2.md
     * section 4.5). A null array gives no arguments. In this order:
     * <ol>
     *   <li>More than {@value #MAX_EVENT_ARGS} arguments throw {@link Dropped} before any argument is read: no
     *       buffer moves, and {@code onUnsupported} is not called.</li>
     *   <li>The name is a STRING (Latin-1, as in {@link #values}).</li>
     *   <li>Each argument: {@code null} is NIL (trailing nulls count); {@link Boolean} and {@link Number}
     *       ({@code doubleValue()}, -0.0 sent as +0.0: D29) map directly; a {@link String} is Latin-1; a
     *       {@code byte[]} is copied as it is; a {@link ByteBuffer} gives its {@code remaining()} bytes, read with
     *       {@code get}, so its position advances, as in Cobalt; anything else ({@link Map}, {@link Collection},
     *       {@code Object[]}, any other type) is passed to {@code onUnsupported}, which may throw, and becomes NIL
     *       (until S3).</li>
     *   <li>A record that would grow past {@value #MAX_BYTES} bytes throws {@link Dropped} before the value that
     *       does not fit is copied.</li>
     * </ol>
     *
     * @throws Dropped for too many arguments or a record over {@value #MAX_BYTES} bytes; anything that
     *                 {@code onUnsupported} or a {@link Number} throws passes through
     */
    public static byte[] event(String name, Object[] args, Consumer<Object> onUnsupported) {
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(onUnsupported, "onUnsupported");
        int n = args == null ? 0 : args.length;
        if (n > MAX_EVENT_ARGS) {
            throw new Dropped("it has " + n + " arguments, and at most " + MAX_EVENT_ARGS + " fit");
        }
        Writer w = new Writer(64 + 9 * n, MAX_BYTES);
        w.u32(1 + n);
        w.u8(STRING);
        w.latin1(name);
        for (int i = 0; i < n; i++) {
            Object v = args[i];
            if (v == null) {
                w.u8(NIL);
            } else if (v instanceof Boolean b) {
                w.u8(b ? TRUE : FALSE);
            } else if (v instanceof Number num) {
                w.number(num.doubleValue());
            } else if (v instanceof String s) {
                w.u8(STRING);
                w.latin1(s);
            } else if (v instanceof byte[] bytes) {
                w.string(bytes);
            } else if (v instanceof ByteBuffer bb) {
                w.string(bb);
            } else {
                onUnsupported.accept(v);
                w.u8(NIL);
            }
        }
        return w.toByteArray();
    }

    /**
     * Encodes an ERROR reply: {@code 01 i32 level, value}. The message is a STRING (Latin-1, as in
     * {@link #values}), or NIL if it is null. The native side adds the position of {@code level} when the message
     * is a string and {@code level > 0}.
     */
    public static byte[] error(String message, int level) {
        Writer w = new Writer(16 + (message == null ? 0 : message.length()));
        w.u8(ERROR);
        w.u32(level);
        if (message == null) {
            w.u8(NIL);
        } else {
            w.u8(STRING);
            w.latin1(message);
        }
        return w.toByteArray();
    }

    /**
     * Writes a spec, the description of the API tables that {@code nLoad} builds. Function indices number the
     * methods in the order they are added, across all APIs, from 0. Names are Latin-1 (as in {@link #values}) and
     * at most {@value #MAX_NAME} bytes; at most {@value #MAX_FUNCTIONS} methods in all. Not thread-safe.
     */
    public static final class SpecWriter {
        private final List<Writer> apis = new ArrayList<>();
        private Writer current;   // the API that methods go to; null before the first api()
        private int countAt;      // offset of its method count in current
        private int count, total;

        /**
         * Starts an API: its global names (any number, also none) and its module name ({@code null} for none).
         * The methods added next belong to it.
         *
         * @throws IllegalArgumentException for a name longer than {@value #MAX_NAME} bytes
         */
        public SpecWriter api(Collection<String> names, String module) {
            Writer w = new Writer(64);
            w.u32(names.size());
            for (String name : names) w.name(name);
            if (module == null) {
                w.u8(0);
            } else {
                w.u8(1);
                w.name(module);
            }
            countAt = w.size();
            w.u32(0);   // the method count, rewritten by method()
            apis.add(w);
            current = w;
            count = 0;
            return this;
        }

        /**
         * Adds a method to the current API. Its function index is the number of methods added before it, in all
         * APIs.
         *
         * @throws IllegalStateException    if no API has been started
         * @throws IllegalArgumentException for a name longer than {@value #MAX_NAME} bytes, or more than
         *                                  {@value #MAX_FUNCTIONS} methods in all
         */
        public SpecWriter method(String name, boolean mayYield) {
            if (current == null) throw new IllegalStateException("wire: method " + name + " outside an API");
            if (total == MAX_FUNCTIONS) throw new IllegalArgumentException("more than " + MAX_FUNCTIONS + " methods");
            current.name(name);
            current.u8(mayYield ? MAY_YIELD : 0);
            count++;
            total++;
            INT.set(current.buf, countAt, count);
            return this;
        }

        /** The number of methods added so far, which is the function index of the next one. */
        public int functions() {
            return total;
        }

        /** The spec: {@code u32 napi, api*}. */
        public byte[] toByteArray() {
            int size = 4;
            for (Writer a : apis) size += a.size();
            Writer w = new Writer(size);
            w.u32(apis.size());
            for (Writer a : apis) w.bytes(a.buf, 0, a.size());
            return w.toByteArray();
        }
    }

    // ---- Codec helpers -------------------------------------------------------------------------------------------

    /** A bounded little-endian reader. Every read checks the bytes left first. */
    private static final class Reader {
        private final byte[] b;
        private int pos;

        Reader(byte[] b) {
            this(b, 0);
        }

        Reader(byte[] b, int pos) {
            this.b = b;
            this.pos = pos;
        }

        int left() {
            return b.length - pos;
        }

        private void need(long n) {
            if (n > left()) throw new IllegalStateException("wire: " + n + " bytes needed, " + left() + " left");
        }

        int u8() {
            need(1);
            return b[pos++] & 0xff;
        }

        long u32() {
            need(4);
            int v = (int) INT.get(b, pos);
            pos += 4;
            return Integer.toUnsignedLong(v);
        }

        double f64() {
            need(8);
            long v = (long) LONG.get(b, pos);
            pos += 8;
            return Double.longBitsToDouble(v);
        }

        Object value() {
            int tag = u8();
            switch (tag) {
                case NIL:
                    return null;
                case FALSE:
                    return Boolean.FALSE;
                case TRUE:
                    return Boolean.TRUE;
                case NUMBER:
                    return f64();
                case STRING: {
                    long n = u32();
                    need(n);
                    String s = new String(b, pos, (int) n, StandardCharsets.ISO_8859_1);
                    pos += (int) n;
                    return s;
                }
                default:
                    throw new IllegalStateException("wire: unknown value tag " + tag);
            }
        }

        void end() {
            if (left() != 0) throw new IllegalStateException("wire: " + left() + " bytes left over");
        }
    }

    /**
     * A growable little-endian writer, at most {@code max} bytes long. Past that it throws before it writes: an
     * {@link OutOfMemoryError} when {@code max} is the largest Java array, else {@link Dropped} (an event record).
     */
    private static final class Writer {
        private static final int ARRAY_MAX = Integer.MAX_VALUE - 8;

        byte[] buf;
        private int len;
        private final int max;

        Writer(int capacity) {
            this(capacity, ARRAY_MAX);
        }

        Writer(int capacity, int max) {
            this.max = max;
            buf = new byte[Math.min(Math.max(capacity, 16), max)];
        }

        int size() {
            return len;
        }

        /** Makes room for n more bytes; the capacity doubles, up to max. */
        private void room(int n) {
            if ((long) len + n > max) {
                if (max == ARRAY_MAX) throw new OutOfMemoryError("wire: value too large");
                throw new Dropped("its record is over " + (max >> 20) + " MiB");
            }
            if (n > buf.length - len) {
                buf = Arrays.copyOf(buf, (int) Math.min(Math.max((long) buf.length * 2, (long) len + n), max));
            }
        }

        void u8(int v) {
            room(1);
            buf[len++] = (byte) v;
        }

        void u32(int v) {
            room(4);
            INT.set(buf, len, v);
            len += 4;
        }

        void f64(double d) {
            room(8);
            LONG.set(buf, len, Double.doubleToRawLongBits(d));
            len += 8;
        }

        /** A NUMBER: -0.0 is written as +0.0, as Cobalt's {@code valueOf} does (D29); any other keeps its bits. */
        void number(double d) {
            u8(NUMBER);
            f64(d == 0.0 ? 0.0 : d);
        }

        void bytes(byte[] src, int off, int n) {
            room(n);
            System.arraycopy(src, off, buf, len, n);
            len += n;
        }

        /** A STRING of the bytes of b, copied. */
        void string(byte[] b) {
            u8(STRING);
            u32(b.length);
            bytes(b, 0, b.length);
        }

        /**
         * A STRING of the {@code remaining()} bytes of bb, read with {@code get}: the position advances, and a
         * read-only buffer works. Nothing moves when the bytes do not fit.
         */
        void string(ByteBuffer bb) {
            int n = bb.remaining();
            u8(STRING);
            u32(n);
            room(n);
            bb.get(buf, len, n);
            len += n;
        }

        /** {@code u32 len, bytes} of s, one byte per char: {@code c < 256 ? c : '?'}. */
        void latin1(String s) {
            int n = s.length();
            u32(n);
            room(n);
            for (int i = 0; i < n; i++) {
                char c = s.charAt(i);
                buf[len++] = (byte) (c < 256 ? c : '?');
            }
        }

        /** A spec name: Latin-1, at most 256 bytes ({@code MAX_NAME}). */
        void name(String s) {
            if (s.length() > MAX_NAME) {
                throw new IllegalArgumentException("name longer than " + MAX_NAME + " bytes: " + s.substring(0, 40)
                    + "...");
            }
            latin1(s);
        }

        byte[] toByteArray() {
            return len == buf.length ? buf : Arrays.copyOf(buf, len);
        }
    }
}
