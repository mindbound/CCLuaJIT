package harness;

import dan200.computercraft.api.lua.*;
import gay.vereena.cclj.computer.LuaJITMachine;

import java.util.*;

/**
 * Drives the ORIGINAL CCLuaJIT native glue (origin/1.14.4_CCT main.cpp, unmodified) through JNI.
 * Usage: java -cp out harness.Main <lua51.dll> <cclj.dll> <test>
 * Each test runs in its own JVM because several of them kill the process.
 */
public final class Main {
    static LuaJITMachine newMachine(String bios) {
        LuaJITMachine m = new LuaJITMachine();
        if (!m.initMachine("0.1.2", "1.86.2", "1.14.4", "harness", "", 42L)) throw new RuntimeException("initMachine failed");
        if (!m.loadBios(bios)) throw new RuntimeException("loadBios failed");
        return m;
    }

    static String dump(Object[] r) {
        if (r == null) return "null";
        StringBuilder sb = new StringBuilder("[");
        for (Object o : r) {
            if (o instanceof String s) {
                sb.append("String(len=").append(s.length()).append(", chars=");
                for (char c : s.toCharArray()) sb.append(String.format("U+%04X ", (int) c));
                sb.append(")");
            } else sb.append(o);
            sb.append(", ");
        }
        return sb.append("]").toString();
    }

    static void p(String s) { System.out.println(s); System.out.flush(); }

    // Lua helper: hex-dump a string's bytes
    static final String HEX = "local function hex(s) return (s:gsub('.', function(c) return string.format('%02X ', c:byte()) end)) end\n";

    public static void main(String[] a) throws Exception {
        System.load(a[0]);
        System.load(a[1]);
        String test = a[2];
        p("== test " + test);
        switch (test) {
            case "strings" -> {
                // Java->Lua: String with Latin-1 / CC teletext chars, and byte[] with an embedded NUL.
                var m = newMachine(HEX +
                    "local ev = {coroutine.yield()}\n" +
                    "local s, b = ev[2], ev[3]\n" +
                    "coroutine.yield('J2L string len=' .. #s .. ' bytes=' .. hex(s), 'J2L byte[] len=' .. #b .. ' bytes=' .. hex(b))\n" +
                    "coroutine.yield('\\128\\149\\167\\255')\n");
                p("start -> " + dump(m.handleEvent(new Object[0])));
                Object[] r = m.handleEvent(new Object[]{"ev", "§\u0095A", new byte[]{65, 0, 66}});
                p("J2L -> " + Arrays.toString(r));
                p("  (CC:T/Cobalt expectation: string len=3 bytes=A7 95 41 ; byte[] len=3 bytes=41 00 42)");
                r = m.handleEvent(new Object[]{"ev2"});
                p("L2J yield('\\128\\149\\167\\255') -> " + dump(r));
                p("  (CC:T/Cobalt expectation: U+0080 U+0095 U+00A7 U+00FF)");
                m.deinitMachine();
                p("strings: completed normally");
            }
            case "unknown-java-type" -> {
                // Any Java value the converter does not know (CC:T 1.21.1 events carry handle objects, ByteBuffer, List...)
                var m = newMachine("while true do coroutine.yield() end");
                m.handleEvent(new Object[0]);
                p("calling handleEvent with an ArrayList argument ...");
                m.handleEvent(new Object[]{"http_success", new ArrayList<>(List.of("x"))});
                p("UNREACHED? handleEvent returned normally");
            }
            case "yield-function" -> {
                // Lua code yields a function value to the top level (e.g. after overriding os.pullEventRaw)
                var m = newMachine("coroutine.yield() coroutine.yield(print)");
                m.handleEvent(new Object[0]);
                p("calling handleEvent; bios yields a function ...");
                m.handleEvent(new Object[0]);
                p("UNREACHED? handleEvent returned normally");
            }
            case "cyclic-map" -> {
                var m = newMachine("while true do coroutine.yield() end");
                m.handleEvent(new Object[0]);
                Map<Object, Object> map = new HashMap<>();
                map.put("self", map);
                p("calling handleEvent with a self-referencing Map ...");
                m.handleEvent(new Object[]{"modem_message", map});
                p("UNREACHED? handleEvent returned normally");
            }
            case "cyclic-table-roundtrip" -> {
                var m = newMachine("coroutine.yield() local t = {} t.t = t coroutine.yield(t) while true do coroutine.yield() end");
                m.handleEvent(new Object[0]); // first resume (its results are dropped by the off-by-one bug)
                Object[] r = m.handleEvent(new Object[]{"go"});
                p("Lua->Java cyclic table converted to: " + r[0].getClass().getName() + " size=" + ((Map<?, ?>) r[0]).size() + " selfRef=" + (((Map<?, ?>) r[0]).get("t") == r[0]));
                p("feeding it back (os.queueEvent-style) ...");
                m.handleEvent(new Object[]{"ev", r[0]});
                p("UNREACHED? handleEvent returned normally");
            }
            case "deep-table" -> {
                int depth = Integer.parseInt(a[3]);
                var m = newMachine("coroutine.yield() local t = {} for i = 1, " + depth + " do t = {t} end coroutine.yield(t) while true do coroutine.yield() end");
                m.handleEvent(new Object[0]);
                p("Lua->Java conversion of a table nested " + depth + " deep ...");
                Object[] r = m.handleEvent(new Object[]{"go"});
                int d = 0; Object o = r[0];
                while (o instanceof Map<?, ?> mm && !mm.isEmpty()) { o = mm.get(1.0); d++; }
                p("converted, java depth=" + d);
                p("Java->Lua of the same structure ...");
                Object[] r2 = m.handleEvent(new Object[]{"ev", r[0]});
                p("returned: " + dump(r2));
                m.deinitMachine();
                p("deep-table: completed normally");
            }
            case "abort-plain", "abort-pcall" -> {
                String body = test.equals("abort-plain") ? "while true do end"
                    : "local n = 0 while true do local ok, e = pcall(function() while true do end end) n = n + 1 if n == 1 then ERR = e end end";
                var m = newMachine(body);
                var t = new Thread(() -> {
                    Object[] r = m.handleEvent(new Object[0]);
                    p("handleEvent returned: " + dump(r));
                });
                t.setDaemon(true);
                t.start();
                Thread.sleep(300);
                p("calling abortMachine() from a second thread (what CC:T's monitor/abort path does)");
                m.abortMachine();
                t.join(3000);
                p(t.isAlive() ? "RESULT: still running 3s after abort -> computer thread never freed" : "RESULT: aborted OK");
                System.exit(0);
            }
            case "close-while-running" -> {
                // CC:T 1.21.1 calls ILuaMachine.close() from the monitor thread while the worker is inside handleEvent.
                var m = newMachine("while true do local t = {} for i = 1, 100 do t[i] = {} end end");
                var t = new Thread(() -> {
                    Object[] r = m.handleEvent(new Object[0]);
                    p("handleEvent returned: " + dump(r));
                });
                t.setDaemon(true);
                t.start();
                Thread.sleep(300);
                p("calling deinitMachine() (lua_close) from a second thread while Lua is running ...");
                m.deinitMachine();
                p("deinit returned; waiting for worker ...");
                t.join(3000);
                p(t.isAlive() ? "RESULT: worker still alive (UB but no crash yet)" : "RESULT: worker finished");
                System.exit(0);
            }
            case "api-call" -> {
                // registerAPI + invoke_java_fn path (ILuaObject API), incl. Java exception -> Lua error
                var m = newMachine(
                    "local ok, e = pcall(test.boom) \n" +
                    "local v = test.echo('a\\200b', 1.5, true, {x={y=1}}) \n" +
                    "coroutine.yield(ok, e, v)\n");
                m.registerAPI(new ILuaAPI() {
                    public String[] getNames() { return new String[]{"test"}; }
                    public String[] getMethodNames() { return new String[]{"echo", "boom"}; }
                    public Object[] callMethod(ILuaContext c, int idx, Object[] args) throws LuaException {
                        if (idx == 1) throw new LuaException("boom from java §");
                        p("  java echo received: " + dump(args));
                        return new Object[]{args[0]};
                    }
                });
                p("result: " + dump(m.handleEvent(new Object[0])));
                m.deinitMachine();
                p("api-call: completed normally");
            }
            case "bios-error" -> {
                var m = newMachine("error('bios died')");
                Object[] r = m.handleEvent(new Object[0]);
                p("first resume after error -> " + dump(r));
                r = m.handleEvent(new Object[]{"timer", 1.0});
                p("next event -> " + dump(r) + "   (1.14.4 Java maps this to MachineResult.OK -> computer silently wedged)");
                m.deinitMachine();
            }
            case "first-yield-filter" -> {
                var m = newMachine("coroutine.yield('timer') coroutine.yield('key')");
                p("first resume (filter 'timer' expected) -> " + dump(m.handleEvent(new Object[0])));
                p("second resume (filter 'key' expected) -> " + dump(m.handleEvent(new Object[]{"timer", 1.0})));
                m.deinitMachine();
            }
            case "abort-after-close" -> {
                // 1.14.4 LuaJITMachine.abort() does not check mainRoutine != 0 (close()/deinit may already have run)
                var m = newMachine("while true do coroutine.yield() end");
                m.handleEvent(new Object[0]);
                m.deinitMachine();
                p("deinitMachine done (mainRoutine=" + m.mainRoutine + "); calling abortMachine() ...");
                m.abortMachine();
                p("UNREACHED? abortMachine returned");
            }
            case "gc-upvalue" -> {
                // Needs the audit variant DLL that also opens the debug library (as a 1.21.1 port must).
                var m = newMachine(
                    "coroutine.yield()\n" +
                    "local name, ud = debug.getupvalue(test.echo, 1)\n" +
                    "print('upvalue of API C closure:', type(ud), 'metatable __gc:', getmetatable(ud).__gc)\n" +
                    "getmetatable(ud).__gc(ud)  -- user code runs the finalizer: DeleteGlobalRef + obj = NULL\n" +
                    "print('finalizer called from Lua; now calling test.echo again ...')\n" +
                    "test.echo('x')\n" +
                    "coroutine.yield('UNREACHED')\n");
                m.registerAPI(new ILuaAPI() {
                    public String[] getNames() { return new String[]{"test"}; }
                    public String[] getMethodNames() { return new String[]{"echo"}; }
                    public Object[] callMethod(ILuaContext c, int idx, Object[] args) { return args; }
                });
                m.handleEvent(new Object[0]);
                p("result: " + dump(m.handleEvent(new Object[]{"go"})));
            }
            default -> p("unknown test");
        }
    }
}
