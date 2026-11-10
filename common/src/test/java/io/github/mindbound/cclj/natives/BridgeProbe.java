package io.github.mindbound.cclj.natives;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.management.ManagementFactory;
import java.math.BigDecimal;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryIteratorException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Predicate;

/**
 * Tier 1 of S1 (docs/port-1.21.1/M1.md section 3.15): the JNI bridge through the production {@link NativeMachine},
 * {@link NativeLibrary} and {@link Wire}, with a fake host and bioses written here. JDK-only: it is compiled with
 * nothing but the classes of the {@code natives} package and {@link NativeProbe} (F17).
 * <p>
 * Usage: {@code java -Xcheck:jni -Xmx256m -cp <classes><sep>natives/build/out
 * io.github.mindbound.cclj.natives.BridgeProbe <cache-dir>}. Write stdout and stderr to one log
 * ({@code 2>&1 | tee <log>} under {@code set -o pipefail}) and scan it with natives/tests/jni-scan.sh: -Xcheck:jni
 * reports problems in the output and does not change the exit code (F13). -Xmx256m matters: the local-reference
 * test relies on it.
 * <p>
 * It loads the library with {@link NativeLibrary#ensureLoaded} (extracting it below {@code <cache-dir>}), prints
 * one line per check ({@code "ok   ..."} or {@code "FAIL ..."}), the time per host call, and a summary line. Each
 * group of checks runs on a thread of its own, so local references that a broken jni_machine.c leaks die with that
 * thread when the group ends. The main thread waits at most 240 s for a group: a late group fails and goes on in the
 * background (it can still print check lines) while the next one starts, so the report still comes out. One check
 * starts a second JVM (the same {@code java}, the same class path, {@code -Xmx64m}) whose library load must fail.
 * Exit code 0: every check passed; 1: a check failed, or the watchdog stopped the run; 2: no library is bundled for
 * this platform; 3: the library could not be loaded, or its ABI does not match.
 * <p>
 * The checks (M1.md section 3.15, Tier 1 items 1-9; the decoders of {@link Wire}, {@code NativeMachine.ended}, and
 * the cleanup of {@link NativeLoader}):
 * <ol>
 *   <li>the ABI; {@link NativeLibrary} loads once (a second call with another cache root returns the first path),
 *       and a failed load is sticky (in the second JVM); the G1 path: {@code term.write("hi")} then
 *       {@code error("boom")} on line 3, with the exact message and traceback offset; yield, return, load errors,
 *       error levels, Latin-1 through the machine;</li>
 *   <li>the host throws {@link RuntimeException} and {@code new OutOfMemoryError("fake")}: a fixed Lua error, no
 *       exception left pending, the exception printed to System.err (ExceptionDescribe), and the machine goes on
 *       under {@code pcall}; a request that the full Java heap cannot hold (a JNI failure) gives a fixed Lua error
 *       too;</li>
 *   <li>the host returns null, or more than 16 MiB;</li>
 *   <li>10,000 seeded random replies (and fixed malformed ones): each gives its values, its error, or
 *       {@code CCLuaJIT: corrupt host reply}, as a Java model of the reply checker predicts; never a crash;</li>
 *   <li>a re-entrant {@code resume} from the host gives {@link IllegalStateException}, which becomes a Lua error;
 *       {@code close()} from inside an upcall is deferred;</li>
 *   <li>{@code close()} from another thread while the host blocks on a latch: the free is deferred,
 *       {@code closedWhileRunning} is true, and no machine is left at the end; also 300 rounds of {@code close()}
 *       racing {@code resume}, of which some must land during the resume (not required when the JVM sees one
 *       CPU) and some outside it;</li>
 *   <li>create on thread A, resume on B, close on C;</li>
 *   <li>local references: one resume with 100,000 upcalls of 4 KiB each way; inside the last upcall, after
 *       {@link System#gc()}, the used heap is below the baseline + 32 MiB (-Xcheck:jni reports no local-reference
 *       leak, F13); the same for the failing exits of the host call (null, an exception, a reply over 16 MiB),
 *       with 128 failing upcalls of 448 KiB each and a limit of baseline + 16 MiB;</li>
 *   <li>the time per host call ({@code t.echo("hello")}: a string in, a string out), median of 5 runs.</li>
 * </ol>
 */
public final class BridgeProbe {
    private BridgeProbe() {
    }

    private static final long MEM = 64L << 20;           // the machine cap LuaJITMachine uses
    private static final int WIRE_MAX = 16 << 20;        // CM_WIRE_MAX
    private static final int MSG_MAX = 64 << 10;         // CM_MSG_MAX
    private static final long WAIT_S = 60;               // latches and helper threads
    private static final long SECTION_S = 240;           // one section (a group of checks)
    private static final long WATCHDOG_S = 900;          // the whole run
    private static final String G1_BIOS = "term.write(\"hi\")\nlocal x = 1\nerror(\"boom\")";
    private static final String G1_TEXT =
        "bios.lua:3: boom\nstack traceback:\n\t[C]: in function 'error'\n\tbios.lua:3: in main chunk";
    private static final String CORRUPT = "CCLuaJIT: corrupt host reply";
    private static final String JAVA_EXCEPTION = "CCLuaJIT: Java exception in host call";

    private static final Consumer<Object> NO_UNKNOWN = o -> {
        throw new AssertionError("BridgeProbe: unexpected value type " + o.getClass().getName());
    };
    private static final byte[] NONE = Wire.values(null, NO_UNKNOWN);
    private static final Random KEYS = new Random(0x5eed1d);   // id keys of the machines

    private static int checks, failed;
    private static Path cache, lib;       // the cache root and the loaded library; set before the sections run
    private static boolean checkJni;

    public static void main(String[] args) {
        if (args.length == 2 && args[0].equals("--library-failure")) {
            exit(libraryFailureChild(Path.of(args[1])));
            return;
        }
        if (args.length != 1) {
            System.err.println("usage: BridgeProbe <cache-dir>");
            System.exit(64);
        }
        cache = Path.of(args[0]).toAbsolutePath();
        checkJni = ManagementFactory.getRuntimeMXBean().getInputArguments().contains("-Xcheck:jni");
        System.out.println("java " + System.getProperty("java.version") + " (" + System.getProperty("java.vm.name")
            + ") on " + System.getProperty("os.name") + " " + System.getProperty("os.arch") + ", -Xcheck:jni "
            + (checkJni ? "on" : "OFF") + ", max heap " + (Runtime.getRuntime().maxMemory() >> 20) + " MiB");

        String platform;
        try {
            platform = NativeLoader.platformId();
        } catch (UnsupportedOperationException e) {
            System.err.println("bridgeProbe: " + e.getMessage());
            exit(2);
            return;
        }
        String dir = "/natives/" + platform + "/";
        URL source = NativeLoader.class.getResource(dir + NativeLoader.libraryFileName(platform));
        System.out.println("resource " + dir + NativeLoader.libraryFileName(platform) + ": "
            + (source != null ? source : "not on the classpath"));
        System.out.println("library build: " + (NativeLoader.class.getResource(dir + "DEBUG-BUILD.txt") != null
            ? "DEBUG (CCLJ_DEBUG=1, DEBUG-BUILD.txt next to it)" : "release"));

        long t0 = System.nanoTime();
        try {
            lib = NativeLibrary.ensureLoaded(cache);
        } catch (NativeLoader.NotBundledException e) {
            System.err.println("bridgeProbe: " + e.getMessage() + ". Build it first: bash natives/build-natives.sh "
                + platform);
            exit(2);
            return;
        } catch (Exception | LinkageError e) {
            System.err.println("bridgeProbe: could not load the native library: " + e);
            exit(3);
            return;
        }
        System.out.printf("loaded %s in %.1f ms%n", lib, (System.nanoTime() - t0) / 1e6);
        startWatchdog();

        section("library and ABI", BridgeProbe::testLibrary);
        section("library failure", BridgeProbe::testLibraryFailure);
        section("Wire", BridgeProbe::testWire);
        section("loader cleanup", BridgeProbe::testLoaderCleanup);
        section("G1 path", BridgeProbe::testG1);
        section("statuses", BridgeProbe::testStatuses);
        section("load errors", BridgeProbe::testLoadErrors);
        section("error levels", BridgeProbe::testLevels);
        section("Latin-1", BridgeProbe::testLatin1);
        section("long messages", BridgeProbe::testLongMessages);
        section("heap cap", BridgeProbe::testHeapCap);
        section("host failures", BridgeProbe::testHostFailures);
        section("random replies", BridgeProbe::testRandomReplies);
        section("re-entry", BridgeProbe::testReentry);
        section("close while running", BridgeProbe::testCloseWhileRunning);
        section("threads", BridgeProbe::testThreads);
        section("close racing resume", BridgeProbe::testCloseRace);
        section("local references", BridgeProbe::testLocalRefs);
        section("ns per call", () -> testNsPerCall(checkJni));
        // Last: they fill the Java heap and throw from the host, which slows the timing above by about 8 % when
        // it runs after them.
        section("JNI failure", BridgeProbe::testJniFailure);
        section("local references, failing host calls", BridgeProbe::testLocalRefsOnFailures);
        check("no machine left", NativeMachine.live() == 0 && NativeMachine.leaked() == 0,
            "live " + NativeMachine.live() + ", leaked " + NativeMachine.leaked());

        int all, bad;
        synchronized (BridgeProbe.class) {
            all = checks;
            bad = failed;
        }
        System.out.println("bridgeProbe: " + all + " checks, " + bad + " failed");
        if (bad != 0) {
            System.out.println("bridgeProbe: FAILED");
            exit(1);
        }
        System.out.println("bridgeProbe: OK");
        exit(0);
    }

    // ---- 1. ABI and the G1 path ------------------------------------------------------------------------------------

    private static void testLibrary() throws Exception {
        int abi = NativeMachine.nAbi();
        check("ABI", abi == NativeMachine.ABI, "nAbi() = " + abi + ", NativeMachine.ABI = " + NativeMachine.ABI);
        Path again = NativeLibrary.ensureLoaded(cache.resolve("second-root"));
        check("NativeLibrary: a second ensureLoaded, with another cache root, returns the first path",
            again.equals(lib) && NativeLibrary.isLoaded(), "returned " + again);
    }

    /**
     * The sticky failure of {@link NativeLibrary}, in a second JVM ({@link #libraryFailureChild}): this JVM has
     * loaded the library already. The child's output goes to a file and then into this log.
     */
    private static void testLibraryFailure() throws Exception {
        Path dir = cache.resolve("library-failure");
        Files.createDirectories(dir);
        Path out = dir.resolve("child.log");
        List<String> cmd = new ArrayList<>();
        cmd.add(ProcessHandle.current().info().command()
            .orElse(Path.of(System.getProperty("java.home"), "bin", "java").toString()));
        if (checkJni) cmd.add("-Xcheck:jni");
        cmd.addAll(List.of("-Xmx64m", "-cp", System.getProperty("java.class.path"), BridgeProbe.class.getName(),
            "--library-failure", dir.toString()));
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).redirectOutput(out.toFile()).start();
        boolean done = p.waitFor(WAIT_S, TimeUnit.SECONDS);
        if (!done) p.destroyForcibly().waitFor(WAIT_S, TimeUnit.SECONDS);
        List<String> lines = Files.readAllLines(out, StandardCharsets.ISO_8859_1);
        for (String l : lines) System.out.println("  child: " + l);
        check("NativeLibrary: a failed load is sticky (second JVM: the same exception again, nothing loaded)",
            done && p.exitValue() == 0, done ? "child exit " + p.exitValue() : "the child did not finish in " + WAIT_S
                + " s");
    }

    /**
     * The second JVM of {@link #testLibraryFailure}. A first {@link NativeLibrary#ensureLoaded} must fail: its cache
     * root is below a regular file, so the library cannot be extracted. A second one, with a usable root, must throw
     * the same exception object and load nothing. Returns the exit code: 0 if so, 1 otherwise.
     */
    private static int libraryFailureChild(Path dir) {
        try {
            Files.createDirectories(dir);
            Path file = dir.resolve("a-file");
            Files.write(file, new byte[0]);
            Path usable = dir.resolve("usable-root-" + ProcessHandle.current().pid());
            Throwable first = null, second = null;
            try {
                NativeLibrary.ensureLoaded(file.resolve("cache"));
            } catch (Throwable t) {
                first = t;
            }
            try {
                NativeLibrary.ensureLoaded(usable);
            } catch (Throwable t) {
                second = t;
            }
            boolean created = Files.exists(usable);
            System.out.println("first call: " + first + "; second call: " + (second != null && second == first
                ? "the same exception object" : second == null ? "no exception" : second) + "; isLoaded "
                + NativeLibrary.isLoaded() + "; usable root created " + created);
            boolean ok = first instanceof IOException && !(first instanceof NativeLoader.NotBundledException)
                && second == first && !NativeLibrary.isLoaded() && !created;
            return ok ? 0 : 1;
        } catch (Throwable t) {
            System.out.println("unexpected " + t);
            return 1;
        }
    }

    private static void testG1() throws Exception {
        List<Object[]> writes = new ArrayList<>();
        AtomicReference<byte[]> first = new AtomicReference<>();
        FakeHost host = new FakeHost("term").fn("write", req -> {
            first.compareAndSet(null, req.clone());
            writes.add(Wire.decodeArgs(req));
            return NONE;
        });
        int live = NativeMachine.live();
        NativeMachine vm = host.create(G1_BIOS);
        check("G1: created", NativeMachine.live() == live + 1 && vm.stateName().equals("idle"),
            "live " + NativeMachine.live() + ", " + vm.stateName());
        NativeMachine.Outcome o = vm.resume(null);
        check("G1: status ERROR", o.status() == NativeMachine.ST_ERROR, "status " + o.status());
        check("G1: message", o.message() != null && G1_TEXT.equals(o.message().text()), text(o));
        check("G1: traceback offset 16", o.message() != null && o.message().tracebackAt() == 16,
            o.message() == null ? "no message" : "tracebackAt " + o.message().tracebackAt());
        check("G1: not closed while running", !o.closedWhileRunning(), "");
        check("G1: term.write(\"hi\") called once", writes.size() == 1 && Arrays.equals(writes.get(0),
            new Object[]{"hi"}), writes.size() + " call(s), first " + (writes.isEmpty() ? "-" : show(writes.get(0))));
        byte[] want = bytes(1, 0, 0, 0, 4, 2, 0, 0, 0, 'h', 'i');
        check("G1: request bytes 01000000 04 02000000 6869", Arrays.equals(want, first.get()), hex(first.get()));
        check("G1: freed at the error", NativeMachine.live() == live && vm.stateName().equals("closed"),
            "live " + NativeMachine.live() + ", " + vm.stateName());
        expectThrows("G1: resume after the end", IllegalStateException.class, "Machine has been closed",
            () -> vm.resume(null));
        vm.close();
        vm.close();
        check("G1: two more close() calls do nothing", NativeMachine.live() == live, "live " + NativeMachine.live());
        expectThrows("G1: checkOpen after the end", IllegalStateException.class, "Machine has been closed",
            vm::checkOpen);
    }

    private static void testStatuses() throws Exception {
        AtomicInteger calls = new AtomicInteger();
        FakeHost host = new FakeHost("t").fn("n", req -> {
            calls.incrementAndGet();
            return NONE;
        });
        int live = NativeMachine.live();
        NativeMachine vm = host.create("t.n()\ncoroutine.yield()\nt.n()\nerror(\"after\", 0)");
        NativeMachine.Outcome o = vm.resume(null);
        check("yield: status YIELD, no message, machine kept", o.status() == NativeMachine.ST_YIELD
            && o.message() == null && !o.closedWhileRunning() && vm.stateName().equals("idle")
            && NativeMachine.live() == live + 1, "status " + o.status() + ", " + vm.stateName());
        o = vm.resume(null);
        check("yield: second resume runs on to the error", o.status() == NativeMachine.ST_ERROR
            && firstLine(o).equals("after") && o.message().tracebackAt() == 5 && calls.get() == 2
            && NativeMachine.live() == live, text(o) + ", " + calls.get() + " host calls");

        vm = host.create("t.n()");
        o = vm.resume(null);
        check("return: status RETURNED, freed", o.status() == NativeMachine.ST_RETURNED
            && o.message() != null && o.message().text().isEmpty() && NativeMachine.live() == live,
            "status " + o.status() + ", " + text(o));
        vm = host.create("");
        o = vm.resume(null);
        check("empty bios: RETURNED", o.status() == NativeMachine.ST_RETURNED && NativeMachine.live() == live,
            "status " + o.status());

        NativeMachine ev = host.create("coroutine.yield()");
        o = ev.resume(new byte[0]);
        check("malformed event record (empty): INTERNAL corrupt event, freed", o.status() == NativeMachine.ST_INTERNAL
            && o.message() != null && o.message().text().equals("corrupt event")
            && o.message().tracebackAt() == -1 && NativeMachine.live() == live, "status " + o.status() + ", "
            + text(o));
        expectThrows("malformed event record: then closed", IllegalStateException.class, "Machine has been closed",
            () -> ev.resume(null));
        NativeMachine big = host.create("coroutine.yield()");
        o = big.resume(new byte[WIRE_MAX + 1]);   // copy_array refuses it before cm_resume: the message stays empty
        check("event over 16 MiB: INTERNAL with the fallback text of resume(), freed", o.status()
            == NativeMachine.ST_INTERNAL && o.message() != null && o.message().text().equals(
            "CCLuaJIT: the machine could not be resumed (status 4)") && o.message().tracebackAt() == -1
            && NativeMachine.live() == live, "status " + o.status() + ", " + text(o));
        NativeMachine keyed = host.create("coroutine.yield('key')");
        o = keyed.resume(null);
        boolean yielded = o.status() == NativeMachine.ST_YIELD;
        o = keyed.resume(new byte[WIRE_MAX + 1]);   // the message left over is the filter record of 'key'
        check("event over 16 MiB after a YIELD with a filter: the fallback text, not the filter record", yielded
            && o.status() == NativeMachine.ST_INTERNAL && o.message() != null && o.message().text().equals(
            "CCLuaJIT: the machine could not be resumed (status 4)") && NativeMachine.live() == live,
            "yielded " + yielded + ", status " + o.status() + ", " + text(o));

        NativeMachine idle = host.create("coroutine.yield()");
        check("close() of an idle machine frees it", idle.stateName().equals("idle"), "");
        idle.close();
        check("close() of an idle machine frees it: freed", NativeMachine.live() == live
            && idle.stateName().equals("closed"), "live " + NativeMachine.live());
    }

    private static void testLoadErrors() {
        FakeHost host = new FakeHost("t").fn("n", req -> NONE);
        byte[] spec = host.spec();
        expectCreateFails("load: syntax error", MEM, spec, "x = = 1",
            "bios.lua:1: unexpected symbol near '='"::equals);
        expectCreateFails("load: binary chunk refused", MEM, spec, "\033LJ\002\000\003bad",
            m -> m.contains("binary chunk"));
        expectCreateFails("load: corrupt spec", MEM, bytes(1, 0, 0, 0), "",
            "CCLuaJIT: corrupt API table description"::equals);
        byte[] longName = new byte[4 + 4 + 4 + 257 + 1 + 4];
        ByteBuffer.wrap(longName).order(ByteOrder.LITTLE_ENDIAN).putInt(1).putInt(1).putInt(257);
        Arrays.fill(longName, 12, 12 + 257, (byte) 'a');
        expectCreateFails("load: a 257-byte API name", MEM, longName, "",
            "CCLuaJIT: corrupt API table description"::equals);
        expectCreateFails("load: memLimit 0", 0, spec, "", "not enough memory"::equals);
        expectCreateFails("load: a 1-byte cap (below a fresh state) fails to load", 1, spec, "",
            "not enough memory"::equals);
    }

    private static void testLevels() throws Exception {
        FakeHost host = new FakeHost("t").fn("lvl", req -> {
            Object[] a = Wire.decodeArgs(req);
            return Wire.error((String) a[1], (int) (double) (Double) a[0]);
        });
        String bios = """
            local function inner(l, m) t.lvl(l, m) end
            local function outer(l, m) inner(l, m) end
            local out = {}
            for _, l in ipairs({0, 1, 2, -1, 1000}) do
              local ok, e = pcall(outer, l, "lvl")
              out[#out + 1] = tostring(e)
            end
            local ok, e = pcall(outer, 1, nil)
            out[#out + 1] = type(e)
            error(table.concat(out, "|"), 0)""";
        NativeMachine.Outcome o = host.create(bios).resume(null);
        expectEq("host error levels 0, 1, 2, -1, 1000 and a nil message",
            "lvl|bios.lua:1: lvl|bios.lua:2: lvl|lvl|lvl|nil", firstLine(o));
    }

    private static void testLatin1() throws Exception {
        byte[] all = new byte[256];
        for (int i = 0; i < 256; i++) all[i] = (byte) i;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 256; i++) sb.append((char) i);
        String allChars = sb.toString();
        AtomicReference<Object[]> took = new AtomicReference<>(), took2 = new AtomicReference<>();
        FakeHost host = new FakeHost("t")
            .fn("bytes", req -> Wire.values(new Object[]{all}, NO_UNKNOWN))
            .fn("take", req -> {
                took.set(Wire.decodeArgs(req));
                return NONE;
            })
            .fn("wide", req -> Wire.values(new Object[]{"\u00e9\u20ac\ud83d\ude00"}, NO_UNKNOWN))
            .fn("take2", req -> {
                took2.set(Wire.decodeArgs(req));
                return NONE;
            })
            .fn("echo", req -> Wire.values(Wire.decodeArgs(req), NO_UNKNOWN));
        String bios = """
            local s = t.bytes()
            t.take(s, #s, s:byte(1), s:byte(256), t.echo(s) == s)
            local w = t.wide()
            t.take2(#w, w:byte(1, -1))
            error("done", 0)""";
        NativeMachine.Outcome o = host.create(bios).resume(null);
        check("Latin-1: bytes 0-255 from Java to Lua and back", firstLine(o).equals("done")
            && Arrays.equals(new Object[]{allChars, 256.0, 0.0, 255.0, true}, took.get()), show(took.get()));
        check("Latin-1: a char >= 256 and a surrogate pair reach Lua as '?'", Arrays.equals(
            new Object[]{4.0, 233.0, 63.0, 63.0, 63.0}, took2.get()), show(took2.get()));
    }

    private static void testLongMessages() throws Exception {
        FakeHost host = new FakeHost("t").fn("n", req -> NONE);
        NativeMachine.Outcome o = host.create("error(string.rep(\"x\", 70000), 0)").resume(null);
        String s = o.message() == null ? "" : o.message().text();
        check("a 70,000-byte error value is cut to 64 KiB, without a traceback", s.length() == MSG_MAX
            && s.chars().allMatch(c -> c == 'x') && o.message().tracebackAt() == -1,
            s.length() + " chars, tracebackAt " + (o.message() == null ? "-" : o.message().tracebackAt()));
        o = host.create("error(string.rep(\"x\", 60000), 0)").resume(null);
        s = o.message() == null ? "" : o.message().text();
        check("a 60,000-byte error value keeps its traceback after it", o.message() != null
            && o.message().tracebackAt() == 60000 && s.startsWith("\nstack traceback:", 60000),
            s.length() + " chars, tracebackAt " + (o.message() == null ? "-" : o.message().tracebackAt()));
    }

    private static void testHeapCap() throws Exception {
        FakeHost host = new FakeHost("t").fn("echo", req -> Wire.values(Wire.decodeArgs(req), NO_UNKNOWN));
        NativeMachine.Outcome o = host.create("""
            local ok, e = pcall(string.rep, "x", 100 * 1024 * 1024)
            error(tostring(ok) .. " " .. tostring(e) .. " " .. t.echo("alive"), 0)""").resume(null);
        expectEq("a 100 MiB string.rep hits the 64 MiB cap; the machine goes on", "false not enough memory alive",
            firstLine(o));
    }

    // ---- 2 and 3. Host failures -----------------------------------------------------------------------------------

    /** printed: the text that ExceptionDescribe must print once per failing call, or null for nothing printed. */
    private record Failure(String name, Fn fn, String text, String printed) {
    }

    private static void testHostFailures() throws Exception {
        List<Failure> cases = List.of(
            new Failure("host throws RuntimeException", req -> {
                throw new RuntimeException("BridgeProbe: fake host failure (expected)");
            }, JAVA_EXCEPTION, "java.lang.RuntimeException: BridgeProbe: fake host failure (expected)"),
            new Failure("host throws new OutOfMemoryError(\"fake\")", req -> {
                throw new OutOfMemoryError("fake");
            }, JAVA_EXCEPTION, "java.lang.OutOfMemoryError: fake"),
            new Failure("host returns null", req -> null, "CCLuaJIT: host call returned null", null),
            new Failure("host returns 16 MiB + 1 byte", req -> new byte[WIRE_MAX + 1],
                "CCLuaJIT: host reply too large", null));
        System.out.println("note: the RuntimeException and OutOfMemoryError stack traces on stderr are expected: the "
            + "native side prints them (ExceptionDescribe)");
        for (Failure c : cases) {
            AtomicInteger after = new AtomicInteger();
            FakeHost host = new FakeHost("t").fn("f", c.fn()).fn("after", req -> {
                after.incrementAndGet();
                return NONE;
            });
            int live = NativeMachine.live();
            StringBuilder err = new StringBuilder();
            System.out.flush();
            // A pending exception at the return of nResume would be thrown here, out of resume().
            NativeMachine.Outcome o = captureErr(err, true, () -> host.create("t.f()").resume(null));
            check(c.name() + ": uncaught, the fixed level-0 error", o.status() == NativeMachine.ST_ERROR
                && firstLine(o).equals(c.text()) && o.message().tracebackAt() == c.text().length()
                && NativeMachine.live() == live, text(o));
            o = captureErr(err, true, () -> host.create("local ok, e = pcall(t.f)\nt.after()\n"
                + "error(tostring(ok) .. \"|\" .. tostring(e), 0)").resume(null));
            check(c.name() + ": under pcall, the machine goes on", firstLine(o).equals("false|" + c.text())
                && after.get() == 1, text(o) + ", after() called " + after.get() + " time(s)");
            if (c.printed() != null) {
                int n = count(err.toString(), c.printed());
                check(c.name() + ": printed to System.err once per call (ExceptionDescribe)", n == 2,
                    n + " of 2 times, " + err.length() + " chars on System.err");
            } else {
                check(c.name() + ": nothing printed to System.err", err.length() == 0, "\"" + esc(err.toString())
                    + "\"");
            }
        }
    }

    /** Holds the Java heap full during the JNI-failure check. */
    private static List<byte[]> hog;

    /** Fills the Java heap with 64 KiB arrays until an OutOfMemoryError, then lets 32 of them (2 MiB) go. */
    private static void fillHeap() {
        List<byte[]> h = new ArrayList<>();
        hog = h;
        try {
            while (true) h.add(new byte[64 << 10]);
        } catch (OutOfMemoryError e) {
            for (int i = 0; i < 32 && !h.isEmpty(); i++) h.remove(h.size() - 1);
        }
    }

    /**
     * CM_HOST_E_JNI: NewByteArray for an 8 MiB request fails, because the host has filled the Java heap. 2 MiB stay
     * free for the small arrays that the rest of the resume needs (nMessage, Wire.message), which runs with the heap
     * still full. This depends on the collector. It holds for the default G1 under -Xmx256m (1 MiB regions), G1 with
     * 4 MiB regions, Serial, Parallel, ZGC and Shenandoah. It does not hold for G1 with regions of 8 MiB or more: no
     * region is free after the failure, and Wire.message throws OutOfMemoryError. Under generational ZGC, no Java
     * allocation may happen between fillHeap and the 8 MiB request. jni_host_call must clear the OutOfMemoryError:
     * Lua gets the fixed error, and nothing is pending when nResume returns.
     */
    private static void testJniFailure() throws Exception {
        long max = Runtime.getRuntime().maxMemory();
        if (max > (1L << 30)) {
            check("JNI failure", false, "needs a small heap, as with -Xmx256m (the max heap is " + (max >> 20)
                + " MiB)");
            return;
        }
        String text = "CCLuaJIT: JNI failure in host call";
        FakeHost host = new FakeHost("t")
            .fn("fill", req -> {
                fillHeap();
                return NONE;
            })
            .fn("big", req -> Wire.error("BridgeProbe: the 8 MiB request reached the host (the heap was not full)", 0))
            .fn("release", req -> {
                hog = null;
                return NONE;
            });
        String fill = "local s = string.rep(\"x\", 8 * 1024 * 1024)\nt.fill()\n";
        NativeMachine.Outcome o;
        int live = NativeMachine.live();
        try {
            o = host.create(fill + "t.big(s)").resume(null);
        } finally {
            hog = null;
        }
        check("JNI failure: a request the full Java heap cannot hold gives the fixed level-0 error, uncaught",
            o.status() == NativeMachine.ST_ERROR && firstLine(o).equals(text)
                && o.message().tracebackAt() == text.length() && NativeMachine.live() == live,
            "status " + o.status() + ", " + text(o));
        try {
            o = host.create(fill + "local ok, e = pcall(t.big, s)\nt.release()\n"
                + "error(tostring(ok) .. \"|\" .. tostring(e), 0)").resume(null);
        } finally {
            hog = null;
        }
        check("JNI failure: under pcall, the machine goes on", o.status() == NativeMachine.ST_ERROR
            && firstLine(o).equals("false|" + text), "status " + o.status() + ", " + text(o));
    }

    // ---- 4. Random replies ----------------------------------------------------------------------------------------

    /** What the native side must make of a reply: kind 0 VALUES, 1 ERROR, 2 corrupt. */
    private record Expect(int kind, Object[] values, int level, String message) {
    }

    private static final Expect CORRUPT_REPLY = new Expect(2, null, 0, null);
    private static final double[] SPECIAL_DOUBLES = {0.0, -0.0, Double.NaN, Double.POSITIVE_INFINITY,
        Double.NEGATIVE_INFINITY, Double.MAX_VALUE, Double.MIN_VALUE, 9007199254740992.0, -1.5, 1e100};

    private static void testRandomReplies() throws Exception {
        Random rnd = new Random(20261109L);
        List<byte[]> replies = new ArrayList<>();
        // Fixed cases first: the "try to break" list of M1.md section 3.17 and the other malformed shapes.
        replies.add(new byte[0]);                                                // no kind
        replies.add(bytes(0));                                                   // no count
        replies.add(bytes(0, 0xff, 0xff, 0xff, 0xff));                           // count 0xFFFFFFFF
        replies.add(bytes(0, 1, 0, 0, 0, 4, 10, 0, 0, 0, 'a'));                  // string past the end
        replies.add(bytes(0, 1, 0, 0, 0, 4, 0xff, 0xff, 0xff, 0xff));           // string length 0xFFFFFFFF
        replies.add(bytes(0, 1, 0, 0, 0, 5));                                    // reserved tag 05
        replies.add(bytes(0, 1, 0, 0, 0, 0x0f));                                 // reserved tag 0F
        replies.add(bytes(0, 1, 0, 0, 0, 3, 0, 0, 0));                           // short number
        replies.add(bytes(0, 0, 0, 0, 0, 0));                                    // a byte left over
        replies.add(bytes(2, 0, 0, 0, 0));                                       // reserved kind 02 (S5)
        replies.add(bytes(1, 1, 0, 0, 0, 2));                                    // ERROR with TRUE
        replies.add(bytes(1, 1, 0, 0, 0));                                       // ERROR without a value
        replies.add(Wire.error("level 1000", 1000));                             // valid
        replies.add(Wire.error("lowest level", Integer.MIN_VALUE));              // valid
        replies.add(Wire.error(null, 1));                                        // valid
        replies.add(Wire.values(new Object[]{null, null, null}, NO_UNKNOWN));    // valid, trailing nils
        replies.add(bytes(0, 0, 0, 0, 0));                                       // valid, no values
        int fixed = replies.size();
        for (int i = 0; i < 10_000; i++) replies.add(randomReply(rnd));

        AtomicInteger next = new AtomicInteger(), reported = new AtomicInteger(), mismatches = new AtomicInteger();
        AtomicReference<Expect> current = new AtomicReference<>();
        int[] kinds = new int[3];
        List<String> details = new ArrayList<>();
        FakeHost host = new FakeHost("t")
            .fn("r", req -> {
                byte[] reply = replies.get(next.getAndIncrement());
                Expect e = classify(reply);
                kinds[e.kind()]++;
                current.set(e);
                return reply;
            })
            .fn("report", req -> {
                int i = reported.getAndIncrement();
                Object[] got = Wire.decodeArgs(req);
                Expect e = current.getAndSet(null);
                if (e == null || !matches(e, got)) {
                    if (mismatches.getAndIncrement() < 5) {
                        details.add("reply " + i + " " + hex(replies.get(i)) + ": expected " + describe(e) + ", got "
                            + show(got));
                    }
                }
                return NONE;
            });
        String bios = "local r, report = t.r, t.report\nfor i = 1, " + replies.size()
            + " do report(pcall(r)) end\nerror(\"done\", 0)";
        NativeMachine.Outcome o = host.create(bios).resume(null);
        check("random replies: " + fixed + " fixed and 10,000 seeded random replies, as the Java model predicts",
            firstLine(o).equals("done") && reported.get() == replies.size() && mismatches.get() == 0,
            reported.get() + " reported, " + kinds[0] + " values, " + kinds[1] + " errors, " + kinds[2]
                + " corrupt, " + mismatches.get() + " mismatches" + (details.isEmpty() ? "" : "; " + details)
                + "; machine: " + text(o));
    }

    private static byte[] randomReply(Random rnd) {
        int k = rnd.nextInt(10);
        if (k < 2) {
            byte[] b = new byte[rnd.nextInt(24)];
            rnd.nextBytes(b);
            if (b.length > 0 && rnd.nextBoolean()) b[0] = (byte) rnd.nextInt(3);
            return b;
        }
        byte[] valid = k == 4 || k == 9 ? randomError(rnd) : randomValues(rnd);
        if (k < 5) return valid;
        int mutations = 1 + rnd.nextInt(3);
        for (int m = 0; m < mutations; m++) valid = mutate(valid, rnd);
        return valid;
    }

    private static byte[] randomValues(Random rnd) {
        Object[] v = new Object[rnd.nextInt(7)];
        for (int i = 0; i < v.length; i++) v[i] = randomScalar(rnd);
        return Wire.values(v, NO_UNKNOWN);
    }

    private static byte[] randomError(Random rnd) {
        int[] levels = {-1, 0, 1, 2, 3, 1000, Integer.MIN_VALUE, Integer.MAX_VALUE};
        int level = rnd.nextInt(4) == 0 ? rnd.nextInt() : levels[rnd.nextInt(levels.length)];
        return Wire.error(rnd.nextInt(4) == 0 ? null : randomLatin1(rnd), level);
    }

    private static Object randomScalar(Random rnd) {
        return switch (rnd.nextInt(6)) {
            case 0 -> null;
            case 1 -> rnd.nextBoolean();
            case 2 -> SPECIAL_DOUBLES[rnd.nextInt(SPECIAL_DOUBLES.length)];
            case 3 -> Double.longBitsToDouble(rnd.nextLong());
            default -> randomLatin1(rnd);
        };
    }

    private static String randomLatin1(Random rnd) {
        char[] c = new char[rnd.nextInt(13)];
        for (int i = 0; i < c.length; i++) c[i] = (char) rnd.nextInt(256);
        return new String(c);
    }

    private static byte[] mutate(byte[] b, Random rnd) {
        switch (rnd.nextInt(6)) {
            case 0: {   // flip a bit
                if (b.length == 0) return b;
                byte[] c = b.clone();
                c[rnd.nextInt(c.length)] ^= (byte) (1 << rnd.nextInt(8));
                return c;
            }
            case 1: {   // set a byte
                if (b.length == 0) return b;
                byte[] c = b.clone();
                c[rnd.nextInt(c.length)] = (byte) rnd.nextInt(256);
                return c;
            }
            case 2:     // cut
                return Arrays.copyOf(b, rnd.nextInt(b.length + 1));
            case 3: {   // append 1-3 bytes
                byte[] c = Arrays.copyOf(b, b.length + 1 + rnd.nextInt(3));
                for (int i = b.length; i < c.length; i++) c[i] = (byte) rnd.nextInt(256);
                return c;
            }
            case 4: {   // a large or off-by-one u32 somewhere
                if (b.length < 4) return b;
                byte[] c = b.clone();
                int at = rnd.nextInt(c.length - 3);
                int v = switch (rnd.nextInt(4)) {
                    case 0 -> -1;
                    case 1 -> 0x7fffffff;
                    case 2 -> ByteBuffer.wrap(c, at, 4).order(ByteOrder.LITTLE_ENDIAN).getInt() + 1;
                    default -> rnd.nextInt(8);
                };
                ByteBuffer.wrap(c, at, 4).order(ByteOrder.LITTLE_ENDIAN).putInt(v);
                return c;
            }
            default: {  // insert a byte
                int at = rnd.nextInt(b.length + 1);
                byte[] c = new byte[b.length + 1];
                System.arraycopy(b, 0, c, 0, at);
                c[at] = (byte) rnd.nextInt(256);
                System.arraycopy(b, at, c, at + 1, b.length - at);
                return c;
            }
        }
    }

    /** A Java model of wire_check_reply (machine.c): VALUES with these values, ERROR, or corrupt. */
    private static Expect classify(byte[] r) {
        if (r.length < 1) return CORRUPT_REPLY;
        int kind = r[0] & 0xff;
        if (kind == 0) {
            try {
                return new Expect(0, Wire.decodeArgs(Arrays.copyOfRange(r, 1, r.length)), 0, null);
            } catch (IllegalStateException e) {
                return CORRUPT_REPLY;
            }
        }
        if (kind != 1 || r.length < 6) return CORRUPT_REPLY;
        ByteBuffer b = ByteBuffer.wrap(r).order(ByteOrder.LITTLE_ENDIAN);
        int level = b.getInt(1);
        int tag = r[5] & 0xff;
        if (tag == 0) return r.length == 6 ? new Expect(1, null, level, null) : CORRUPT_REPLY;
        if (tag != 4 || r.length < 10) return CORRUPT_REPLY;
        long n = Integer.toUnsignedLong(b.getInt(6));
        if (n != r.length - 10) return CORRUPT_REPLY;
        return new Expect(1, null, level, new String(r, 10, (int) n, StandardCharsets.ISO_8859_1));
    }

    /**
     * Whether report(pcall(r)) got what the reply should give. An ERROR message with a level gets the position of
     * that level: level 1 is pcall itself, a C frame without one; level 2 is the main chunk at line 2; there is no
     * level 3 in the bios coroutine.
     */
    private static boolean matches(Expect e, Object[] got) {
        if (e.kind() == 0) {
            if (got.length != 1 + e.values().length || !Boolean.TRUE.equals(got[0])) return false;
            for (int i = 0; i < e.values().length; i++) if (!same(e.values()[i], got[i + 1])) return false;
            return true;
        }
        if (got.length != 2 || !Boolean.FALSE.equals(got[0])) return false;
        if (e.kind() == 2) return CORRUPT.equals(got[1]);
        if (e.message() == null) return got[1] == null;
        return (e.level() == 2 ? "bios.lua:2: " + e.message() : e.message()).equals(got[1]);
    }

    private static String describe(Expect e) {
        if (e == null) return "no reply";
        return switch (e.kind()) {
            case 0 -> "values " + show(e.values());
            case 1 -> "error level " + e.level() + " " + (e.message() == null ? "nil" : "\"" + esc(e.message()) + "\"");
            default -> "corrupt";
        };
    }

    private static boolean same(Object a, Object b) {
        if (a instanceof Double x && b instanceof Double y) {
            return Double.doubleToLongBits(x) == Double.doubleToLongBits(y);   // NaN == NaN; 0.0 != -0.0
        }
        return Objects.equals(a, b);
    }

    // ---- 5. Re-entry --------------------------------------------------------------------------------------------

    private static void testReentry() throws Exception {
        AtomicReference<NativeMachine> self = new AtomicReference<>();
        AtomicInteger liveInside = new AtomicInteger(-1);
        FakeHost host = new FakeHost("t")
            .fn("reenter", req -> {
                try {
                    self.get().resume(null);
                    return Wire.error("BridgeProbe: the re-entrant resume did not throw", 0);
                } catch (IllegalStateException e) {
                    return Wire.error("Java Exception Thrown: " + e, 0);   // what LuaJITMachine.call does
                }
            })
            .fn("closeme", req -> {
                self.get().close();
                liveInside.set(NativeMachine.live());
                return NONE;
            })
            .fn("n", req -> NONE);
        int live = NativeMachine.live();
        NativeMachine vm = host.create("t.reenter()");
        self.set(vm);
        NativeMachine.Outcome o = vm.resume(null);
        expectEq("re-entrant resume from the host: IllegalStateException, as a Lua error",
            "Java Exception Thrown: java.lang.IllegalStateException: Machine is already running", firstLine(o));

        vm = host.create("t.closeme()\nt.n()\ncoroutine.yield()");
        self.set(vm);
        o = vm.resume(null);
        check("close() inside an upcall: deferred to the end of the resume", o.status() == NativeMachine.ST_YIELD
            && o.closedWhileRunning() && liveInside.get() == live + 1 && NativeMachine.live() == live
            && vm.stateName().equals("closed"), "status " + o.status() + ", closedWhileRunning "
            + o.closedWhileRunning() + ", live inside " + liveInside.get() + ", after " + NativeMachine.live());
    }

    // ---- 6 and 7. Threads ---------------------------------------------------------------------------------------

    private static void testCloseWhileRunning() throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        FakeHost host = new FakeHost("t").fn("block", req -> {
            entered.countDown();
            return release.await(WAIT_S, TimeUnit.SECONDS) ? NONE : Wire.error("BridgeProbe: latch timeout", 0);
        });
        int live = NativeMachine.live();
        NativeMachine vm = host.create("t.block()\ncoroutine.yield()");
        AtomicReference<NativeMachine.Outcome> outcome = new AtomicReference<>();
        AtomicReference<Throwable> error = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                outcome.set(vm.resume(null));
            } catch (Throwable t) {
                error.set(t);
            }
        }, "bridge-worker");
        worker.start();
        if (!entered.await(WAIT_S, TimeUnit.SECONDS)) throw new IllegalStateException("block() was not called");
        onThread("bridge-closer", () -> {
            vm.close();
            return null;
        });
        check("close() from another thread while the host blocks: the free is deferred",
            NativeMachine.live() == live + 1 && vm.stateName().equals("closed"),
            "live " + NativeMachine.live() + " (" + (live + 1) + " expected), " + vm.stateName());
        expectThrows("close() while running: a second resume throws", IllegalStateException.class,
            "Machine has been closed", () -> vm.resume(null));
        release.countDown();
        worker.join(TimeUnit.SECONDS.toMillis(WAIT_S));
        NativeMachine.Outcome o = outcome.get();
        check("close() while running: the resume returns, closedWhileRunning, freed by the worker", !worker.isAlive()
            && error.get() == null && o != null && o.closedWhileRunning() && o.status() == NativeMachine.ST_YIELD
            && NativeMachine.live() == live, (o == null ? "no outcome" : "status " + o.status()
            + ", closedWhileRunning " + o.closedWhileRunning()) + ", error " + error.get() + ", live "
            + NativeMachine.live());
        check("close() while running: LIVE == 0 at the end", NativeMachine.live() == 0, "live " + NativeMachine.live());
    }

    private static void testThreads() throws Exception {
        List<String> seen = new ArrayList<>();
        FakeHost host = new FakeHost("t").fn("who", req -> {
            synchronized (seen) {
                seen.add(Thread.currentThread().getName());
            }
            return NONE;
        });
        int live = NativeMachine.live();
        NativeMachine vm = onThread("bridge-A", () -> host.create("t.who()\ncoroutine.yield()\nt.who()\nerror(\"x\")"));
        NativeMachine.Outcome o = onThread("bridge-B", () -> vm.resume(null));
        boolean yielded = o.status() == NativeMachine.ST_YIELD && NativeMachine.live() == live + 1;
        onThread("bridge-C", () -> {
            vm.close();
            return null;
        });
        check("create on thread A, resume on B, close on C", yielded && seen.equals(List.of("bridge-B"))
            && NativeMachine.live() == live && vm.stateName().equals("closed"),
            "resume status " + o.status() + ", host calls on " + seen + ", live " + NativeMachine.live());

        seen.clear();
        NativeMachine vm2 = onThread("bridge-A",
            () -> host.create("t.who()\ncoroutine.yield()\nt.who()\nerror(\"x\")"));
        onThread("bridge-B", () -> vm2.resume(null));
        o = onThread("bridge-C", () -> vm2.resume(null));
        check("create on A, resume on B, resume to the error (and the free) on C", o.status() == NativeMachine.ST_ERROR
            && seen.equals(List.of("bridge-B", "bridge-C")) && NativeMachine.live() == live,
            "status " + o.status() + ", host calls on " + seen + ", live " + NativeMachine.live());
    }

    /**
     * close() and resume start together on two threads. close() first spins for a seeded random 0-150 us, so it
     * comes before, during or after the resume (which makes 200 host calls before it yields). When the JVM sees one
     * CPU, the two threads never run at the same time: close() lands during a resume only when the scheduler
     * preempts the resume, which may never happen in 300 rounds. There a round during the resume is not required.
     */
    private static void testCloseRace() throws Exception {
        FakeHost host = new FakeHost("t").fn("n", req -> NONE);
        Random rnd = new Random(7);
        int live = NativeMachine.live(), rounds = 300;
        boolean oneCpu = Runtime.getRuntime().availableProcessors() == 1;
        int[] before = new int[1], during = new int[1], after = new int[1], bad = new int[1];
        List<String> details = new ArrayList<>();
        for (int i = 0; i < rounds; i++) {
            NativeMachine vm = host.create("for i = 1, 200 do t.n() end\ncoroutine.yield()\nt.n()");
            long spinNs = rnd.nextInt(150_000);
            CyclicBarrier barrier = new CyclicBarrier(2);
            AtomicReference<Object> result = new AtomicReference<>();
            Thread a = new Thread(() -> {
                try {
                    barrier.await(WAIT_S, TimeUnit.SECONDS);
                    result.set(vm.resume(null));
                } catch (Throwable t) {
                    result.set(t);
                }
            }, "bridge-race-resume");
            Thread b = new Thread(() -> {
                try {
                    barrier.await(WAIT_S, TimeUnit.SECONDS);
                    long until = System.nanoTime() + spinNs;
                    while (System.nanoTime() < until) Thread.onSpinWait();
                    vm.close();
                } catch (Throwable t) {
                    synchronized (details) {
                        details.add("close: " + t);
                    }
                }
            }, "bridge-race-close");
            a.start();
            b.start();
            a.join(TimeUnit.SECONDS.toMillis(WAIT_S));
            b.join(TimeUnit.SECONDS.toMillis(WAIT_S));
            Object r = result.get();
            if (r instanceof NativeMachine.Outcome o && o.status() == NativeMachine.ST_YIELD) {
                if (o.closedWhileRunning()) during[0]++;
                else after[0]++;
            } else if (r instanceof IllegalStateException e && "Machine has been closed".equals(e.getMessage())) {
                before[0]++;
            } else {
                bad[0]++;
                if (details.size() < 5) details.add("round " + i + ": " + r);
            }
            vm.close();
            if (NativeMachine.live() != live) {
                bad[0]++;
                if (details.size() < 5) details.add("round " + i + ": live " + NativeMachine.live());
            }
        }
        // Without a round during the resume, and one outside it, the check would say nothing about the race.
        check("close() racing resume, " + rounds + " rounds: one free each, no leak; some during the resume"
            + (oneCpu ? " (not required: one CPU)" : "") + ", some outside it", bad[0] == 0 && details.isEmpty()
            && NativeMachine.leaked() == 0 && (during[0] > 0 || oneCpu) && before[0] + after[0] > 0,
            "close() came before the resume " + before[0] + " times, during it "
            + during[0] + " (closedWhileRunning), after it " + after[0] + (details.isEmpty() ? "" : "; " + details));
    }

    // ---- 8. Local references --------------------------------------------------------------------------------------

    private static void testLocalRefs() throws Exception {
        final int n = 100_000;
        long[] heap = new long[2];
        AtomicInteger count = new AtomicInteger();
        AtomicReference<String> bad = new AtomicReference<>();
        FakeHost host = new FakeHost("t").fn("leak", req -> {
            int c = count.incrementAndGet();
            if (req.length != 4 + 1 + 4 + 4096) bad.compareAndSet(null, "call " + c + ": a request of " + req.length
                + " bytes");
            if (c == n) heap[1] = usedHeapAfterGc();
            return Wire.values(new Object[]{new byte[4096]}, NO_UNKNOWN);
        });
        NativeMachine vm = host.create("local s = string.rep(\"x\", 4096)\nlocal f = t.leak\nfor i = 1, " + n
            + " do f(s) end\nerror(\"done\", 0)");
        heap[0] = usedHeapAfterGc();
        long t0 = System.nanoTime();
        NativeMachine.Outcome o = vm.resume(null);
        long ms = (System.nanoTime() - t0) / 1_000_000;
        long limit = 32L << 20;
        check("local references: " + n + " upcalls of 4 KiB each way in one resume; heap below baseline + 32 MiB",
            firstLine(o).equals("done") && count.get() == n && bad.get() == null && heap[1] > 0
                && heap[1] < heap[0] + limit,
            String.format("baseline %.1f MiB, in the last upcall after System.gc() %.1f MiB (%+.1f MiB), %d calls in %d"
                    + " ms%s; machine: %s", heap[0] / 1048576.0, heap[1] / 1048576.0, (heap[1] - heap[0]) / 1048576.0,
                count.get(), ms, bad.get() == null ? "" : ", " + bad.get(), text(o)));
    }

    /** A host failure without a stack trace: ExceptionDescribe prints one short line for it. */
    private static final class Quiet extends RuntimeException {
        Quiet(String message) {
            super(message, null, false, false);
        }
    }

    /**
     * Item 8 for the failing exits of jni_host_call (E_NULL, E_JAVA, E_TOOBIG): each must pop its local frame too.
     * One resume per exit with 128 failing upcalls under pcall, each with a 448 KiB request: a frame that is not
     * popped keeps its request array, 56 MiB in all. (448 KiB stays below G1's humongous size, half of a 1 MiB
     * region.) Inside a last upcall, after {@link System#gc()}, the used heap must be below the baseline + 16 MiB.
     */
    private static void testLocalRefsOnFailures() throws Exception {
        final int n = 128, size = 448 << 10;
        String quietText = "BridgeProbe: fake host failure without a stack trace (expected)";
        byte[] huge = new byte[WIRE_MAX + 1];
        List<Failure> cases = List.of(
            new Failure("null", req -> null, "CCLuaJIT: host call returned null", null),
            new Failure("exception", req -> {
                throw new Quiet(quietText);
            }, JAVA_EXCEPTION, Quiet.class.getName() + ": " + quietText),
            new Failure("16 MiB + 1 byte", req -> huge, "CCLuaJIT: host reply too large", null));
        System.out.println("note: for the " + n + " host exceptions below, ExceptionDescribe writes \"Exception in "
            + "thread ...\" to stderr each time (one long line, expected); their text is captured and counted");
        for (Failure c : cases) {
            long[] heap = new long[2];
            AtomicInteger calls = new AtomicInteger();
            AtomicReference<String> bad = new AtomicReference<>();
            FakeHost host = new FakeHost("t")
                .fn("f", req -> {
                    if (calls.incrementAndGet() == 1 && req.length != 4 + 1 + 4 + size) {
                        bad.set("a request of " + req.length + " bytes");
                    }
                    return c.fn().call(req);
                })
                .fn("measure", req -> {
                    heap[1] = usedHeapAfterGc();
                    return NONE;
                });
            NativeMachine vm = host.create("local s = string.rep(\"x\", " + size + ")\nlocal f = t.f\nfor i = 1, " + n
                + " do\n  local ok, e = pcall(f, s)\n  if ok or e ~= \"" + c.text() + "\" then error(\"call \" .. i .. "
                + "\": \" .. tostring(e), 0) end\nend\nt.measure()\nerror(\"done\", 0)");
            heap[0] = usedHeapAfterGc();
            StringBuilder err = new StringBuilder();
            System.out.flush();
            NativeMachine.Outcome o = captureErr(err, false, () -> vm.resume(null));
            if (c.printed() != null) System.err.println();   // ends the line of "Exception in thread" prefixes
            int printed = c.printed() == null ? 0 : count(err.toString(), c.printed());
            String name = "local references, " + c.name() + " exit: " + n + " failing upcalls of " + (size >> 10)
                + " KiB; heap below baseline + 16 MiB; " + (c.printed() == null ? "nothing printed"
                : "each exception printed once");
            check(name, firstLine(o).equals("done") && calls.get() == n && bad.get() == null
                && heap[1] > 0 && heap[1] < heap[0] + (16L << 20) && printed == (c.printed() == null ? 0 : n)
                && (c.printed() != null || err.length() == 0),
                String.format("baseline %.1f MiB, in the last upcall after System.gc() %.1f MiB (%+.1f MiB), %d calls, "
                    + "%d printed to System.err%s; machine: %s", heap[0] / 1048576.0, heap[1] / 1048576.0,
                    (heap[1] - heap[0]) / 1048576.0, calls.get(), printed, bad.get() == null ? "" : ", " + bad.get(),
                    text(o)));
        }
    }

    private static long usedHeapAfterGc() {
        Runtime rt = Runtime.getRuntime();
        for (int i = 0; i < 3; i++) System.gc();
        return rt.totalMemory() - rt.freeMemory();
    }

    // ---- 9. Time per call -----------------------------------------------------------------------------------------

    private static void testNsPerCall(boolean checkJni) throws Exception {
        for (int i = 0; i < 2; i++) echoRun(50_000);   // warm-up: compile the Java and Lua paths
        int n = 200_000, runs = 5;
        double[] ns = new double[runs];
        for (int i = 0; i < runs; i++) ns[i] = echoRun(n);
        double[] sorted = ns.clone();
        Arrays.sort(sorted);
        StringBuilder all = new StringBuilder();
        for (double v : ns) all.append(all.length() == 0 ? "" : " ").append(String.format("%.1f", v));
        boolean ok = Arrays.stream(ns).allMatch(v -> v > 0);
        check("ns per call measured", ok, runs + " runs of " + n + " calls");
        System.out.printf("ns per call: %.1f (median of %d runs of %d calls of t.echo(\"hello\"), a string in and a "
            + "string out, decoded and encoded by Wire; -Xcheck:jni %s; runs: %s)%n", sorted[runs / 2], runs, n,
            checkJni ? "on" : "off", all);
    }

    /** One machine that calls t.echo("hello") n times; the ns per call between the first and the last upcall. */
    private static double echoRun(int n) throws Exception {
        AtomicLong first = new AtomicLong(), last = new AtomicLong();
        AtomicInteger count = new AtomicInteger();
        FakeHost host = new FakeHost("t").fn("echo", req -> {
            long now = System.nanoTime();
            if (count.getAndIncrement() == 0) first.set(now);
            last.set(now);
            return Wire.values(Wire.decodeArgs(req), NO_UNKNOWN);
        });
        NativeMachine.Outcome o = host.create("local f = t.echo\nfor i = 1, " + n
            + " do f(\"hello\") end\nerror(\"done\", 0)").resume(null);
        if (!firstLine(o).equals("done") || count.get() != n) {
            throw new IllegalStateException("echo run: " + count.get() + " calls, " + text(o));
        }
        return (last.get() - first.get()) / (double) (n - 1);
    }

    // ---- Wire, directly -------------------------------------------------------------------------------------------

    /**
     * NativeLoader's cleanup of other versions' hash directories: it keeps its own and any used within a day, and
     * nothing it meets may throw out of it, an error while listing included (guide finding 5, review R6).
     */
    private static void testLoaderCleanup() throws IOException {
        Path root = Files.createTempDirectory(cache, "cleanup-");
        Path keep = root.resolve("keep"), fresh = root.resolve("fresh"), old = root.resolve("old");
        for (Path d : List.of(keep, fresh, old)) Files.writeString(Files.createDirectories(d).resolve("lib"), "x");
        Instant now = Instant.now();
        FileTime twoDaysAgo = FileTime.from(now.minus(Duration.ofDays(2)));
        Files.setLastModifiedTime(old, twoDaysAgo);
        Files.setLastModifiedTime(keep, twoDaysAgo);
        NativeLoader.cleanupStale(root, keep, Files::isDirectory, now);
        check("NativeLoader: the cleanup deletes a directory unused for two days, and keeps its own and a fresh one",
            !Files.exists(old) && Files.exists(keep.resolve("lib")) && Files.exists(fresh.resolve("lib")),
            "old " + Files.exists(old) + ", keep " + Files.exists(keep) + ", fresh " + Files.exists(fresh));

        // An I/O error of the filter comes out of the iterator as DirectoryIteratorException, as a failing
        // readdir does.
        DirectoryStream.Filter<Path> failing = p -> {
            throw new IOException("injected");
        };
        String direct;
        try (DirectoryStream<Path> s = Files.newDirectoryStream(root, failing)) {
            s.iterator().hasNext();
            direct = "no exception";
        } catch (DirectoryIteratorException e) {
            direct = null;
        }
        String thrown = null;
        try {
            NativeLoader.cleanupStale(root, keep, failing, now);
        } catch (Throwable t) {
            thrown = t.toString();
        }
        check("NativeLoader: the cleanup swallows a DirectoryIteratorException", direct == null && thrown == null,
            direct != null ? "the injected error did not give DirectoryIteratorException: " + direct : thrown);
        for (Path d : List.of(keep, fresh)) {
            Files.deleteIfExists(d.resolve("lib"));
            Files.deleteIfExists(d);
        }
        Files.deleteIfExists(root);
    }

    private static void testWire() {
        // decodeArgs: well-formed input.
        Object[] args = Wire.decodeArgs(bytes(4, 0, 0, 0, 0, 2, 1, 4, 1, 0, 0, 0, 'x'));
        check("Wire.decodeArgs: nil, true, false, a string", Arrays.equals(new Object[]{null, true, false, "x"}, args),
            show(args));
        byte[] num = new byte[4 + 9];
        ByteBuffer.wrap(num).order(ByteOrder.LITTLE_ENDIAN).putInt(1).put((byte) 3).putDouble(-0.5);
        args = Wire.decodeArgs(num);
        check("Wire.decodeArgs: a number is a Double", args.length == 1 && Double.valueOf(-0.5).equals(args[0]),
            show(args));
        check("Wire.decodeArgs: no values", Wire.decodeArgs(bytes(0, 0, 0, 0)).length == 0, "");

        // decodeArgs: malformed input.
        int malformed = 0;
        List<String> notThrown = new ArrayList<>();
        byte[][] bad = {
            new byte[0], bytes(1, 0, 0), bytes(1, 0, 0, 0), bytes(0xff, 0xff, 0xff, 0xff), bytes(2, 0, 0, 0, 0),
            bytes(1, 0, 0, 0, 5), bytes(1, 0, 0, 0, 0x0f), bytes(1, 0, 0, 0, 0xff), bytes(1, 0, 0, 0, 3, 0, 0, 0),
            bytes(1, 0, 0, 0, 4, 5, 0, 0, 0, 'a'), bytes(1, 0, 0, 0, 4, 0xff, 0xff, 0xff, 0xff, 'a'),
            bytes(1, 0, 0, 0, 4, 1, 0, 0), bytes(0, 0, 0, 0, 0), bytes(1, 0, 0, 0, 0, 0)};
        for (byte[] b : bad) {
            try {
                Wire.decodeArgs(b);
                notThrown.add(hex(b) + ": no exception");
            } catch (IllegalStateException e) {
                malformed++;
            } catch (RuntimeException e) {
                notThrown.add(hex(b) + ": " + e);
            }
        }
        try {
            Wire.decodeArgs(null);
            notThrown.add("null: no exception");
        } catch (IllegalStateException e) {
            malformed++;
        }
        check("Wire.decodeArgs: " + (bad.length + 1) + " malformed arrays throw IllegalStateException",
            notThrown.isEmpty(), malformed + " did" + (notThrown.isEmpty() ? "" : "; " + notThrown));

        // Latin-1 round trip of bytes 0-255.
        byte[] req = new byte[4 + 5 + 256];
        ByteBuffer.wrap(req).order(ByteOrder.LITTLE_ENDIAN).putInt(1).put((byte) 4).putInt(256);
        for (int i = 0; i < 256; i++) req[9 + i] = (byte) i;
        String s = (String) Wire.decodeArgs(req)[0];
        boolean chars = s.length() == 256;
        for (int i = 0; chars && i < 256; i++) chars = s.charAt(i) == i;
        byte[] back = Wire.values(new Object[]{s}, NO_UNKNOWN);
        byte[] want = new byte[1 + 4 + 5 + 256];
        want[1] = 1;
        System.arraycopy(req, 4, want, 5, 5 + 256);
        check("Wire: Latin-1 round trip of bytes 0-255", chars && Arrays.equals(want, back), hex(back, 16));

        // Chars of 256 and more.
        check("Wire.values: a char >= 256 is '?'", Arrays.equals(Wire.values(new Object[]{"\u0100"}, NO_UNKNOWN),
            bytes(0, 1, 0, 0, 0, 4, 1, 0, 0, 0, '?')), "");
        check("Wire.values: a surrogate pair is two '?'", Arrays.equals(Wire.values(new Object[]{"a\ud83d\ude00b"},
            NO_UNKNOWN), bytes(0, 1, 0, 0, 0, 4, 4, 0, 0, 0, 'a', '?', '?', 'b')), "");
        check("Wire.error: a char >= 256 is '?'", Arrays.equals(Wire.error("\u20ac", 0),
            bytes(1, 0, 0, 0, 0, 4, 1, 0, 0, 0, '?')), "");

        // values(): the mapping.
        check("Wire.values(null): no values", Arrays.equals(Wire.values(null, NO_UNKNOWN), bytes(0, 0, 0, 0, 0)), "");
        byte[] src = {1, (byte) 0x80, (byte) 0xff};
        byte[] enc = Wire.values(new Object[]{null, true, false, 1, 2.5f, 3L, new BigDecimal("0.5"), "hi", src},
            NO_UNKNOWN);
        src[0] = 9;
        ByteBuffer w = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN);
        w.put((byte) 0).putInt(9).put((byte) 0).put((byte) 2).put((byte) 1);
        w.put((byte) 3).putDouble(1.0).put((byte) 3).putDouble(2.5).put((byte) 3).putDouble(3.0);
        w.put((byte) 3).putDouble(0.5).put((byte) 4).putInt(2).put((byte) 'h').put((byte) 'i');
        w.put((byte) 4).putInt(3).put((byte) 1).put((byte) 0x80).put((byte) 0xff);
        check("Wire.values: null, Boolean, Number (Integer, Float, Long, BigDecimal), String, byte[] (copied)",
            Arrays.equals(Arrays.copyOf(w.array(), w.position()), enc), hex(enc));
        List<String> wrong = new ArrayList<>();
        for (Object o : new Object[]{Map.of(), List.of(1), Set.of(), new Object[0], new Integer[]{1},
            ByteBuffer.allocate(1)}) {
            try {
                Wire.values(new Object[]{o}, NO_UNKNOWN);
                wrong.add(o.getClass().getName() + ": no exception");
            } catch (Wire.UnsupportedValue e) {
                if (!e.getMessage().startsWith("CCLuaJIT: ")) wrong.add(o.getClass().getName() + ": " + e.getMessage());
            } catch (Throwable t) {
                wrong.add(o.getClass().getName() + ": " + t);
            }
        }
        check("Wire.values: Map, Collection, Object[] and ByteBuffer throw UnsupportedValue", wrong.isEmpty(),
            wrong.toString());
        List<Object> unknown = new ArrayList<>();
        Object thing = new Object();
        Object[] odd = {thing, 'c', Optional.empty(), new int[]{1}};
        byte[] nils = Wire.values(odd, unknown::add);
        check("Wire.values: an unknown type calls onUnknown, then becomes nil", Arrays.equals(nils,
            bytes(0, 4, 0, 0, 0, 0, 0, 0, 0)) && unknown.size() == 4 && unknown.get(0) == thing
            && unknown.get(3) == odd[3], hex(nils) + ", onUnknown got " + unknown.size());
        try {
            Wire.values(new Object[]{1, thing}, o -> {
                throw new Wire.UnsupportedValue("refused " + o.getClass().getSimpleName());
            });
            check("Wire.values: onUnknown may throw", false, "no exception");
        } catch (Wire.UnsupportedValue e) {
            check("Wire.values: onUnknown may throw", "refused Object".equals(e.getMessage()), e.getMessage());
        }

        // error().
        check("Wire.error: a nil message", Arrays.equals(Wire.error(null, 1), bytes(1, 1, 0, 0, 0, 0)), "");
        check("Wire.error: a string, level -1", Arrays.equals(Wire.error("m", -1),
            bytes(1, 0xff, 0xff, 0xff, 0xff, 4, 1, 0, 0, 0, 'm')), "");

        // message().
        Wire.Message m = Wire.message(null);
        check("Wire.message(null): \"(no message)\", no traceback", m.text().equals("(no message)")
            && m.tracebackAt() == -1, show(m));
        byte[] g1 = G1_TEXT.getBytes(StandardCharsets.ISO_8859_1);
        byte[] raw = new byte[4 + g1.length];
        ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).putInt(16).put(g1);
        m = Wire.message(raw);
        check("Wire.message: with a traceback", m.text().equals(G1_TEXT) && m.tracebackAt() == 16, show(m));
        m = Wire.message(bytes(0xff, 0xff, 0xff, 0xff, 'a', 0xe9));
        check("Wire.message: without a traceback (0xFFFFFFFF), Latin-1", m.text().equals("a\u00e9")
            && m.tracebackAt() == -1, show(m));
        m = Wire.message(bytes(0xff, 0xff, 0xff, 0xff));
        check("Wire.message: empty", m.text().isEmpty() && m.tracebackAt() == -1, show(m));
        notThrown.clear();
        for (byte[] b : new byte[][]{new byte[0], bytes(1, 2, 3), bytes(2, 0, 0, 0, 'a'), bytes(0xfe, 0xff, 0xff,
            0xff, 'a'), bytes(0, 0, 0, 0x80)}) {
            try {
                Wire.message(b);
                notThrown.add(hex(b));
            } catch (IllegalStateException e) {
                // expected
            }
        }
        check("Wire.message: malformed arrays throw IllegalStateException", notThrown.isEmpty(), notThrown.toString());

        // NativeMachine.ended: what resume and create make of a message (guide finding 4, review R5).
        NativeMachine.Ended en = NativeMachine.ended(NativeMachine.ST_ERROR, raw);
        check("NativeMachine.ended: a well-formed message keeps its status", en.status() == NativeMachine.ST_ERROR
            && en.message().equals(Wire.message(raw)), en.status() + " " + show(en.message()));
        notThrown.clear();
        for (byte[] b : new byte[][]{new byte[0], bytes(1, 2, 3), bytes(2, 0, 0, 0, 'a'), bytes(0, 0, 0, 0x80)}) {
            NativeMachine.Ended e = NativeMachine.ended(NativeMachine.ST_ERROR, b);
            if (e.status() != NativeMachine.ST_INTERNAL || e.message().tracebackAt() != -1
                || !e.message().text().startsWith("CCLuaJIT: malformed native message (wire: ")) {
                notThrown.add(hex(b) + ": " + e.status() + " " + show(e.message()));
            }
        }
        check("NativeMachine.ended: a malformed message gives ST_INTERNAL, not an IllegalStateException",
            notThrown.isEmpty(), notThrown.toString());

        // SpecWriter.
        Wire.SpecWriter sw = new Wire.SpecWriter();
        sw.api(List.of("term"), null).method("write", false);
        sw.api(List.of("a", "b"), "m").method("x", true).method("y", false);
        ByteBuffer sp = ByteBuffer.allocate(128).order(ByteOrder.LITTLE_ENDIAN);
        sp.putInt(2);
        sp.putInt(1).putInt(4).put("term".getBytes(StandardCharsets.ISO_8859_1)).put((byte) 0).putInt(1);
        sp.putInt(5).put("write".getBytes(StandardCharsets.ISO_8859_1)).put((byte) 0);
        sp.putInt(2).putInt(1).put((byte) 'a').putInt(1).put((byte) 'b').put((byte) 1).putInt(1).put((byte) 'm');
        sp.putInt(2).putInt(1).put((byte) 'x').put((byte) 1).putInt(1).put((byte) 'y').put((byte) 0);
        byte[] specBytes = sw.toByteArray();
        check("Wire.SpecWriter: two APIs, a module, the MAY_YIELD flag", Arrays.equals(
            Arrays.copyOf(sp.array(), sp.position()), specBytes) && sw.functions() == 3, hex(specBytes));
        expectThrows("Wire.SpecWriter: a method before any API", IllegalStateException.class, null,
            () -> new Wire.SpecWriter().method("x", false));
        expectThrows("Wire.SpecWriter: a 257-byte name", IllegalArgumentException.class, null,
            () -> new Wire.SpecWriter().api(List.of("a".repeat(257)), null));
    }

    // ---- The fake host ----------------------------------------------------------------------------------------------

    /** One host function: the request in, the reply out. May throw (the host-failure checks do). */
    @FunctionalInterface
    private interface Fn {
        byte[] call(byte[] request) throws Exception;
    }

    /** A host with one API whose methods are {@link Fn}s, numbered in the order they are added. */
    private static final class FakeHost implements NativeMachine.Host {
        private final Wire.SpecWriter spec = new Wire.SpecWriter();
        private final List<Fn> fns = new ArrayList<>();

        FakeHost(String api) {
            spec.api(List.of(api), null);
        }

        FakeHost fn(String name, Fn f) {
            spec.method(name, false);
            fns.add(f);
            return this;
        }

        byte[] spec() {
            return spec.toByteArray();
        }

        NativeMachine create(String bios) throws NativeMachineException {
            return NativeMachine.create(this, MEM, KEYS.nextLong(), spec(), bios.getBytes(StandardCharsets.ISO_8859_1));
        }

        @Override
        public byte[] call(int function, byte[] request) {
            try {
                return fns.get(function).call(request);
            } catch (RuntimeException | Error e) {
                throw e;   // the host-failure checks: the native side must handle it
            } catch (Exception e) {
                return Wire.error("BridgeProbe host: " + e, 0);
            }
        }
    }

    // ---- Helpers --------------------------------------------------------------------------------------------------

    @FunctionalInterface
    private interface Body {
        void run() throws Exception;
    }

    /**
     * Runs one group of checks on a thread of its own and waits for it, at most SECTION_S. JNI local references that
     * a broken jni_machine.c leaks (a local frame it never pops) live until their thread ends: so they cannot fill
     * the heap of the sections after this one, or of the summary. A section that does not finish in time (such a
     * leak makes the garbage collector crawl) fails, keeps running in the background, and the next one starts.
     */
    private static void section(String name, Body body) {
        AtomicReference<Throwable> error = new AtomicReference<>();
        Thread t = new Thread(() -> {
            try {
                body.run();
            } catch (Throwable e) {
                error.set(e);
            }
        }, "bridge-section");
        t.setDaemon(true);
        t.start();
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(SECTION_S);
        while (t.isAlive() && System.nanoTime() < until) {
            try {
                t.join(Math.max(1, TimeUnit.NANOSECONDS.toMillis(until - System.nanoTime())));
            } catch (InterruptedException e) {
                // nothing interrupts the main thread; join again
            }
        }
        if (t.isAlive()) {
            check(name, false, "did not finish in " + SECTION_S + " s; it goes on in the background");
        } else if (error.get() != null) {
            check(name, false, "unexpected " + error.get());
            error.get().printStackTrace(System.out);
        }
        System.out.flush();
    }

    /**
     * Runs body with System.err also written to a buffer, then appends what reached System.err to {@code into}.
     * ExceptionDescribe prints the exception through System.err; only its {@code Exception in thread "..."} prefix
     * goes to the process's stderr directly. forward: pass the output on to the real System.err as well.
     */
    private static <T> T captureErr(StringBuilder into, boolean forward, Callable<T> body) throws Exception {
        PrintStream err = System.err;
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        OutputStream out = !forward ? buf : new OutputStream() {
            @Override
            public void write(int b) {
                buf.write(b);
                err.write(b);
            }

            @Override
            public void write(byte[] b, int off, int len) {
                buf.write(b, off, len);
                err.write(b, off, len);
            }

            @Override
            public void flush() {
                err.flush();
            }
        };
        System.setErr(new PrintStream(out, true, err.charset()));
        try {
            return body.call();
        } finally {
            System.err.flush();
            System.setErr(err);
            into.append(buf.toString(err.charset()));
        }
    }

    /** The number of times part occurs in s, without overlaps. */
    private static int count(String s, String part) {
        int n = 0;
        for (int i = s.indexOf(part); i >= 0; i = s.indexOf(part, i + part.length())) n++;
        return n;
    }

    /** Synchronized: a section that ran out of time may still report from its thread. */
    private static synchronized boolean check(String name, boolean pass, String detail) {
        checks++;
        if (!pass) failed++;
        System.out.println((pass ? "ok   " : "FAIL ") + name + (detail == null || detail.isEmpty() ? "" : ": "
            + detail));
        return pass;
    }

    private static void expectEq(String name, String want, String got) {
        check(name, want.equals(got), "\"" + esc(got) + "\"" + (want.equals(got) ? "" : ", expected \"" + esc(want)
            + "\""));
    }

    private static void expectThrows(String name, Class<? extends Throwable> type, String message, Body body) {
        try {
            body.run();
            check(name, false, "no exception");
        } catch (Throwable t) {
            check(name, type.isInstance(t) && (message == null || message.equals(t.getMessage())), t.toString());
        }
    }

    private static void expectCreateFails(String name, long mem, byte[] spec, String bios, Predicate<String> ok) {
        int live = NativeMachine.live();
        try {
            NativeMachine vm = NativeMachine.create((f, r) -> NONE, mem, 1, spec,
                bios.getBytes(StandardCharsets.ISO_8859_1));
            vm.close();
            check(name, false, "the machine was created");
        } catch (NativeMachineException e) {
            check(name, ok.test(e.getMessage()) && NativeMachine.live() == live, "NativeMachineException(\""
                + esc(e.getMessage()) + "\"), live " + NativeMachine.live());
        }
    }

    private static <T> T onThread(String name, Callable<T> body) throws Exception {
        AtomicReference<T> result = new AtomicReference<>();
        AtomicReference<Throwable> error = new AtomicReference<>();
        Thread t = new Thread(() -> {
            try {
                result.set(body.call());
            } catch (Throwable e) {
                error.set(e);
            }
        }, name);
        t.start();
        t.join(TimeUnit.SECONDS.toMillis(WAIT_S));
        if (t.isAlive()) throw new IllegalStateException("thread " + name + " did not finish in " + WAIT_S + " s");
        if (error.get() != null) throw new IllegalStateException("on thread " + name + ": " + error.get(), error.get());
        return result.get();
    }

    private static void startWatchdog() {
        Thread t = new Thread(() -> {
            try {
                Thread.sleep(TimeUnit.SECONDS.toMillis(WATCHDOG_S));
            } catch (InterruptedException e) {
                return;
            }
            System.out.println("FAIL watchdog: BridgeProbe did not finish in " + WATCHDOG_S + " s");
            System.out.println("bridgeProbe: FAILED");
            System.out.flush();
            Runtime.getRuntime().halt(1);
        }, "bridge-watchdog");
        t.setDaemon(true);
        t.start();
    }

    private static void exit(int code) {
        System.out.flush();
        System.err.flush();
        System.exit(code);
    }

    private static String firstLine(NativeMachine.Outcome o) {
        if (o == null || o.message() == null) return "(no message; status " + (o == null ? "-" : o.status()) + ")";
        String s = o.message().text();
        int nl = s.indexOf('\n');
        return nl < 0 ? s : s.substring(0, nl);
    }

    private static String text(NativeMachine.Outcome o) {
        if (o == null) return "no outcome";
        if (o.message() == null) return "status " + o.status() + ", no message";
        return "\"" + esc(o.message().text()) + "\"";
    }

    private static String show(Object[] values) {
        if (values == null) return "null";
        StringBuilder b = new StringBuilder("[");
        for (Object v : values) {
            if (b.length() > 1) b.append(", ");
            if (v instanceof String s) b.append('"').append(esc(s)).append('"');
            else if (v instanceof Double d && Double.isNaN(d)) b.append("NaN");
            else b.append(v);
        }
        return b.append(']').toString();
    }

    private static String show(Wire.Message m) {
        return "\"" + esc(m.text()) + "\", tracebackAt " + m.tracebackAt();
    }

    /** Printable ASCII, cut to 300 chars. */
    private static String esc(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length() && b.length() < 300; i++) {
            char c = s.charAt(i);
            if (c == '\n') b.append("\\n");
            else if (c == '\t') b.append("\\t");
            else if (c == '\\') b.append("\\\\");
            else if (c < 0x20 || c >= 0x7f) b.append(String.format(c < 256 ? "\\x%02x" : "\\u%04x", (int) c));
            else b.append(c);
        }
        if (b.length() >= 300) b.append("... (").append(s.length()).append(" chars)");
        return b.toString();
    }

    private static String hex(byte[] b) {
        return hex(b, 64);
    }

    private static String hex(byte[] b, int max) {
        if (b == null) return "null";
        String h = HexFormat.of().formatHex(b, 0, Math.min(b.length, max));
        return b.length > max ? h + "... (" + b.length + " bytes)" : h;
    }

    private static byte[] bytes(int... v) {
        byte[] b = new byte[v.length];
        for (int i = 0; i < v.length; i++) b[i] = (byte) v[i];
        return b;
    }
}
