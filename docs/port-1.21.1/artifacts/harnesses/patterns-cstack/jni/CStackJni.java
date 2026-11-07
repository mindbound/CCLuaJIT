/** JVM test for patch 08: deep recursion through C on Java threads with the
 *  default stack size (1 MB on Windows x64), as CC:T's computer threads use.
 *  usage: java CStackJni <cstackjni.dll> <margin_kb> <guard:1|0> [case]
 */
public final class CStackJni {
    static native String run(String src, int marginKB, boolean guard);

    /** Called from Lua through JNI (like a CC:T API method), recursing in Java. */
    static int javaDepth(int n) { return n == 0 ? 0 : 1 + javaDepth(n - 1); }

    static final String[][] CASES = {
        {"gsub_callback", "local function f() D = D + 1 return (('x'):gsub('x', f)) end return f()"},
        {"sort_comparator", "local function f() D = D + 1 table.sort({2, 1}, function(a, b) f() return a < b end) end f()"},
        {"format_s_tostring", "local t t = setmetatable({}, {__tostring = function(x) D = D + 1 return string.format('%s', t) end}) return tostring(t)"},
        {"coroutine_wrap_nesting", "local function f() D = D + 1 return coroutine.wrap(f)() end return f()"},
        {"host_lua_call", "local function f() D = D + 1 return ccall(f) end return f()"},
        {"gsub_with_java_upcalls", "local function f() D = D + 1 jcall(200) return (('x'):gsub('x', f)) end return f()"},
        {"coroutines_with_java_upcalls", "local function f() D = D + 1 jcall(200) return coroutine.wrap(f)() end return f()"},
        {"host_call_with_java_upcalls", "local function f() D = D + 1 jcall(500) return ccall(f) end return f()"},
    };

    public static void main(String[] a) throws Exception {
        System.load(a[0]);
        int margin = Integer.parseInt(a[1]);
        boolean guard = a[2].equals("1");
        for (String[] c : CASES) {
            if (a.length > 3 && !a[3].equals(c[0])) continue;
            String[] out = new String[1];
            Thread t = new Thread(null, () -> out[0] = run(c[1], margin, guard), "computer-worker", 0);
            t.start();
            t.join();
            System.out.printf("  %-30s %s%n", c[0], out[0]);
            System.out.flush();
        }
        System.out.println("  JVM still alive after all cases");
    }
}
