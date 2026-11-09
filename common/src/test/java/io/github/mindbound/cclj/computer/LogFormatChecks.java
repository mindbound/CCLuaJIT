package io.github.mindbound.cclj.computer;

import io.github.mindbound.cclj.natives.Wire;

import java.util.ArrayList;
import java.util.List;

/**
 * Checks of the log text of {@link LuaJITMachine} ({@code forLog}, {@code escape}) and of {@code describe}, for
 * scenario 26 of {@code MachineSpecs}. Test code in LuaJITMachine's package, so that those methods stay
 * package-private. They cover the cut paths that no MachineSpecs run reaches: with printable names, a traceback of
 * at most 23 frames is about 4.3 KiB after escaping, so in the tests only this class reaches the 8 KiB cut of the
 * traceback (a Lua program whose function names carry many escaped bytes can reach it, M1.md section 3.10).
 */
public final class LogFormatChecks {
    private LogFormatChecks() {
    }

    /** The text that {@code handleEvent} logs after {@code "Top level coroutine errored: "} for this message. */
    public static String forLog(String text, int tracebackAt) {
        return LuaJITMachine.forLog(new Wire.Message(text, tracebackAt));
    }

    /** Null if every check passes, else the failed checks. */
    public static String run() {
        List<String> bad = new ArrayList<>();
        expect(bad, "escape: backslash, LF, tab, CR, C0, DEL, C1, Latin-1",
            LuaJITMachine.escape("\\\n\t\r\u0001\u007f\u0085\u00e9", 100), "\\\\\\n\\t\\r\\x01\\x7f\\x85\u00e9");
        String a1024 = "a".repeat(1024);
        expect(bad, "escape: max characters are not cut", LuaJITMachine.escape(a1024, 1024), a1024);
        expect(bad, "escape: max + 1 characters are cut to max - 3 and '...'",
            LuaJITMachine.escape("a".repeat(1025), 1024), "a".repeat(1021) + "...");
        expect(bad, "escape: the cut never splits an escape",
            LuaJITMachine.escape("\u0001".repeat(300), 1024), "\\x01".repeat(255) + "...");

        String value = "v".repeat(2000), tb = "\nstack traceback:" + "\n\tx".repeat(5000);
        String tbLog = LuaJITMachine.escape(tb, LuaJITMachine.LOG_TRACEBACK_MAX);
        if (tbLog.length() > LuaJITMachine.LOG_TRACEBACK_MAX || !tbLog.endsWith("...")
            || !tbLog.startsWith("\\nstack traceback:\\n\\tx")) {
            bad.add("escape: a traceback over 8 KiB is cut to at most 8192 characters, ending in '...' (got "
                + tbLog.length() + ")");
        }
        expect(bad, "forLog: the value and the traceback are cut each to its own limit",
            forLog(value + tb, value.length()), "v".repeat(LuaJITMachine.LOG_MESSAGE_MAX - 3) + "..." + tbLog);
        expect(bad, "forLog: no traceback", forLog("x\ny", -1), "x\\ny");

        expect(bad, "describe: toString", LuaJITMachine.describe(new RuntimeException("x")),
            "java.lang.RuntimeException: x");
        expect(bad, "describe: a throwing toString gives the class name", LuaJITMachine.describe(new Nasty()),
            Nasty.class.getName());
        return bad.isEmpty() ? null : String.join("; ", bad);
    }

    private static void expect(List<String> bad, String what, String got, String want) {
        if (!got.equals(want)) {
            bad.add(what + ": got " + got.length() + " chars '" + head(got) + "', expected " + want.length()
                + " chars '" + head(want) + "'");
        }
    }

    private static String head(String s) {
        return s.length() <= 40 ? s : s.substring(0, 40) + "[...]";
    }

    /** An exception whose {@code toString} throws. */
    private static final class Nasty extends RuntimeException {
        Nasty() {
            super("nasty", null, false, false);
        }

        @Override
        public String toString() {
            throw new IllegalStateException("LogFormatChecks: Nasty.toString");
        }
    }
}
