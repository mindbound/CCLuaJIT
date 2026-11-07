/*
** cstack.c -- deep recursion through C on a thread with a small native stack
** (patch 08, C-stack guard). Builds on Windows (MinGW) and Linux (pthreads).
**
** Usage: cstack <case|all|list> [stack_kb=1024] [margin_kb=128] [noguard]
** Each case runs on a fresh thread with the given stack size; the host sets
** the red zone [stack_low, stack_low + margin) with luaJIT_setcstacklimit()
** (unless 'noguard' or a build without LUAJIT_ENABLE_CSTACKGUARD). Each case
** runs twice (the second run checks the state is still usable and behaves
** the same), followed by a small sanity chunk.
*/
#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#include <windows.h>
#else
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

static double now_ms(void)
{
#ifdef _WIN32
  LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* Lowest and highest address of the calling thread's stack. */
static void stack_bounds(uintptr_t *lo, uintptr_t *hi)
{
#ifdef _WIN32
  ULONG_PTR l, h;
  GetCurrentThreadStackLimits(&l, &h);
  *lo = (uintptr_t)l; *hi = (uintptr_t)h;
#else
  pthread_attr_t a; void *addr; size_t size;
  pthread_getattr_np(pthread_self(), &a);
  pthread_attr_getstack(&a, &addr, &size);
  pthread_attr_destroy(&a);
  *lo = (uintptr_t)addr; *hi = (uintptr_t)addr + size;
#endif
}

/* ---- Host C functions that call back into Lua ---------------------------- */

static int l_ccall(lua_State *L)  /* ccall(f, ...) -> lua_call */
{
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}

static int l_cpcallr(lua_State *L)  /* cpcallr(f, ...) -> lua_pcall: ok, ... */
{
  int st = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
  lua_pushboolean(L, st == 0);
  lua_insert(L, 1);
  return lua_gettop(L);
}

static int cpcall_body(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "ccpcall_f");
  lua_call(L, 0, 0);
  return 0;
}
static int l_ccpcall(lua_State *L)  /* ccpcall(f) -> lua_cpcall: ok, err */
{
  int st;
  lua_pushvalue(L, 1);
  lua_setfield(L, LUA_REGISTRYINDEX, "ccpcall_f");
  st = lua_cpcall(L, cpcall_body, NULL);
  lua_pushboolean(L, st == 0);
  if (st) { lua_insert(L, -2); return 2; }
  return 1;
}

static int l_cget(lua_State *L) { lua_settop(L, 2); lua_gettable(L, 1); return 1; }
static int l_cset(lua_State *L) { lua_settop(L, 3); lua_settable(L, 1); return 0; }
static int l_ceq(lua_State *L) { lua_pushboolean(L, lua_equal(L, 1, 2)); return 1; }
static int l_clt(lua_State *L) { lua_pushboolean(L, lua_lessthan(L, 1, 2)); return 1; }
static int l_ccat(lua_State *L) { lua_settop(L, 2); lua_concat(L, 2); return 1; }
static int l_ctostr(lua_State *L) { if (!luaL_callmeta(L, 1, "__tostring")) lua_pushnil(L); return 1; }

/* cresume(co, ...): a host-side coroutine.resume built on lua_resume, as
** the sandbox wrapper of the port plan would be. Returns ok, results...
*/
static int l_cresume(lua_State *L)
{
  lua_State *co = lua_tothread(L, 1);
  int n = lua_gettop(L) - 1, st;
  luaL_checkstack(co, n + 1, "cresume");
  lua_xmove(L, co, n);
  st = lua_resume(co, n);
  if (st == 0 || st == LUA_YIELD) {
    int nres = lua_gettop(co);
    luaL_checkstack(L, nres + 1, "cresume");
    lua_pushboolean(L, 1);
    lua_xmove(co, L, nres);
    return nres + 1;
  }
  lua_pushboolean(L, 0);
  lua_xmove(co, L, 1);
  return 2;
}

static const luaL_Reg hostfns[] = {
  {"ccall", l_ccall}, {"cpcallr", l_cpcallr}, {"ccpcall", l_ccpcall},
  {"cget", l_cget}, {"cset", l_cset}, {"ceq", l_ceq}, {"clt", l_clt},
  {"ccat", l_ccat}, {"ctostr", l_ctostr}, {"cresume", l_cresume}, {NULL, NULL}
};

/* ---- Cases ----------------------------------------------------------------- */

typedef struct Case { const char *name, *src; } Case;

#define DEPTH "D = D + 1 "
static const Case cases[] = {
  /* Pure Lua recursion: bounded by the Lua stack (unchanged). */
  {"lua_recursion", "local function f(n) " DEPTH "return 1 + f(n + 1) end return f(1)"},
  {"pcall_recursion", "local function f() " DEPTH "return pcall(f) end return select(2, f())"},
  {"index_fn_recursion_vm", "local t t = setmetatable({}, {__index = function(s, k) " DEPTH "return t[k] end}) return t.x"},
  {"tostring_tailcall", "local t t = setmetatable({}, {__tostring = function(x) " DEPTH "if D > 1e6 then error('1e6 tail calls, no stack growth', 0) end return tostring(t) end}) return tostring(t)"},
  /* Recursion through C library functions. */
  {"gsub_callback", "local function f() " DEPTH "return (('x'):gsub('x', f)) end return f()"},
  {"gsub_table_index", "local t t = setmetatable({}, {__index = function(s, k) " DEPTH "return (('x'):gsub('x', t)) end}) return (('x'):gsub('x', t))"},
  {"sort_comparator", "local function f() " DEPTH "table.sort({2, 1}, function(a, b) f() return a < b end) end f()"},
  {"sort_lt_metamethod", "local mt = {} mt.__lt = function(a, b) " DEPTH "table.sort({setmetatable({}, mt), setmetatable({}, mt)}) return false end\n"
			 "table.sort({setmetatable({}, mt), setmetatable({}, mt)})"},
  {"tostring_mm_paren", "local t t = setmetatable({}, {__tostring = function(x) " DEPTH "return (tostring(t)) end}) return tostring(t)"},
  {"format_s_tostring", "local t t = setmetatable({}, {__tostring = function(x) " DEPTH "return string.format('%s', t) end}) return tostring(t)"},
  {"table_foreach", "local function f() " DEPTH "table.foreach({1}, f) end f()"},
  {"load_reader", "local E local function r() " DEPTH "local f, e = load(r) E = E or e return nil end load(r) return E"},
  {"xpcall_handler", "local function h(e) " DEPTH "return xpcall(error, h) end return tostring(select(2, xpcall(error, h)))"},
  {"error_in_handler_deep", "local function f() " DEPTH "return (('x'):gsub('x', f)) end return select(2, xpcall(f, function(e) return 'handled: ' .. tostring(e) end))"},
  /* Coroutine nesting (stock coroutine library: VM fast path). */
  {"coroutine_wrap_nesting", "local function f() " DEPTH "return coroutine.wrap(f)() end return f()"},
  {"coroutine_resume_nesting", "local function f() " DEPTH "local co = coroutine.create(f) local ok, e = coroutine.resume(co) if not ok then error(e, 0) end return e end return f()"},
  {"coroutine_precreated_chain",
   "local cos = {} for i = 1, 200000 do cos[i] = coroutine.create(function() " DEPTH "local ok, e = coroutine.resume(cos[i + 1]) error(e, 0) end) end\n"
   "local ok, e = coroutine.resume(cos[1]) error(e, 0)"},
  /* Host C functions (JNI-bridge style) re-entering the VM. */
  {"host_lua_call", "local function f() " DEPTH "return ccall(f) end return f()"},
  {"host_lua_pcall", "local function f() " DEPTH "local r = {cpcallr(f)} if not r[1] then error(r[2], 0) end end f()"},
  {"host_lua_cpcall", "local function f() " DEPTH "local ok, e = ccpcall(f) if not ok then error(e, 0) end end f()"},
  {"host_lua_gettable", "local t t = setmetatable({}, {__index = function(s, k) " DEPTH "return cget(t, k) end}) return cget(t, 'x')"},
  {"host_lua_settable", "local t t = setmetatable({}, {__newindex = function(s, k, v) " DEPTH "cset(t, k, v) end}) cset(t, 'x', 1)"},
  {"host_lua_equal", "local mt = {} local a, b = setmetatable({}, mt), setmetatable({}, mt) mt.__eq = function() " DEPTH "return ceq(a, b) end return ceq(a, b)"},
  {"host_lua_lessthan", "local mt = {} local a, b = setmetatable({}, mt), setmetatable({}, mt) mt.__lt = function() " DEPTH "return clt(a, b) end return clt(a, b)"},
  {"host_lua_concat", "local mt = {} local a = setmetatable({}, mt) mt.__concat = function() " DEPTH "return ccat(a, 'x') end return ccat(a, 'x')"},
  {"host_callmeta", "local t t = setmetatable({}, {__tostring = function() " DEPTH "return ctostr(t) end}) return ctostr(t)"},
  {"host_lua_resume_nesting", "local function f() " DEPTH "local co = coroutine.create(f) local r = {cresume(co)} if not r[1] then error(r[2], 0) end return r[2] end return f()"},
  /* lua_resume refused in the red zone: the coroutine must stay resumable. */
  {"host_resume_stays_resumable",
   "local co = coroutine.create(function(a) while true do a = coroutine.yield('got ' .. tostring(a)) end end)\n"
   "local err local function f() " DEPTH "local ok, v = cresume(co, D) if not ok then err = v return end return ccall(f) end f()\n"
   "local ok, v = cresume(co, 'again') return tostring(err) .. ' / later: ' .. tostring(ok) .. ' ' .. tostring(v) .. ' ' .. coroutine.status(co)"},
  {"stock_resume_stays_resumable",
   "local co = coroutine.create(function(a) while true do a = coroutine.yield('got ' .. tostring(a)) end end)\n"
   "local err local function f() " DEPTH "local ok, v = coroutine.resume(co, D) if not ok then err = v return end return coroutine.wrap(f)() end f()\n"
   "local ok, v = coroutine.resume(co, 'again') return tostring(err) .. ' / later: ' .. tostring(ok) .. ' ' .. tostring(v) .. ' ' .. coroutine.status(co)"},
  /* Parser: nesting at a deep C stack. */
  {"parser_at_depth", "local src = 'return ' .. ('('):rep(190) .. '1' .. (')'):rep(190)\n"
		      "local function f() " DEPTH "local fn, e = load(src) if not fn then error(e, 0) end return (('x'):gsub('x', f)) end return f()"},
  /* Mixed: coroutines inside gsub callbacks inside sort comparators. */
  {"mixed_chain", "local f f = function(k) " DEPTH "k = k % 3 if k == 0 then return (('x'):gsub('x', function() return f(1) end))\n"
		  "elseif k == 1 then return coroutine.wrap(function() return f(2) end)() else table.sort({2, 1}, function(a, b) f(0) return a < b end) return 'y' end end\n"
		  "return f(0)"},
  {NULL, NULL}
};

static const Case *cur;
static int use_guard = 1;
static size_t margin_kb = 128;
static int crashed_marker;

static void run_case_on_this_thread(void)
{
  lua_State *L = luaL_newstate();
  uintptr_t lo, hi;
  int run, st;
  double t0;
  luaL_openlibs(L);
  luaL_register(L, "_G", hostfns); lua_pop(L, 1);
  stack_bounds(&lo, &hi);
#ifdef HAVE_CSTACKGUARD
  if (use_guard) luaJIT_setcstacklimit(L, (void *)lo, (void *)(lo + margin_kb * 1024));
#endif
  for (run = 1; run <= 2; run++) {
    const char *msg; char buf[160];
    lua_pushinteger(L, 0); lua_setglobal(L, "D");
    t0 = now_ms();
    st = luaL_loadstring(L, cur->src);
    if (!st) st = lua_pcall(L, 0, 1, 0);
    msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : (lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "true" : "false") : "nil");
    snprintf(buf, sizeof(buf), "%s", msg);
    for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    lua_pop(L, 1);
    lua_getglobal(L, "D");
    printf("  run %d: %s depth=%-6d %.0f ms: %s\n", run, st ? "error " : "ok    ",
	   (int)lua_tointeger(L, -1), now_ms() - t0, buf);
    lua_pop(L, 1);
    fflush(stdout);
  }
  st = luaL_dostring(L, "local t = {} for i = 1, 1000 do t[i] = tostring(i) end\n"
			"local co = coroutine.wrap(function(a) return (a:gsub('%d', function(d) return d + 1 end)) end)\n"
			"return #table.concat(t) .. ' ' .. co('a1b2')");
  printf("  state usable: %s (lua stack top %d)\n", st ? lua_tostring(L, -1) : lua_tostring(L, -1), lua_gettop(L) - 1);
  lua_close(L);
  crashed_marker = 1;
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) { (void)p; run_case_on_this_thread(); return 0; }
#else
static void *thread_main(void *p) { (void)p; run_case_on_this_thread(); return NULL; }
#endif

static int run_case(const Case *c, size_t stack_kb)
{
  cur = c; crashed_marker = 0;
  printf("CASE %s (stack %u KB, %s)\n", c->name, (unsigned)stack_kb,
	 use_guard ? "guard on" : "guard off"); fflush(stdout);
#ifdef _WIN32
  {
    HANDLE th = CreateThread(NULL, stack_kb * 1024, thread_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (WaitForSingleObject(th, 60000) != WAIT_OBJECT_0) { printf("  RESULT HUNG\n"); fflush(stdout); ExitProcess(3); }
    CloseHandle(th);
  }
#else
  {
    pthread_t th; pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_kb * 1024);
    pthread_create(&th, &a, thread_main, NULL);
    pthread_join(th, NULL);
  }
#endif
  printf("  RESULT %s\n", crashed_marker ? "COMPLETED (no crash)" : "???");
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv)
{
  size_t stack_kb = argc > 2 ? (size_t)atoi(argv[2]) : 1024;
  const Case *c;
  if (argc > 3) margin_kb = (size_t)atoi(argv[3]);
  if (argc > 4 && !strcmp(argv[4], "noguard")) use_guard = 0;
#ifndef HAVE_CSTACKGUARD
  use_guard = 0;
#endif
  if (argc < 2 || !strcmp(argv[1], "list")) { for (c = cases; c->name; c++) puts(c->name); return 0; }
  for (c = cases; c->name; c++)
    if (!strcmp(argv[1], "all") || !strcmp(argv[1], c->name)) run_case(c, stack_kb);
  return 0;
}
