package io.github.mindbound.cclj.headless;

import dan200.computercraft.api.filesystem.Mount;
import dan200.computercraft.api.filesystem.WritableMount;
import dan200.computercraft.api.lua.LuaException;
import dan200.computercraft.core.ComputerContext;
import dan200.computercraft.core.apis.IAPIEnvironment;
import dan200.computercraft.core.apis.TermAPI;
import dan200.computercraft.core.asm.LuaMethodSupplier;
import dan200.computercraft.core.computer.Computer;
import dan200.computercraft.core.computer.ComputerEnvironment;
import dan200.computercraft.core.computer.GlobalEnvironment;
import dan200.computercraft.core.computer.TimeoutState;
import dan200.computercraft.core.computer.computerthread.ManagedTimeoutState;
import dan200.computercraft.core.computer.mainthread.MainThread;
import dan200.computercraft.core.filesystem.MemoryMount;
import dan200.computercraft.core.lua.CobaltLuaMachine;
import dan200.computercraft.core.lua.ILuaMachine;
import dan200.computercraft.core.lua.MachineEnvironment;
import dan200.computercraft.core.lua.MachineException;
import dan200.computercraft.core.lua.MachineResult;
import dan200.computercraft.core.methods.LuaMethod;
import dan200.computercraft.core.methods.MethodSupplier;
import dan200.computercraft.core.metrics.MetricsObserver;
import dan200.computercraft.core.terminal.Terminal;
import io.github.mindbound.cclj.computer.LuaJITMachine;
import io.github.mindbound.cclj.natives.NativeLibrary;
import io.github.mindbound.cclj.natives.NativeMachine;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiPredicate;
import java.util.function.Function;
import java.util.regex.Pattern;

/**
 * Tier 2 (docs/port-1.21.1/M1.md section 3.15): the CC:T machine contract, for {@link LuaJITMachine}, mostly as a
 * differential against {@link CobaltLuaMachine}. A plain main program, like the other headless runners.
 * <p>
 * <b>Direct environment (Option A).</b> Each scenario builds a {@link MachineEnvironment} by hand: an
 * {@code ILuaContext} whose {@code issueMainThreadTask} throws, {@link MetricsObserver#discard()}, a
 * {@link ManagedTimeoutState} that never pauses, the APIs {@code term} ({@link TermAPI} on a 51x19 terminal) and
 * {@code t} ({@link SpecAPI}), and no generic methods. Before each {@code handleEvent} it calls
 * {@code startTimer(TIMEOUT)} and {@code setRemainingTime(TIMEOUT)}, as CC:T does (F10). The scenario runs on both
 * machines (LuaJIT only where marked). For LuaJIT it checks the expected result; then it compares the two runs:
 * the kind of each {@code handleEvent} result, the first line of its message, the {@code MachineException} text and
 * terminal line 0. An error result from an uncaught Lua error must have {@code stack traceback:} as its second
 * line on both machines (D7, F21). Every difference must be on the scenario's allow-list, with a reason.
 * <p>
 * <b>Through {@code Computer} (Option B).</b> A real {@link ComputerContext} with two computer threads, an empty
 * {@link MemoryMount} as ROM and as root, and a factory that records what the machine does: B1 an error bios, B2 a
 * bios that does not compile (and can be retried), B3 the context closes.
 * <p>
 * Each scenario has a watchdog (10 s; 75 s for Option B), which prints every thread's stack and ends the JVM with
 * {@code Runtime.halt(2)}: before S6a, a machine cannot be stopped. slf4j-simple writes the log to
 * {@code System.err}, which this class tees, so a scenario can count WARN lines.
 * <pre>
 *   MachineSpecs [--report &lt;file.tsv&gt;]      Gradle: ./gradlew :common:machineSpecs
 * </pre>
 * The natives are loaded from {@code -Dccluajit.nativeDir} (default {@code <java.io.tmpdir>/ccluajit-natives}).
 * Exit: 0 every check passed; 1 a check failed; 2 the watchdog fired; 3 the natives could not be loaded.
 */
public final class MachineSpecs {
    static final String HOST = "ComputerCraft " + HeadlessRunner.CCT_VERSION + " (MachineSpecs)";
    static final MethodSupplier<LuaMethod> METHODS = LuaMethodSupplier.create(List.of());
    static final String G1_BIOS = "term.write(\"hi\")\nlocal x = 1\nerror(\"boom\")\n";
    static final Pattern TABLE_NAME = Pattern.compile("^table: [0-9a-f]{8}$");

    private static final ScheduledExecutorService WATCHDOG = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "machineSpecs-watchdog");
        t.setDaemon(true);
        return t;
    });
    private static Tee tee;
    private static int passed, allowed, failed;
    private static final List<String> report = new ArrayList<>();

    private MachineSpecs() {
    }

    // ---- One machine's run of a scenario ----------------------------------------------------------------------------

    /**
     * One {@code handleEvent}: {@code OK}, {@code PAUSE}, {@code TIMEOUT}, {@code GENERIC_ERROR}, {@code error} (with
     * its message) or {@code throws}.
     */
    record Step(String kind, String message) {
        String line(int n) {
            if (message == null) return "";
            String[] lines = message.split("\n", -1);
            return n < lines.length ? lines[n] : "";
        }

        String show() {
            if (message == null) return kind;
            int lines = message.split("\n", -1).length;
            return kind + ": " + esc(line(0)) + (lines > 1 ? " (+" + (lines - 1) + " lines)" : "");
        }
    }

    /** What one machine did in one scenario. */
    static final class Run {
        final String machine;
        String exception;                        // MachineException text of the constructor
        final List<Step> steps = new ArrayList<>();
        String line0 = "";
        int cursorX;
        int marks;
        long eventNanos;                         // time of the last handleEvent
        String log = "";                         // System.err while the scenario ran
        String extra = "";                       // scenario-specific detail for the report
        String warnTag;                          // if set, the report counts the WARN lines that name it
        String fault;                            // set by a scenario body that saw its precondition fail

        Run(String machine) {
            this.machine = machine;
        }

        Step last() {
            return steps.isEmpty() ? new Step("none", null) : steps.get(steps.size() - 1);
        }

        String show() {
            StringBuilder b = new StringBuilder();
            if (exception != null) b.append("MachineException: ").append(esc(exception));
            for (Step s : steps) b.append(b.isEmpty() ? "" : "; ").append(s.show());
            b.append(" | line 0 '").append(esc(line0.stripTrailing())).append('\'');
            if (!extra.isEmpty()) b.append(" | ").append(extra);
            if (warnTag != null) b.append(" | ").append(warnings(log, warnTag)).append(" WARN line(s) naming ")
                .append(warnTag);
            return b.toString();
        }
    }

    /** A machine environment for one scenario (Option A). */
    static final class Env {
        final Terminal terminal = new Terminal(51, 19, false);
        final SpecAPI api = new SpecAPI();
        final ManagedTimeoutState timeout = new ManagedTimeoutState() {
            @Override
            protected boolean shouldPause() {
                return false;
            }
        };
        final MachineEnvironment env;

        Env() {
            IAPIEnvironment proxy = (IAPIEnvironment) Proxy.newProxyInstance(MachineSpecs.class.getClassLoader(),
                new Class<?>[]{ IAPIEnvironment.class }, (self, method, args) -> switch (method.getName()) {
                    case "getTerminal" -> terminal;
                    case "toString" -> "MachineSpecs.IAPIEnvironment";
                    case "hashCode" -> System.identityHashCode(self);
                    case "equals" -> self == args[0];
                    default -> throw new UnsupportedOperationException("MachineSpecs: IAPIEnvironment."
                        + method.getName());
                });
            env = new MachineEnvironment(task -> {
                throw new LuaException("MachineSpecs: no main thread");
            }, MetricsObserver.discard(), timeout, List.of(new TermAPI(proxy), api), METHODS, HOST);
        }

        ILuaMachine create(ILuaMachine.Factory factory, String bios) throws IOException, MachineException {
            ILuaMachine m = factory.create(env, new ByteArrayInputStream(bios.getBytes(StandardCharsets.ISO_8859_1)));
            api.machine = m;
            return m;
        }

        /** One {@code handleEvent(null, null)}, timed, after the timer calls CC:T makes before each work unit. */
        Step event(ILuaMachine m, Run r) {
            timeout.startTimer(TimeoutState.TIMEOUT);
            timeout.setRemainingTime(TimeoutState.TIMEOUT);
            long t0 = System.nanoTime();
            try {
                return step(m.handleEvent(null, null));
            } catch (RuntimeException e) {
                return new Step("throws", e.toString());
            } finally {
                r.eventNanos = System.nanoTime() - t0;
            }
        }

        void finish(Run r) {
            r.line0 = terminal.getLine(0).toString();
            r.cursorX = terminal.getCursorX();
            r.marks = api.marks.get();
        }
    }

    static Step step(MachineResult res) {
        if (res == null) return new Step("null", null);
        if (res == MachineResult.OK) return new Step("OK", null);
        if (res == MachineResult.PAUSE) return new Step("PAUSE", null);
        if (res == MachineResult.TIMEOUT) return new Step("TIMEOUT", null);
        if (res == MachineResult.GENERIC_ERROR) return new Step("GENERIC_ERROR", null);
        if (res.isError()) return new Step("error", res.getMessage());
        return new Step(res.isPause() ? "pause?" : "ok?", res.getMessage());
    }

    /** The usual run: create, up to {@code events} handleEvent calls (stopping after the first not OK), close. */
    static Run run(String machine, ILuaMachine.Factory factory, String bios, int events) throws IOException {
        Env e = new Env();
        Run r = new Run(machine);
        ILuaMachine m;
        try {
            m = e.create(factory, bios);
        } catch (MachineException x) {
            r.exception = x.getMessage();
            e.finish(r);
            return r;
        }
        try {
            for (int i = 0; i < events; i++) {
                Step s = e.event(m, r);
                r.steps.add(s);
                if (!s.kind().equals("OK")) break;
            }
        } finally {
            m.close();
        }
        e.finish(r);
        return r;
    }

    // ---- Scenarios --------------------------------------------------------------------------------------------------

    @FunctionalInterface
    interface Body {
        Run run(String machine, ILuaMachine.Factory factory) throws Exception;
    }

    /** A difference from Cobalt that is expected, with the reason; {@code accepts} sees the LuaJIT and Cobalt runs. */
    record Allow(String reason, BiPredicate<Run, Run> accepts) {
    }

    /**
     * @param expect checks the LuaJIT run: null if it is right, else what is wrong
     * @param allow  the allowed difference from Cobalt, or null; unused for a LuaJIT-only scenario
     */
    record Scenario(String id, String name, boolean luajitOnly, Body body, Function<Run, String> expect, Allow allow) {
    }

    static Body bios(String bios) {
        return (machine, factory) -> run(machine, factory, bios, 1);
    }

    /** The last step is an error whose first line is {@code expected}. */
    static Function<Run, String> errorLine(String expected) {
        return r -> {
            if (r.exception != null) return "MachineException: " + esc(r.exception);
            Step s = r.last();
            if (!s.kind().equals("error")) return "expected an error, got " + s.show();
            return s.line(0).equals(expected) ? null : "first line '" + esc(s.line(0)) + "', expected '"
                + esc(expected) + "'";
        };
    }

    static Function<Run, String> errorMatches(Pattern p) {
        return r -> {
            if (r.exception != null) return "MachineException: " + esc(r.exception);
            Step s = r.last();
            if (!s.kind().equals("error")) return "expected an error, got " + s.show();
            return p.matcher(s.line(0)).find() ? null : "first line '" + esc(s.line(0)) + "' does not match " + p;
        };
    }

    static Function<Run, String> both(Function<Run, String> a, Function<Run, String> b) {
        return r -> {
            String x = a.apply(r);
            return x != null ? x : b.apply(r);
        };
    }

    static Function<Run, String> line0Starts(String prefix) {
        return r -> r.line0.startsWith(prefix) ? null : "terminal line 0 '" + esc(r.line0.stripTrailing())
            + "', expected it to start with '" + esc(prefix) + "'";
    }

    /** Allows a difference only in the first line of the last step, when {@code ok(luajitLine, cobaltLine)}. */
    static Allow lastLine(String reason, BiPredicate<String, String> ok) {
        return new Allow(reason, (lj, co) -> differ(lj, co, true) == null && ok.test(lj.last().line(0),
            co.last().line(0)));
    }

    static List<Scenario> scenarios() {
        List<Scenario> l = new ArrayList<>();
        l.add(new Scenario("1", "g1", false, MachineSpecs::g1, MachineSpecs::g1Expect, null));
        l.add(new Scenario("2", "yield", false, (m, f) -> run(m, f,
            "term.write(\"S1 hello\")\ncoroutine.yield()\nerror(\"after\", 0)\n", 2), both(r -> r.steps.size() == 2
            && r.steps.get(0).kind().equals("OK") ? null : "expected OK then an error: " + r.show(),
            both(errorLine("after"), line0Starts("S1 hello"))), null));
        l.add(new Scenario("3", "returns", false, bios("term.write(\"x\")\n"),
            r -> r.last().kind().equals("GENERIC_ERROR") ? null : "expected GENERIC_ERROR, got " + r.show(), null));
        l.add(new Scenario("4", "bad_arg", false, bios("term.setCursorPos(\"x\")\n"),
            errorLine("bios.lua:1: bad argument #1 (number expected, got string)"), null));
        l.add(new Scenario("5a", "levels: t.lvl(0)", false, bios("t.lvl(0)\n"), errorLine("lvl0"), null));
        l.add(new Scenario("5b", "levels: t.lvl(1)", false, bios("t.lvl(1)\n"), errorLine("bios.lua:1: lvl1"), null));
        l.add(new Scenario("5c", "levels: t.lvl(2)", false, bios("local function f() t.lvl(2) end\nf()\n"),
            errorLine("bios.lua:2: lvl2"), null));
        l.add(new Scenario("5d", "levels: t.lvl(3)", false,
            bios("local function g() t.lvl(3) end\nlocal function f() g() end\nf()\n"),
            errorLine("bios.lua:3: lvl3"), null));
        l.add(new Scenario("5e", "levels: t.lvl(-1)", false, bios("t.lvl(-1)\n"), errorLine("lvl-1"), null));
        l.add(new Scenario("5f", "levels: t.nullmsg()", false, bios("t.nullmsg()\n"), errorLine("nil"), null));
        l.add(new Scenario("6", "java_exception", false, bios("t.boom()\n"),
            errorLine("Java Exception Thrown: java.lang.RuntimeException: kaboom"), null));
        l.add(new Scenario("7", "pcall_continue", false, bios("local ok, e = pcall(term.setCursorPos, \"x\")\n"
            + "term.write(\"after\")\nerror(tostring(ok) .. \"|\" .. e, 0)\n"),
            both(errorLine("false|bad argument #1 (number expected, got string)"), line0Starts("after")),
            lastLine("F5: luaL_where adds no position for a C function called by pcall; Cobalt adds bios.lua:1: "
                + "(decided in S3)", (lj, co) -> co.equals(lj.replace("false|", "false|bios.lua:1: ")))));
        l.add(new Scenario("8", "results", false,
            bios("term.setCursorPos(3, 4)\nlocal x, y = term.getCursorPos()\nerror(x .. \",\" .. y, 0)\n"),
            errorLine("3,4"), null));
        l.add(new Scenario("9", "latin1", false, bios("local s = t.str()\nlocal e = t.echo(\"\\255\\0\\128\")\n"
            + "term.write(\"\\233\\128\")\n"
            + "error(#s .. \":\" .. table.concat({s:byte(1, -1)}, \",\") .. \"|\"\n"
            + "  .. (e == \"\\255\\0\\128\" and \"same\" or \"changed\"), 0)\n"),
            both(errorLine("4:233,63,63,63|same"), line0Starts("\u00e9\u0080")), null));
        l.add(new Scenario("10", "coerce", false, bios("error(t.coerce(1, 1.5, nil, 1e100, 0/0, -0.0, 2^53), 0)\n"),
            errorLine("1|1.5|nil|1.0E100|NaN|0|9.007199254740992E15"),
            lastLine("F7: ObjectArguments.getStringCoerced uses Double.toString for 1e100, NaN and 2^53; Cobalt "
                + "formats them as Lua does (S3 replaces IArguments)", (lj, co) -> {
                String[] a = lj.split("\\|", -1), b = co.split("\\|", -1);
                if (a.length != b.length) return false;
                for (int i = 0; i < a.length; i++) if (i != 3 && i != 4 && i != 6 && !a[i].equals(b[i])) return false;
                return true;
            })));
        l.add(new Scenario("11", "weird", false, (m, f) -> {
            Run r = run(m, f, "local a = t.weird()\nlocal b = t.weird()\n"
                + "error(tostring(a) .. \"|\" .. tostring(b), 0)\n", 1);
            r.warnTag = "SpecAPI$Weird";
            return r;
        }, both(errorLine("nil|nil"), r -> warnings(r.log, "SpecAPI$Weird") == 1 ? null
            : "expected one WARN for the two calls (D9), got " + warnings(r.log, "SpecAPI$Weird")), null));
        l.add(new Scenario("12a", "error_values: nil", false, bios("error(nil)\n"), errorLine("nil"), null));
        l.add(new Scenario("12b", "error_values: true", false, bios("error(true)\n"), errorLine("true"), null));
        l.add(new Scenario("12c", "error_values: {}", false, bios("error({})\n"), errorMatches(TABLE_NAME),
            lastLine("a table's name holds its identity, which differs per run",
                (lj, co) -> co.startsWith("table: "))));
        l.add(new Scenario("12d", "error_values: 42", false, bios("error(42)\n"), errorLine("bios.lua:1: 42"),
            lastLine("F6 (P13): LuaJIT's error adds a position to a number, which lua_isstring accepts; Cobalt "
                + "does not", (lj, co) -> co.equals("42"))));
        l.add(new Scenario("13", "no_tostring", false, (m, f) -> {
            Run r = run(m, f, "error(setmetatable({}, {__tostring = function() t.mark() return \"x\" end}))\n", 1);
            r.extra = "mark() called " + r.marks + " time(s)";
            return r;
        }, both(errorMatches(Pattern.compile("^table: ")), r -> r.marks == 0 ? null : "__tostring ran " + r.marks
            + " time(s)"), lastLine("a table's name holds its identity, which differs per run",
            (lj, co) -> co.startsWith("table: "))));
        l.add(new Scenario("14", "syntax", false, bios("x = = 1\n"), r -> "bios.lua:1: unexpected symbol near '='"
            .equals(r.exception) ? null : "expected that MachineException, got " + r.show(), null));
        // M1.md expected Cobalt's wording to differ; both say "attempt to load a binary chunk".
        l.add(new Scenario("15", "binary", false, bios("\u001bLJ\u0002\u0000return 1\n"),
            r -> r.exception != null && r.exception.contains("binary chunk") ? null
                : "expected a MachineException about a binary chunk, got " + r.show(), null));
        l.add(new Scenario("16", "table_arg", true, bios("term.write({})\n"), r -> {
            String x = errorMatches(Pattern.compile("^bios\\.lua:1: ")).apply(r);
            if (x != null) return x;
            return r.last().line(0).contains("tables are not supported yet") ? null : "first line " + r.last().show();
        }, null));
        l.add(new Scenario("17", "environment", true, bios("local found = {}\n"
            + "for _, n in ipairs({\"io\", \"os\", \"package\", \"require\", \"dofile\", \"loadfile\",\n"
            + "  \"collectgarbage\",\"gcinfo\", \"newproxy\", \"print\", \"jit\", \"debug\", \"bit\", \"module\"}) do\n"
            + "  if _G[n] ~= nil then found[#found + 1] = n end\nend\n"
            + "if string.dump ~= nil then found[#found + 1] = \"string.dump\" end\n"
            + "error(\"[\" .. table.concat(found, \" \") .. \"]\", 0)\n"), errorLine("[]"), null));
        l.add(new Scenario("18", "oom", true, bios("local ok, e = pcall(string.rep, \"x\", 100 * 1024 * 1024)\n"
            + "error(tostring(ok) .. \" \" .. tostring(e) .. \" \" .. t.echo(\"alive\"), 0)\n"),
            errorLine("false not enough memory alive"), null));
        l.add(new Scenario("19", "close_while_running", true, MachineSpecs::closeWhileRunning,
            r -> r.fault != null ? r.fault : r.last().kind().equals("TIMEOUT") ? null
                : "expected TIMEOUT, got " + r.show(), null));
        l.add(new Scenario("20", "reentry", true, bios("t.reenter()\n"),
            errorLine("Java Exception Thrown: java.lang.IllegalStateException: Machine is already running"), null));
        l.add(new Scenario("21", "churn", true, MachineSpecs::churn,
            r -> r.extra.startsWith("200 of 200 ") ? null : r.extra, null));
        l.add(new Scenario("22", "many_calls", true, (m, f) -> {
            Run r = run(m, f, "for i = 1, 100000 do t.nop() end\nerror(\"done\", 0)\n", 1);
            r.extra = String.format("%.0f ns per call (100,000 calls, one resume)", r.eventNanos / 1e5);
            return r;
        }, errorLine("done"), null));
        return l;
    }

    /** Scenario 1: the constructor, handleEvent and close each on a thread of its own; then the machine is closed. */
    static Run g1(String machine, ILuaMachine.Factory factory) throws Exception {
        Env e = new Env();
        Run r = new Run(machine);
        ILuaMachine m;
        try {
            m = onThread("spec-create", () -> e.create(factory, G1_BIOS));
        } catch (MachineException x) {
            r.exception = x.getMessage();
            e.finish(r);
            return r;
        }
        try {
            r.steps.add(onThread("spec-event", () -> e.event(m, r)));
            onThread("spec-close", () -> {
                m.close();
                return null;
            });
            e.finish(r);   // the terminal as the bios left it
            r.steps.add(e.event(m, r));
            m.close();
            m.close();
        } finally {
            m.close();   // so that a failure here does not leave a machine for the next scenario's LIVE check
        }
        r.extra = "create on spec-create, handleEvent on spec-event, close on spec-close";
        return r;
    }

    /** What scenario 1 expects of a run (also for each run of scenario 21). */
    static String g1Expect(Run r) {
        if (r.exception != null) return "MachineException: " + esc(r.exception);
        if (r.steps.size() != 2) return "expected 2 steps, got " + r.steps.size();
        Step s = r.steps.get(0);
        if (!s.kind().equals("error") || !s.line(0).equals("bios.lua:3: boom")) return "step 1: " + s.show();
        if (!s.message().contains("\n\tbios.lua:3: in main chunk")) return "no frame 'bios.lua:3: in main chunk'";
        Step t = r.steps.get(1);
        if (!t.kind().equals("throws")
            || !t.message().equals("java.lang.IllegalStateException: Machine has been closed")) {
            return "step 2: " + t.show();
        }
        if (!r.line0.startsWith("hi") || r.cursorX != 2) {
            return "terminal: '" + esc(r.line0.stripTrailing()) + "', cursor x " + r.cursorX;
        }
        return null;
    }

    /**
     * Scenario 19: close() from another thread while a host call blocks; then the host call returns. The scenario
     * fails unless the host call was reached, close() returned within 2 s while it blocked, and the host call was
     * released by the test (not by its own 10 s limit).
     */
    static Run closeWhileRunning(String machine, ILuaMachine.Factory factory) throws Exception {
        Env e = new Env();
        Run r = new Run(machine);
        ILuaMachine m = e.create(factory, "t.block()\nerror(\"not stopped\", 0)\n");
        try {
            AtomicReference<Step> result = new AtomicReference<>();
            Thread worker = new Thread(() -> result.set(e.event(m, r)), "spec-event");
            worker.start();
            if (!e.api.blocked.await(5, TimeUnit.SECONDS)) r.fault = "t.block() was not reached in 5 s";
            Thread closer = new Thread(m::close, "spec-close");
            closer.start();
            closer.join(2000);
            if (closer.isAlive() && r.fault == null) r.fault = "close() did not return in 2 s during t.block()";
            e.api.release.countDown();
            worker.join();
            closer.join();
            if (!e.api.released && r.fault == null) {
                r.fault = "t.block() did not end through the test's release (timed out or interrupted)";
            }
            r.steps.add(result.get());
        } finally {
            e.api.release.countDown();
            m.close();
        }
        e.finish(r);
        r.extra = "close() on spec-close during t.block()";
        return r;
    }

    /** Scenario 21: scenario 1 on 200 machines, one after another, each checked as scenario 1 is. */
    static Run churn(String machine, ILuaMachine.Factory factory) throws Exception {
        Run r = new Run(machine);
        int good = 0;
        String first = null;
        for (int i = 0; i < 200; i++) {
            Run one = g1(machine, factory);
            String x = g1Expect(one);
            if (x == null) x = traceback(one);
            if (x == null) good++;
            else if (first == null) first = "run " + i + ": " + x;
        }
        r.extra = good + " of 200 passed scenario 1's checks" + (first != null ? "; " + first : "");
        r.steps.add(new Step("churn", null));
        return r;
    }

    // ---- Comparison -------------------------------------------------------------------------------------------------

    /**
     * Null if the two runs agree on what the differential compares: the {@code MachineException} text, the kind and
     * first message line of each step, and terminal line 0. With {@code exceptLastLine}, the first line of the last
     * step is left out.
     */
    static String differ(Run a, Run b, boolean exceptLastLine) {
        if (!java.util.Objects.equals(a.exception, b.exception)) {
            return "MachineException '" + esc(a.exception) + "' vs '" + esc(b.exception) + "'";
        }
        if (a.steps.size() != b.steps.size()) return a.steps.size() + " vs " + b.steps.size() + " results";
        for (int i = 0; i < a.steps.size(); i++) {
            Step x = a.steps.get(i), y = b.steps.get(i);
            if (!x.kind().equals(y.kind())) return "result " + (i + 1) + ": " + x.kind() + " vs " + y.kind();
            boolean skip = exceptLastLine && i == a.steps.size() - 1;
            if (!skip && !x.line(0).equals(y.line(0))) {
                return "result " + (i + 1) + ": '" + esc(x.line(0)) + "' vs '" + esc(y.line(0)) + "'";
            }
        }
        if (!a.line0.equals(b.line0)) {
            return "terminal line 0 '" + esc(a.line0.stripTrailing()) + "' vs '" + esc(b.line0.stripTrailing()) + "'";
        }
        return null;
    }

    /** Every error result must carry a traceback: its second line is {@code stack traceback:} (D7, F21). */
    static String traceback(Run r) {
        for (Step s : r.steps) {
            if (s.kind().equals("error") && !s.line(1).equals("stack traceback:")) {
                return r.machine + ": error without 'stack traceback:' as its second line: " + s.show();
            }
        }
        return null;
    }

    static int warnings(String log, String what) {
        int n = 0;
        for (String line : log.split("\n")) if (line.contains("WARN") && line.contains(what)) n++;
        return n;
    }

    // ---- Running ----------------------------------------------------------------------------------------------------

    static Run runOne(Scenario s, String machine, ILuaMachine.Factory factory, long watchdogS) throws Exception {
        ScheduledFuture<?> dog = watchdog(s.id() + " " + s.name() + " (" + machine + ")", watchdogS);
        tee.start();
        Run r = null;
        try {
            r = s.body().run(machine, factory);
            return r;
        } finally {
            String log = tee.stop();
            if (r != null) r.log = log;
            dog.cancel(false);
        }
    }

    static void scenario(Scenario s, ILuaMachine.Factory luajit, ILuaMachine.Factory cobalt) {
        List<String> problems = new ArrayList<>();
        String note = "";
        Run lj = null, co = null;
        try {
            // Counted per scenario, so a leak is reported by the scenario that made it and no other.
            int live0 = NativeMachine.live(), leaked0 = NativeMachine.leaked();
            lj = runOne(s, "luajit", luajit, 10);
            String x = s.expect().apply(lj);
            if (x != null) problems.add(x);
            if ((x = traceback(lj)) != null) problems.add(x);
            if (NativeMachine.live() != live0) {
                problems.add((NativeMachine.live() - live0) + " native machine(s) left live");
            }
            if (NativeMachine.leaked() != leaked0) {
                problems.add((NativeMachine.leaked() - leaked0) + " native free(s) refused (leaked)");
            }
            if (!s.luajitOnly()) {
                co = runOne(s, "cobalt", cobalt, 10);
                if ((x = traceback(co)) != null) problems.add(x);
                String d = differ(lj, co, false);
                if (d == null) {
                    if (s.allow() != null) note = "allow-list entry not needed: " + s.allow().reason();
                } else if (s.allow() != null && s.allow().accepts().test(lj, co)) {
                    note = "allowed (" + s.allow().reason() + "): " + d;
                } else {
                    problems.add("differs from Cobalt: " + d);
                }
            }
        } catch (Throwable t) {
            problems.add("threw " + t);
            t.printStackTrace(System.out);
        }
        String verdict = !problems.isEmpty() ? "FAIL" : note.startsWith("allowed") ? "allowed" : "ok";
        switch (verdict) {
            case "FAIL" -> failed++;
            case "allowed" -> allowed++;
            default -> passed++;
        }
        System.out.println(String.format("%-4s %-3s %s", verdict.equals("allowed") ? "ok" : verdict, s.id(), s.name()));
        System.out.println("       luajit: " + (lj == null ? "-" : lj.show()));
        if (co != null) System.out.println("       cobalt: " + co.show());
        for (String p : problems) System.out.println("       problem: " + p);
        if (!note.isEmpty()) System.out.println("       note: " + note);
        report.add(String.join("\t", s.id(), s.name(), verdict, lj == null ? "-" : lj.show(),
            co == null ? "-" : co.show(), String.join("; ", problems) + (note.isEmpty() ? "" : " " + note)));
    }

    /** Arms the watchdog: after {@code seconds}, print every thread's stack and halt the JVM with exit code 2. */
    static ScheduledFuture<?> watchdog(String what, long seconds) {
        return WATCHDOG.schedule(() -> {
            System.out.println("machineSpecs: " + what + " did not finish in " + seconds + " s; halting");
            for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                System.out.println("  thread " + e.getKey().getName() + " (" + e.getKey().getState() + ")");
                for (StackTraceElement f : e.getValue()) System.out.println("      at " + f);
            }
            System.out.flush();
            Runtime.getRuntime().halt(2);
        }, seconds, TimeUnit.SECONDS);
    }

    // ---- Option B: through Computer ---------------------------------------------------------------------------------

    /** Records what the machines made by a factory do. */
    static final class Recorder {
        final AtomicInteger creates = new AtomicInteger(), closes = new AtomicInteger();
        final List<MachineResult> results = new CopyOnWriteArrayList<>();
        volatile String createThread = "-", eventThread = "-";
    }

    static ILuaMachine.Factory recording(ILuaMachine.Factory inner, AtomicReference<Recorder> current) {
        return (env, bios) -> {
            Recorder rec = current.get();
            rec.creates.incrementAndGet();
            rec.createThread = Thread.currentThread().getName();
            ILuaMachine m = inner.create(env, bios);
            return new ILuaMachine() {
                @Override
                public MachineResult handleEvent(String name, Object[] args) {
                    rec.eventThread = Thread.currentThread().getName();
                    MachineResult r = m.handleEvent(name, args);
                    rec.results.add(r);
                    return r;
                }

                @Override
                public void printExecutionState(StringBuilder out) {
                    m.printExecutionState(out);
                }

                @Override
                public void close() {
                    rec.closes.incrementAndGet();
                    m.close();
                }
            };
        };
    }

    static void optionB(ILuaMachine.Factory luajit) throws Exception {
        // Above the worst case of the waits below (3 drives of at most 20 s, and 2 s in B3), so that a condition that
        // is never met is reported as a FAIL of its check, not as a halt.
        ScheduledFuture<?> dog = watchdog("Option B", 75);
        AtomicReference<String> bios = new AtomicReference<>(G1_BIOS);
        AtomicReference<Recorder> current = new AtomicReference<>(new Recorder());
        GlobalEnvironment global = new GlobalEnvironment() {
            @Override
            public String getHostString() {
                return HOST;
            }

            @Override
            public String getUserAgent() {
                return "computercraft/" + HeadlessRunner.CCT_VERSION + " MachineSpecs";
            }

            @Override
            public Mount getRomMount() {
                return new MemoryMount();
            }

            @Override
            public InputStream getLuaBios() {
                return new ByteArrayInputStream(bios.get().getBytes(StandardCharsets.ISO_8859_1));
            }
        };
        ComputerEnvironment computerEnv = new ComputerEnvironment() {
            @Override
            public int getDay() {
                return 1;
            }

            @Override
            public double getTimeOfDay() {
                return 6.0;
            }

            @Override
            public MetricsObserver getMetrics() {
                return MetricsObserver.discard();
            }

            @Override
            public WritableMount createRootMount() {
                return new MemoryMount();
            }
        };
        MainThread mainThread = new MainThread();
        ComputerContext ctx = ComputerContext.builder(global).computerThreads(2).mainThreadScheduler(mainThread)
            .luaFactory(recording(luajit, current)).disableHttp().build();
        try {
            // B1: the G1 bios through a real Computer.
            Recorder rec = current.get();
            Terminal term = new Terminal(51, 19, false);
            Computer computer = new Computer(ctx, computerEnv, term, 0);
            computer.turnOn();
            drive(computer, mainThread, () -> !rec.results.isEmpty() && rec.closes.get() >= 1);
            List<String> p = new ArrayList<>();
            Step s = rec.results.isEmpty() ? new Step("none", null) : step(rec.results.get(0));
            if (!s.kind().equals("error") || !s.line(0).equals("bios.lua:3: boom") || !s.line(1).equals(
                "stack traceback:")) p.add("result " + s.show());
            if (!term.getLine(0).toString().startsWith("Error running computer")) {
                p.add("terminal line 0 '" + esc(term.getLine(0).toString().stripTrailing()) + "'");
            }
            if (!term.getLine(1).toString().startsWith("bios.lua:3: boom")) {
                p.add("terminal line 1 '" + esc(term.getLine(1).toString().stripTrailing()) + "'");
            }
            if (rec.closes.get() < 1) p.add("close() was not called");
            optionResult("B1", "error bios through Computer", p, "result " + s.show() + "; close() called "
                + rec.closes.get() + " time(s); create on " + rec.createThread + ", handleEvent on "
                + rec.eventThread);

            // B2: a bios that does not compile; turning the computer on again calls the factory again.
            Recorder rec2 = new Recorder();
            current.set(rec2);
            bios.set("x = = 1\n");
            Terminal term2 = new Terminal(51, 19, false);
            Computer computer2 = new Computer(ctx, computerEnv, term2, 1);
            // CC:T resets the terminal before it calls the factory and writes the failure after it, line 0 first:
            // so wait for both lines of this boot's failure, not just for the factory call.
            java.util.function.BooleanSupplier shown = () -> term2.getLine(0).toString().startsWith(
                "Error loading bios.lua") && !term2.getLine(1).toString().isBlank();
            computer2.turnOn();
            drive(computer2, mainThread, () -> rec2.creates.get() >= 1 && shown.getAsBoolean());
            String first = term2.getLine(1).toString().stripTrailing();
            computer2.turnOn();
            drive(computer2, mainThread, () -> rec2.creates.get() >= 2 && shown.getAsBoolean());
            String second = term2.getLine(1).toString().stripTrailing();
            p = new ArrayList<>();
            if (!term2.getLine(0).toString().startsWith("Error loading bios.lua")) {
                p.add("terminal line 0 '" + esc(term2.getLine(0).toString().stripTrailing()) + "'");
            }
            for (String line : List.of(first, second)) {
                if (!line.equals("bios.lua:1: unexpected symbol near '='")) {
                    p.add("terminal line 1 '" + esc(line) + "'");
                }
            }
            if (rec2.creates.get() != 2) p.add("the factory was called " + rec2.creates.get() + " time(s), expected 2");
            optionResult("B2", "syntax error, retried", p, "factory called " + rec2.creates.get()
                + " time(s); line 1 '" + esc(first) + "', then '" + esc(second) + "'");

            // B3: the context closes in time.
            computer.shutdown();
            computer2.shutdown();
            for (int i = 0; i < 20 && (computer.isOn() || computer2.isOn()); i++) {
                computer.tick();
                computer2.tick();
                Thread.sleep(50);
            }
            boolean closed = ctx.close(1, TimeUnit.SECONDS);
            optionResult("B3", "context closes", closed ? List.of() : List.of("ctx.close(1, SECONDS) returned false"),
                "ctx.close(1, SECONDS) = " + closed);
        } finally {
            dog.cancel(false);
        }
    }

    static void drive(Computer computer, MainThread mainThread, java.util.function.BooleanSupplier done)
        throws InterruptedException {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
        while (!done.getAsBoolean() && System.nanoTime() < end) {
            computer.tick();
            mainThread.tick();
            Thread.sleep(20);
        }
    }

    static void optionResult(String id, String name, List<String> problems, String detail) {
        String verdict = problems.isEmpty() ? "ok" : "FAIL";
        if (problems.isEmpty()) passed++;
        else failed++;
        System.out.println(String.format("%-4s %-3s %s", verdict, id, name));
        System.out.println("       " + detail);
        for (String p : problems) System.out.println("       problem: " + p);
        report.add(String.join("\t", id, name, verdict, detail, "-", String.join("; ", problems)));
    }

    // ---- Main -------------------------------------------------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        Path reportFile = null;
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--report") && i + 1 < args.length) reportFile = Path.of(args[++i]);
            else throw new IllegalArgumentException("usage: MachineSpecs [--report <file.tsv>]");
        }
        // A run that halts or exits early writes no report: never leave the previous run's report behind.
        if (reportFile != null) Files.deleteIfExists(reportFile);
        tee = new Tee(System.err);
        System.setErr(new PrintStream(tee, true, System.err.charset()));
        System.out.println("machineSpecs: java " + System.getProperty("java.version") + " on "
            + System.getProperty("os.name") + " " + System.getProperty("os.arch") + ", CC:T "
            + HeadlessRunner.CCT_VERSION);

        String dir = System.getProperty("ccluajit.nativeDir");
        Path cache = dir != null ? Path.of(dir) : Path.of(System.getProperty("java.io.tmpdir"), "ccluajit-natives");
        try {
            System.out.println("natives: " + NativeLibrary.ensureLoaded(cache));
        } catch (Exception | LinkageError e) {
            System.out.println("machineSpecs: the natives could not be loaded: " + e);
            System.exit(3);
        }

        ILuaMachine.Factory luajit = LuaJITMachine::new, cobalt = CobaltLuaMachine::new;
        for (Scenario s : scenarios()) scenario(s, luajit, cobalt);
        try {
            optionB(luajit);
        } catch (Throwable t) {
            failed++;
            System.out.println("FAIL B   Option B threw " + t);
            t.printStackTrace(System.out);
        }

        System.out.println("machineSpecs: " + (passed + allowed) + " passed (" + allowed + " with an allowed "
            + "difference from Cobalt), " + failed + " failed; native machines live " + NativeMachine.live()
            + ", leaked " + NativeMachine.leaked());
        if (reportFile != null) {
            Files.createDirectories(reportFile.toAbsolutePath().getParent());
            List<String> lines = new ArrayList<>();
            lines.add("id\tname\tverdict\tluajit\tcobalt\tproblems and notes");
            lines.addAll(report);
            Files.write(reportFile, lines, StandardCharsets.UTF_8);
            System.out.println("report: " + reportFile.toAbsolutePath());
        }
        System.out.flush();
        System.exit(failed == 0 && NativeMachine.live() == 0 && NativeMachine.leaked() == 0 ? 0 : 1);
    }

    // ---- Helpers ----------------------------------------------------------------------------------------------------

    /** Runs {@code c} on a new thread named {@code name}, waits for it, and passes on what it threw. */
    @SuppressWarnings("unchecked")
    static <T> T onThread(String name, Callable<T> c) throws Exception {
        Object[] box = new Object[2];
        Thread t = new Thread(() -> {
            try {
                box[0] = c.call();
            } catch (Throwable x) {
                box[1] = x;
            }
        }, name);
        t.start();
        t.join();
        if (box[1] instanceof Exception x) throw x;
        if (box[1] instanceof Error x) throw x;
        return (T) box[0];
    }

    /** For the report: control characters and anything outside printable ASCII as escapes. */
    static String esc(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\n') b.append("\\n");
            else if (c == '\t') b.append("\\t");
            else if (c == '\\') b.append("\\\\");
            else if (c < 0x20 || c > 0x7e) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.toString();
    }

    /** Passes everything to {@code out}, and keeps a copy while started. */
    static final class Tee extends OutputStream {
        private final OutputStream out;
        private ByteArrayOutputStream copy;   // guarded by this

        Tee(OutputStream out) {
            this.out = out;
        }

        synchronized void start() {
            copy = new ByteArrayOutputStream();
        }

        /** The copy since {@link #start}, and stops copying. */
        synchronized String stop() {
            String s = copy == null ? "" : copy.toString(StandardCharsets.UTF_8);
            copy = null;
            return s;
        }

        @Override
        public synchronized void write(int b) throws IOException {
            out.write(b);
            if (copy != null) copy.write(b);
        }

        @Override
        public synchronized void write(byte[] b, int off, int len) throws IOException {
            out.write(b, off, len);
            if (copy != null) copy.write(b, off, len);
        }

        @Override
        public void flush() throws IOException {
            out.flush();
        }
    }
}
