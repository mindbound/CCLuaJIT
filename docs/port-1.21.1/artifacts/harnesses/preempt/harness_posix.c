/*
** Preemption harness for LuaJIT: a second thread interrupts a Lua state that
** runs an endless loop (often in JIT-compiled code) and measures how long it
** takes until the count hook runs on the VM thread.
**
** Usage: harness <method> <action> <case|list> [delay_ms]
**   method: sethook  - other thread calls lua_sethook(L, hook, LUA_MASKCOUNT, 1)
**                      (the protocol of the original CCLuaJIT; NOT thread-safe)
**           intr     - VM thread installs a permanent count hook (count HOOKN, default 1e4)
**                      at startup; other thread calls luaJIT_interrupt(L)
**                      (needs a -DLUAJIT_ENABLE_INTERRUPT build)
**   action: error    - hook raises an error, lua_pcall must return it
**           yield    - see yieldtest(): long checksum loop run in a coroutine,
**                      interrupted every 2 ms, hook yields, host resumes.
**           nested   - hook yields once while the loop runs in a *nested*
**                      coroutine (shows where a hook yield goes).
**
** Build: gcc -O2 -I<luajit>/src harness.c <luajit>/src/libluajit.a
*/
#include "win32_on_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"   /* Internal: only to sample G(L)->vmstate. */
#include "compat_intr.h"

typedef struct Case { const char *name; const char *src; } Case;

static const Case cases[] = {
  {"while_true", "while true do end"},
  {"numeric_for_1e12", "local x = 0 for i = 1, 1e12 do x = x + i end"},
  {"repeat_until", "local i = 0 repeat i = i + 1 until i < 0"},
  {"goto_loop", "local i = 0 ::top:: i = i + 1 goto top"},
  {"tail_recursion", "local function f(n) return f(n + 1) end return f(0)"},
  {"mutual_tail_recursion",
   "local a, b function a(n) return b(n + 1) end function b(n) return a(n - 1) end return a(0)"},
  {"up_down_recursion_loop",
   "local function f(n) if n > 0 then return 1 + f(n - 1) end return 0 end\n"
   "local s = 0 while true do s = s + f(200) end"},
  {"deep_recursion_loop",
   "local function f(n) if n > 0 then f(n - 1) end end while true do f(5000) end"},
  {"loop_calls_lua_fn",
   "local function g(x) return (x * 3 + 1) % 1000003 end local s = 1 while true do s = g(s) end"},
  {"nested_loops",
   "local x = 0 while true do for i = 1, 100 do for j = 1, 100 do x = x + j end end end"},
  {"side_traces",
   "local i, a, b, c = 0, 0, 0, 0\n"
   "while true do i = i + 1 local m = i % 3\n"
   "  if m == 0 then a = a + 1 elseif m == 1 then b = b + 1 else c = c + 1 end end"},
  {"coroutine_pingpong",
   "local co = coroutine.wrap(function() while true do coroutine.yield(1) end end)\n"
   "local s = 0 while true do s = s + co() end"},
  {"loop_inside_coroutine",
   "local co = coroutine.create(function() local x = 0 while true do x = x + 1 end end)\n"
   "local ok, err = coroutine.resume(co) error(err, 0)"},
  {"string_concat",
   "local s = '' while true do s = s .. 'x' if #s > 4096 then s = '' end end"},
  {"table_concat",
   "local t, n = {}, 0 while true do n = n + 1 t[#t + 1] = tostring(n)\n"
   "  if #t > 1000 then local s = table.concat(t) t = {} end end"},
  {"pairs_loop",
   "local t = {a = 1, b = 2, c = 3, 1, 2, 3} local s = 0\n"
   "while true do for k, v in pairs(t) do s = s + v end end"},
  {"ipairs_loop",
   "local t = {1, 2, 3, 4} local s = 0 while true do for i, v in ipairs(t) do s = s + v end end"},
  {"pcall_loop", "local f = function() end while true do pcall(f) end"},
  {"stitch_c_call", "local c = 0 while true do c = c + os.clock() end"},
  {"alloc_loop", "while true do local t = {} for i = 1, 100 do t[i] = {i} end end"},
  {"metamethod_loop",
   "local o = setmetatable({}, {__index = function(t, k) return k end})\n"
   "local s = 0 while true do s = s + o[1] end"},
  {"gsub_callback", "while true do string.gsub('aaaa', 'a', function(c) return c end) end"},
  {"load_loop", "while true do load('return 1')() end"},
  {"interp_only_jit_off", "jit.off() while true do end"},
  {"bit_ops", "local x = 1 while true do x = bit.bxor(bit.rol(x, 5), 0x9e3779b9) end"},
  /* Expected to be non-interruptible by any hook-based scheme: */
  {"X_finalizer_loop",
   "local p = newproxy(true) getmetatable(p).__gc = function() while true do end end\n"
   "p = nil collectgarbage() collectgarbage()"},
  {"X_lua_hook_loop",
   "debug.sethook(function() while true do end end, '', 1) local x = 0 x = x + 1"},
  {"X_string_find_backtrack",
   "return string.find(string.rep('a', 40), string.rep('a*', 20) .. 'b')"},
  {NULL, NULL}
};

static lua_State *gL;
static int method_intr;          /* 0: async lua_sethook, 1: luaJIT_interrupt */
static int delay_ms = 300;
static int hookn = 10000;      /* Count of the permanent hook (env HOOKN). */
static volatile LONG pending;    /* Host-side request flag (intr method). */
static volatile LONG fired;
static volatile LONGLONG t_req, t_fire;
static volatile LONG done;
static LARGE_INTEGER freq;
static volatile int yield_mode;
static volatile LONG nyields, nnotyieldable;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double us(LONGLONG d) { return (double)d * 1e6 / (double)freq.QuadPart; }

static const char *vmstate_str(int32_t st, char *buf)
{
  static const char *names[] = {"INTERP","C","GC","EXIT","RECORD","OPT","ASM"};
  if (st >= 0) sprintf(buf, "TRACE#%d", (int)st);
  else if (~st < 7) strcpy(buf, names[~st]);
  else sprintf(buf, "?%d", (int)st);
  return buf;
}

static void hook_action(lua_State *L)
{
  t_fire = qpc();
  InterlockedIncrement(&fired);
  if (yield_mode) {
    if (lua_isyieldable(L)) { InterlockedIncrement(&nyields); lua_yield(L, 0); }
    InterlockedIncrement(&nnotyieldable);
    return;
  }
  luaL_error(L, "interrupted");
}

/* Hook for the 'sethook' method: one-shot, clears itself (VM thread). */
static void hook_oneshot(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  lua_sethook(L, NULL, 0, 0);
  hook_action(L);
}

/* Hard abort: once triggered, error at every instruction (VM thread). */
static volatile int hard_mode;
static volatile int hard_yield;  /* Abort by yielding to the host if possible. */
static void hook_hard(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (hard_yield && lua_isyieldable(L)) lua_yield(L, 0);
  luaL_error(L, "hard abort");
}

/* Permanent hook for the 'intr' method (installed once from the VM thread). */
static void hook_permanent(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (!InterlockedExchange(&pending, 0)) return;  /* Periodic call, no request. */
  if (hard_mode) {  /* Re-arm with count 1 from the VM thread, then error. */
    t_fire = qpc(); InterlockedIncrement(&fired);
    lua_sethook(L, hook_hard, LUA_MASKCOUNT, 1);
    if (hard_yield && lua_isyieldable(L)) lua_yield(L, 0);
    luaL_error(L, "hard abort");
  }
  hook_action(L);
}

static void request(void)
{
  if (method_intr) {
    InterlockedExchange(&pending, 1);
    luaJIT_interrupt_compat(gL);
  } else {
    lua_sethook(gL, hook_oneshot, LUA_MASKCOUNT, 1);
  }
}

/* Interrupter for the 'error' action. */
static DWORD WINAPI interrupter(LPVOID p)
{
  int i, intrace = 0, nsamp = 0; int32_t last = 0; char b1[32], b2[32];
  (void)p;
  Sleep(delay_ms - 100);
  for (i = 0; i < 100; i++) {  /* Sample VM state for 100 ms. */
    int32_t st = G(gL)->vmstate; nsamp++; if (st >= 0) { intrace++; last = st; }
    Sleep(1);
  }
  {
    int32_t st = G(gL)->vmstate;
    t_req = qpc();
    request();
    printf("  state at request: %s (in-trace samples %d/%d, last trace %d)\n",
	   vmstate_str(st, b1), intrace, nsamp, (int)last);
  }
  while (!fired && !done && us(qpc() - t_req) < 3e6) Sleep(1);
  if (!fired && !done) {
    printf("  RESULT HANG: hook did not run within 3 s (vmstate now %s)\n",
	   vmstate_str(G(gL)->vmstate, b2));
    fflush(stdout);
    ExitProcess(3);
  }
  return 0;
}

static void print_linktypes(lua_State *L)
{
  if (luaL_dostring(L,
    "local ok, u = pcall(require, 'jit.util') if not ok then return end\n"
    "local t, n = {}, 0 for i = 1, 100000 do local ti = u.traceinfo(i)\n"
    "  if not ti then break end n = n + 1 t[ti.linktype] = (t[ti.linktype] or 0) + 1 end\n"
    "local s = {} for k, v in pairs(t) do s[#s + 1] = k .. '=' .. v end table.sort(s)\n"
    "io.write('  traces: ', n, ' {', table.concat(s, ' '), '}\\n')"))
    printf("  (linktype query failed: %s)\n", lua_tostring(L, -1));
}

static int run_error_case(const Case *c)
{
  lua_State *L = luaL_newstate();
  HANDLE th; int status; LONGLONG t0;
  gL = L;
  luaL_openlibs(L);
  if (method_intr) lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);
  printf("CASE %s [%s]\n", c->name, method_intr ? "luaJIT_interrupt" : "async lua_sethook");
  if (luaL_loadstring(L, c->src)) { printf("  load error %s\n", lua_tostring(L, -1)); return 1; }
  th = CreateThread(NULL, 0, interrupter, NULL, 0, NULL);
  t0 = qpc();
  status = lua_pcall(L, 0, 0, 0);
  done = 1;
  if (fired) {
    printf("  RESULT OK: status=%d msg=\"%s\" latency=%.1f us (ran %.0f ms)\n", status,
	   status ? lua_tostring(L, -1) : "", us(t_fire - t_req), us(qpc() - t0) / 1000);
  } else {
    printf("  RESULT NOFIRE: status=%d msg=\"%s\"\n", status, status ? lua_tostring(L, -1) : "");
  }
  if (status) lua_pop(L, 1);
  lua_sethook(L, NULL, 0, 0);
  print_linktypes(L);
  WaitForSingleObject(th, INFINITE);
  fflush(stdout);
  return 0;
}

/* ---- Yield test ---------------------------------------------------------- */

static const char yield_src[] =
  "local function work(n)\n"
  "  local s, h = 0, 0\n"
  "  for i = 1, n do\n"
  "    local m = i % 7\n"
  "    if m == 3 then h = (h * 31 + i) % 1000000007 else s = s + m end\n"
  "  end\n"
  "  return s, h\n"
  "end\n"
  "return work\n";

static volatile LONG yt_stop;
static volatile LONG yt_samples, yt_intrace;

static DWORD WINAPI yield_interrupter(LPVOID p)
{
  (void)p;
  while (!yt_stop) {
    Sleep(2);
    if (yt_stop) break;
    yt_samples++; if (G(gL)->vmstate >= 0) yt_intrace++;
    request();
  }
  return 0;
}

static int count_traces(lua_State *L)
{
  int n;
  luaL_dostring(L, "local u = require('jit.util') local n = 0 "
		   "for i = 1, 100000 do if not u.traceinfo(i) then break end n = i end return n");
  n = (int)lua_tointeger(L, -1); lua_pop(L, 1);
  return n;
}

static int yieldtest(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co; HANDLE th; int st, resumes = 0, ntr0, ntr1, ntr2;
  double s_ref, h_ref, s, h; LONGLONG t0, t1, t2, t3, t4, t5;
  const lua_Number N = 1.5e9;
  gL = L; yield_mode = 1;
  luaL_openlibs(L);
  if (method_intr) lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);
  printf("YIELDTEST [%s] N=%.3g\n", method_intr ? "luaJIT_interrupt" : "async lua_sethook", N);
  luaL_loadstring(L, yield_src); lua_call(L, 0, 1);
  lua_setglobal(L, "work");
  if (getenv("JITV")) {  /* Optional: log trace events with jit.v. */
    char b[300];
    sprintf(b, "require('jit.v').on('%s')", getenv("JITV"));
    if (luaL_dostring(L, b)) printf("jit.v: %s\n", lua_tostring(L, -1));
  }
  /* Reference run: no interrupts. */
  t0 = qpc();
  lua_getglobal(L, "work"); lua_pushnumber(L, N); lua_call(L, 1, 2);
  t1 = qpc();
  s_ref = lua_tonumber(L, -2); h_ref = lua_tonumber(L, -1); lua_pop(L, 2);
  ntr0 = count_traces(L);
  printf("  reference: s=%.0f h=%.0f time=%.0f ms traces=%d\n", s_ref, h_ref, us(t1 - t0) / 1000, ntr0);
  /* Interrupted run: coroutine yielded from the hook every ~2 ms. */
  co = lua_newthread(L);
  lua_getglobal(co, "work"); lua_pushnumber(co, N);
  th = CreateThread(NULL, 0, yield_interrupter, NULL, 0, NULL);
  t2 = qpc();
  st = lua_resume(co, 1); resumes++;
  while (st == LUA_YIELD) {
    if (!method_intr) lua_sethook(co, NULL, 0, 0);  /* Clear hook (VM thread). */
    st = lua_resume(co, 0); resumes++;
  }
  t3 = qpc();
  yt_stop = 1; WaitForSingleObject(th, INFINITE);
  if (st != 0) { printf("  RESULT FAIL: resume status %d: %s\n", st, lua_tostring(co, -1)); return 1; }
  s = lua_tonumber(co, -2); h = lua_tonumber(co, -1);
  printf("  interrupted: s=%.0f h=%.0f time=%.0f ms resumes=%d hook-yields=%ld "
	 "hook-runs=%ld requester-samples-in-trace=%ld/%ld\n",
	 s, h, us(t3 - t2) / 1000, resumes, nyields, fired, yt_intrace, yt_samples);
  printf("  RESULT %s: checksum %s reference\n", (s == s_ref && h == h_ref) ? "OK" : "FAIL",
	 (s == s_ref && h == h_ref) ? "matches" : "DIFFERS from");
  /* After clearing hooks: JIT must still compile new code and run it fast. */
  yield_mode = 0;
  yt_stop = 0;
  if (!method_intr) lua_sethook(L, NULL, 0, 0);
  ntr1 = count_traces(L);
  luaL_loadstring(L, "local n = ... local x = 0 for i = 1, n do x = x + bit.band(i, 255) end return x");
  t4 = qpc(); lua_pushnumber(L, 3e8); lua_call(L, 1, 1); t5 = qpc();
  ntr2 = count_traces(L);
  printf("  after: new chunk 3e8 iters in %.0f ms, traces before=%d after=%d -> %s\n",
	 us(t5 - t4) / 1000, ntr1, ntr2, ntr2 > ntr1 ? "JIT still compiles" : "NO NEW TRACE");
  fflush(stdout);
  return 0;
}

/* ---- Nested coroutine: where does a hook yield go? ----------------------- */

static int nestedtest(void)
{
  lua_State *L = luaL_newstate();
  lua_State *co; HANDLE th; int st;
  gL = L; yield_mode = 1;
  luaL_openlibs(L);
  if (method_intr) lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);
  printf("NESTED [%s]\n", method_intr ? "luaJIT_interrupt" : "async lua_sethook");
  luaL_loadstring(L,
    "local inner = coroutine.create(function() local x = 0 while true do x = x + 1 end end)\n"
    "local r = {coroutine.resume(inner)}\n"
    "return 'outer saw inner.resume -> ' .. tostring(r[1]) .. ' #extra=' .. (#r - 1) .."
    " ', inner status=' .. coroutine.status(inner)");
  co = lua_newthread(L);
  lua_pushvalue(L, -2); lua_xmove(L, co, 1);
  th = CreateThread(NULL, 0, interrupter, NULL, 0, NULL);
  st = lua_resume(co, 0);
  done = 1;
  printf("  host lua_resume -> %d (%s); %s\n", st,
	 st == LUA_YIELD ? "YIELD" : st == 0 ? "finished" : "error",
	 lua_isstring(co, -1) ? lua_tostring(co, -1) : "");
  WaitForSingleObject(th, INFINITE);
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv)
{
  int i;
  QueryPerformanceFrequency(&freq);
  timeBeginPeriod(1);  /* 1 ms Sleep() granularity. */
  if (getenv("HOOKN")) hookn = atoi(getenv("HOOKN"));
  if (argc < 4) { fprintf(stderr, "usage: harness sethook|intr error|yield|nested case|list [delay]\n"); return 2; }
  method_intr = !strcmp(argv[1], "intr");
  if (argc > 4) delay_ms = atoi(argv[4]);
  if (!strcmp(argv[2], "yield")) return yieldtest();
  if (!strcmp(argv[2], "nested")) return nestedtest();
  if (!strcmp(argv[2], "hardabort")) {  /* pcall-swallowing loops */
    static const Case hc[] = {
      {"pcall_swallow", "while true do pcall(function() while true do end end) end"},
      {"xpcall_handler_loop",
       "while true do xpcall(function() while true do end end, function(e) while true do end end) end"},
      {"coroutine_swallow",
       "while true do coroutine.resume(coroutine.create(function() while true do end end)) end"},
      {NULL, NULL}
    };
    hard_mode = 1;
    hard_yield = argc > 5 && !strcmp(argv[5], "yield");
    for (i = 0; hc[i].name; i++)
      if (!strcmp(hc[i].name, argv[3])) {
	if (!hard_yield) return run_error_case(&hc[i]);
	{  /* Run inside a coroutine; the host just abandons it on a yield. */
	  lua_State *L = luaL_newstate(), *co; HANDLE th; int st; int depth = 0;
	  gL = L; luaL_openlibs(L);
	  lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);
	  printf("CASE %s [luaJIT_interrupt, hard abort by yield]\n", hc[i].name);
	  co = lua_newthread(L);
	  luaL_loadstring(co, hc[i].src);
	  th = CreateThread(NULL, 0, interrupter, NULL, 0, NULL);
	  st = lua_resume(co, 0);
	  done = 1;
	  { lua_Debug ar; while (lua_getstack(co, depth, &ar)) depth++; }
	  printf("  RESULT %s: lua_resume -> %d (%s) latency=%.1f us, %d Lua frames left"
		 " (host would now lua_close the state)\n",
		 st == LUA_YIELD ? "OK" : "FAIL", st, st == LUA_YIELD ? "yield" : lua_tostring(co, -1),
		 us(t_fire - t_req), depth);
	  WaitForSingleObject(th, INFINITE);
	  lua_close(L);
	  return 0;
	}
      }
    return 2;
  }
  if (!strcmp(argv[3], "list")) { for (i = 0; cases[i].name; i++) puts(cases[i].name); return 0; }
  for (i = 0; cases[i].name; i++)
    if (!strcmp(cases[i].name, argv[3])) return run_error_case(&cases[i]);
  fprintf(stderr, "unknown case\n");
  return 2;
}
