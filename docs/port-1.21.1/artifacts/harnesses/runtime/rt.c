/*
** rt.c -- empirical LuaJIT C-API runtime behaviour tests relevant to a
** ComputerCraft: Tweaked ILuaMachine implemented on top of LuaJIT.
** Compiles as C (gcc) and as C++ (g++ -x c++). Windows / MinGW-w64.
**
** Usage: rt <test> [args...]   (see main() at the bottom)
** Every test prints lines beginning with "R " (result lines) so the
** driver can grep them.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>
#ifdef __cplusplus
#include "lua.hpp"
#include <stdexcept>
#define LANG "C++"
#else
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"
#define LANG "C"
#endif

/* ------------------------------------------------------------------ utils */

static double now_ms(void)
{
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void R(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fputs("R ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}

static void oneline(char *s)
{
  for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

static int l_cf(lua_State *L)
{
  lua_pushnumber(L, luaL_optnumber(L, 1, 0) + 1);
  return 1;
}
static int l_now(lua_State *L) { lua_pushnumber(L, now_ms()); return 1; }
static int l_cerr(lua_State *L) { return luaL_error(L, "cerr raised %d", 42); }
/* C function that calls a Lua function with lua_call (like a Java-callback
** bridge that calls back into Lua). */
static int l_ccall(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}
static int l_cpcall(lua_State *L)
{
  int st;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  st = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
  lua_pushboolean(L, st == 0);
  lua_insert(L, 1);
  return lua_gettop(L);
}
/* C function that yields (like a CC:T "pullEvent" primitive done in C). */
static int l_cyield(lua_State *L) { return lua_yield(L, lua_gettop(L)); }
static int l_isyieldable(lua_State *L)
{
  lua_pushboolean(L, lua_isyieldable(L));
  return 1;
}

static void reg_helpers(lua_State *L)
{
  lua_register(L, "cf", l_cf);
  lua_register(L, "now", l_now);
  lua_register(L, "cerr", l_cerr);
  lua_register(L, "ccall", l_ccall);
  lua_register(L, "cpcall", l_cpcall);
  lua_register(L, "cyield", l_cyield);
  lua_register(L, "isyieldable", l_isyieldable);
}

static lua_State *newstate_std(void)
{
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  reg_helpers(L);
  return L;
}

/* Load + pcall a string; returns status; prints error into buf. */
static int dostr(lua_State *L, const char *code, char *err, size_t errsz)
{
  int st = luaL_loadstring(L, code);
  if (!st) st = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (st && err) {
    const char *m = lua_tostring(L, -1);
    snprintf(err, errsz, "%s", m ? m : "(non-string error)");
    oneline(err);
  }
  if (st) lua_pop(L, 1);
  return st;
}

static void must(lua_State *L, const char *code)
{
  char e[512];
  if (dostr(L, code, e, sizeof(e))) {
    R("SETUP-ERROR %s :: %s", code, e);
    exit(10);
  }
  lua_settop(L, 0);
}

static const char *stname(int st)
{
  switch (st) {
  case 0: return "OK";
  case LUA_YIELD: return "YIELD";
  case LUA_ERRRUN: return "ERRRUN";
  case LUA_ERRSYNTAX: return "ERRSYNTAX";
  case LUA_ERRMEM: return "ERRMEM";
  case LUA_ERRERR: return "ERRERR";
  default: return "?";
  }
}

/* --------------------------------------------- test a: async interrupt */

static const struct { const char *name; const char *code; } SHAPES[] = {
  {"while", "while true do end"},
  {"fornum", "local x=0 for i=1,1e12 do x=x+i end return x"},
  {"repeat", "repeat until false"},
  {"goto", "::a:: goto a"},
  {"mathsin", "local s=0 while true do s=s+math.sin(s) end"},
  {"alloc", "local t while true do t={1,2,3} end"},
  {"concat", "local n,i=0,0 while true do i=i+1 n=n+#('x'..i) end"},
  {"luacall", "local function g(x) return x+1 end local x=0 while true do x=g(x) end"},
  {"ccall", "local x=0 while true do x=cf(x) end"},
  {"strbyte", "local s='abc' local x=0 while true do x=x+s:byte(2) end"},
  {"tailrec", "local function f(n) return f(n+1) end return f(0)"},
  {"mutualtail", "local f,g function f(n) return g(n+1) end function g(n) return f(n+1) end return f(0)"},
  {"corowrap", "local gen=coroutine.wrap(function() local i=0 while true do i=i+1 coroutine.yield(i) end end) local s=0 while true do s=s+gen() end"},
  {"nested", "local x=0 while true do for i=1,100 do for j=1,100 do x=x+j end end end"},
  {"sidetrace", "local x,i=0,0 while true do i=i+1 if i%1000==0 then x=x+1 else x=x-1 end end"},
  {"pairs", "local t={1,2,3,a=1,b=2} local n=0 while true do for k,v in pairs(t) do n=n+1 end end"},
  {"ipairs", "local t={1,2,3,4} local n=0 while true do for k,v in ipairs(t) do n=n+v end end"},
  {"pcallloop", "local function g() end while true do pcall(g) end"},
  {"strfmt", "local n=0 while true do n=n+#string.format('%d',n) end"},
  {"recurse", "local function f(n) if n==0 then return 0 end return 1+f(n-1) end while true do f(100) end"},
  {"upval", "local x=0 local function inc() x=x+1 end while true do inc() end"},
  {"tblinsrem", "local t={} while true do table.insert(t,1) table.remove(t) end"},
  {"pcallspin", "local function g() for i=1,1e6 do end end while true do pcall(g) end"},
  {"xpcallspin", "local function g() for i=1,1e6 do end end while true do xpcall(g, function(e) return e end) end"},
  {"nestedpcallspin", "local function g() for i=1,1e6 do end end while true do pcall(function() while true do pcall(g) end end) end"},
  {NULL, NULL}
};

static lua_State *gL;
static volatile double t_set, t_fire;
static volatile LONG a_done;
static volatile LONG abort_flag;
static int a_delay_ms = 300, a_limit_ms = 2000;

static int a_sticky = 0;
static volatile long err_hook_calls;
static void hook_err(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  err_hook_calls++;
  if (t_fire == 0) t_fire = now_ms();
  if (!a_sticky) lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted");
}

/* Permanent-hook design: hook always installed, checks a flag. */
static volatile long perm_calls;
static void hook_perm(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  perm_calls++;
  if (abort_flag) {
    if (t_fire == 0) t_fire = now_ms();
    abort_flag = 0;
    luaL_error(L, "interrupted");
  }
}

static int a_permhook = 0;
static DWORD WINAPI interrupter(LPVOID p)
{
  int i;
  (void)p;
  Sleep(a_delay_ms);
  t_set = now_ms();
  if (a_permhook) abort_flag = 1;
  else lua_sethook(gL, hook_err, LUA_MASKCOUNT, 1);
  for (i = 0; i < a_limit_ms / 5; i++) {
    if (a_done) return 0;
    Sleep(5);
  }
  R("HUNG fired=%d", t_fire != 0);
  ExitProcess(3);
  return 0;
}

static int test_a(int argc, char **argv)
{
  const char *shape = argv[2], *cfg = argv[3], *runner = argv[4];
  const char *code = NULL, *pre = "";
  int i, st;
  char err[512] = "", err2[256] = "";
  lua_State *L, *co;
  HANDLE th;
  for (i = 0; SHAPES[i].name; i++)
    if (!strcmp(SHAPES[i].name, shape)) code = SHAPES[i].code;
  if (!code) { R("bad shape"); return 2; }
  if (!strcmp(cfg, "on")) pre = "";
  else if (!strcmp(cfg, "off")) pre = "jit.off()";
  else if (!strcmp(cfg, "O0")) pre = "jit.opt.start(0)";
  else if (!strcmp(cfg, "noloop")) pre = "jit.opt.start('-loop')";
  else if (!strcmp(cfg, "maxside0")) pre = "jit.opt.start('maxside=0')";
  else if (!strcmp(cfg, "hotexit1")) pre = "jit.opt.start('hotexit=1')";
  else if (!strcmp(cfg, "hotloop60k")) pre = "jit.opt.start('hotloop=60000','hotexit=60000')";
  else if (!strcmp(cfg, "permhook")) { pre = ""; a_permhook = 1; }
  else if (!strcmp(cfg, "permhookoff")) { pre = "jit.off()"; a_permhook = 1; }
  else if (!strcmp(cfg, "offsticky")) { pre = "jit.off()"; a_sticky = 1; }
  else if (!strcmp(cfg, "onsticky")) { pre = ""; a_sticky = 1; }
  else { R("bad cfg"); return 2; }
  if (argc > 5) a_delay_ms = atoi(argv[5]);
  L = newstate_std();
  gL = L;
  must(L, pre);
  if (a_permhook) lua_sethook(L, hook_perm, LUA_MASKCOUNT, 1000);
  th = CreateThread(NULL, 0, interrupter, NULL, 0, NULL);
  if (!strcmp(runner, "pcall")) {
    st = dostr(L, code, err, sizeof(err));
  } else {
    co = lua_newthread(L);
    luaL_loadstring(co, code);
    st = lua_resume(co, 0);
    if (st > LUA_YIELD) {
      snprintf(err, sizeof(err), "%s", lua_tostring(co, -1));
      oneline(err);
    }
  }
  a_done = 1;
  /* Is the state usable afterwards? */
  if (a_permhook || a_sticky) lua_sethook(L, NULL, 0, 0);
  lua_settop(L, 0);
  {
    int st2 = dostr(L, "local s=0 for i=1,1000 do s=s+i end assert(s==500500) return s", err2, sizeof(err2));
    if (t_fire != 0)
      R("A %s %s %s INTERRUPTED lat_ms=%.2f st=%s msg=[%s] after=%s %s hookcalls=%ld", shape, cfg, runner,
        t_fire - t_set, stname(st), err, st2 ? "FAIL" : "ok", err2, err_hook_calls);
    else
      R("A %s %s %s FINISHED-WITHOUT-HOOK st=%s msg=[%s]", shape, cfg, runner, stname(st), err);
  }
  WaitForSingleObject(th, 100);
  return 0;
}

/* ------------------------------------------- test b: yield from hook */

static volatile long yields;
static void hook_yield(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  yields++;
  lua_yield(L, 0);
}
static void hook_yield_once(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  lua_sethook(L, NULL, 0, 0);
  yields++;
  lua_yield(L, 0);
}
/* The hook as a careful machine would write it. */
static volatile long deferred;
static void hook_yield_checked(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (lua_isyieldable(L)) {
    lua_sethook(L, NULL, 0, 0);
    yields++;
    lua_yield(L, 0);
  } else {
    deferred++;  /* keep hook installed, try again on a later instruction */
  }
}

static volatile LONG b_stop;
static DWORD WINAPI b_pinger(LPVOID p)
{
  lua_State *L = (lua_State *)p;
  while (!b_stop) {
    Sleep(20);
    if (!b_stop) lua_sethook(L, hook_yield_once, LUA_MASKCOUNT, 1);
  }
  return 0;
}

static int run_to_completion(lua_State *co, int nargs, long *resumes, char *err, size_t errsz)
{
  int st;
  *resumes = 0;
  for (;;) {
    st = lua_resume(co, nargs);
    nargs = 0;
    (*resumes)++;
    if (st != LUA_YIELD) break;
    lua_settop(co, 0);
    if (*resumes > 100000000) break;
  }
  if (st && err) {
    const char *m = lua_tostring(co, -1);
    snprintf(err, errsz, "%s", m ? m : "?");
    oneline(err);
  }
  return st;
}

static const char *B_CODE =
  "local s=0\n"
  "for i=1,N do\n"
  "  if i%M==0 then s=s+cf(0) end\n"
  "  if i%3==0 then s=s+1 elseif i%3==1 then s=s+2.5 else s=s-0.25 end\n"
  "end\n"
  "return s";

static double b_reference(const char *pre, const char *code)
{
  lua_State *L = newstate_std();
  double r;
  must(L, "jit.off()");
  must(L, pre);
  luaL_loadstring(L, code);
  lua_pcall(L, 0, 1, 0);
  r = lua_tonumber(L, -1);
  lua_close(L);
  return r;
}

static int test_b(int argc, char **argv)
{
  char err[512];
  const char *pre = "N=3e7 M=100000";
  double ref = b_reference(pre, B_CODE), got;
  long resumes;
  int st;
  (void)argc; (void)argv;
  R("B reference=%.17g", ref);

  /* b1: interpreted, permanent hook every 1000 instructions, yields */
  {
    lua_State *L = newstate_std(), *co;
    must(L, "jit.off()"); must(L, pre);
    co = lua_newthread(L);
    luaL_loadstring(co, B_CODE);
    yields = 0;
    lua_sethook(co, hook_yield, LUA_MASKCOUNT, 1000);
    { double t0 = now_ms();
    st = run_to_completion(co, 0, &resumes, err, sizeof(err));
    got = lua_tonumber(co, -1);
    R("B b1-interp-permhook1000 st=%s result=%.17g %s yields=%ld resumes=%ld ms=%.0f", stname(st), got,
      got == ref ? "CORRECT" : "WRONG", yields, resumes, now_ms()-t0); }
    lua_close(L);
  }
  /* b2: JIT on, permanent hook every 1000 instructions, yields */
  {
    lua_State *L = newstate_std(), *co;
    must(L, pre);
    co = lua_newthread(L);
    luaL_loadstring(co, B_CODE);
    yields = 0;
    lua_sethook(co, hook_yield, LUA_MASKCOUNT, 1000);
    { double t0 = now_ms();
    st = run_to_completion(co, 0, &resumes, err, sizeof(err));
    got = lua_tonumber(co, -1);
    R("B b2-jit-permhook1000 st=%s result=%.17g %s yields=%ld resumes=%ld ms=%.0f", stname(st), got,
      got == ref ? "CORRECT" : "WRONG", yields, resumes, now_ms()-t0); }
    lua_close(L);
  }
  /* b3: JIT on, async one-shot yield hook every ~20 ms from another thread */
  {
    lua_State *L = newstate_std(), *co;
    HANDLE th;
    must(L, pre);
    must(L, "N=3e8");
    { lua_State *L2 = NULL; (void)L2; }
    co = lua_newthread(L);
    luaL_loadstring(co, B_CODE);
    yields = 0; b_stop = 0;
    th = CreateThread(NULL, 0, b_pinger, co, 0, NULL);
    { double t0 = now_ms();
    st = run_to_completion(co, 0, &resumes, err, sizeof(err));
    b_stop = 1; WaitForSingleObject(th, INFINITE);
    got = lua_tonumber(co, -1);
    { double ref2 = b_reference("N=3e8 M=100000", B_CODE);
    R("B b3-jit-async-yield st=%s result=%.17g %s yields=%ld resumes=%ld ms=%.0f", stname(st), got,
      got == ref2 ? "CORRECT" : "WRONG", yields, resumes, now_ms()-t0); } }
    lua_close(L);
  }
  /* b4: yield from hook while running in the main thread under lua_pcall */
  {
    lua_State *L = newstate_std();
    must(L, "jit.off()");
    yields = 0;
    lua_sethook(L, hook_yield_once, LUA_MASKCOUNT, 1000);
    st = dostr(L, "local s=0 for i=1,1e6 do s=s+i end return s", err, sizeof(err));
    R("B b4-mainthread-pcall-hookyield st=%s msg=[%s]", stname(st), st ? err : "");
    lua_close(L);
  }
  /* b5..b8: hook fires inside various frames inside a coroutine */
  {
    static const struct { const char *n, *code; } C[] = {
      {"inside-pcall", "local ok,r=pcall(function() local s=0 for i=1,2e6 do s=s+i end return s end) return r"},
      {"inside-__index", "local t=setmetatable({}, {__index=function(t,k) local s=0 for i=1,2e6 do s=s+i end return s end}) return t.x"},
      {"inside-sort-cmp", "local n=0 local t={} for i=1,200 do t[i]=(i*7919)%200 end table.sort(t,function(a,b) for i=1,200 do n=n+1 end return a<b end) return n"},
      {"inside-gsub-cb", "local n=0 local s=('x'):rep(500):gsub('x',function() for i=1,1000 do n=n+1 end return 'y' end) return n"},
      {"inside-ccall", "return ccall(function() local s=0 for i=1,2e6 do s=s+i end return s end)"},
      {"inside-tostring-mm", "local t=setmetatable({}, {__tostring=function() local s=0 for i=1,2e6 do s=s+i end return tostring(s) end}) return tostring(t)"},
      {NULL, NULL}
    };
    int k, mode;
    for (mode = 0; mode < 2; mode++) {
      for (k = 0; C[k].n; k++) {
        lua_State *L = newstate_std(), *co;
        must(L, "jit.off()");
        co = lua_newthread(L);
        luaL_loadstring(co, C[k].code);
        yields = 0; deferred = 0;
        lua_sethook(co, mode ? hook_yield_checked : hook_yield, LUA_MASKCOUNT, 10000);
        st = run_to_completion(co, 0, &resumes, err, sizeof(err));
        R("B hook-%s %s st=%s yields=%ld deferred=%ld result=%s msg=[%s]",
          mode ? "checked(isyieldable)" : "naive", C[k].n, stname(st), yields, deferred,
          st == 0 ? lua_tostring(co, -1) : "-", st ? err : "");
        lua_close(L);
      }
    }
  }
  return 0;
}

/* ---------------------------------------------- test c: yield across */

static int test_c(int argc, char **argv)
{
  static const struct { const char *n, *code; } C[] = {
    {"plain", "local v=coroutine.yield('y') return v"},
    {"pcall", "local ok,v=pcall(function() return coroutine.yield('y') end) return v"},
    {"xpcall", "local ok,v=xpcall(function() return coroutine.yield('y') end, debug.traceback) return v"},
    {"xpcall-handler", "local ok,v=xpcall(function() error('e') end, function(e) return coroutine.yield('y') end) return v"},
    {"pcall-os.pullEvent-style", "local function pullEvent(f) local ev={coroutine.yield(f)} if ev[1]=='terminate' then error('Terminated',0) end return unpack(ev) end local ok,v=pcall(pullEvent,'key') return v"},
    {"__index", "local t=setmetatable({}, {__index=function(t,k) return coroutine.yield('y') end}) return t.x"},
    {"__newindex", "local r local t=setmetatable({}, {__newindex=function(t,k,v) r=coroutine.yield('y') end}) t.x=1 return r"},
    {"__call", "local t=setmetatable({}, {__call=function() return coroutine.yield('y') end}) return t()"},
    {"__lt", "local mt={__lt=function(a,b) return coroutine.yield('y') end} local a,b=setmetatable({},mt),setmetatable({},mt) return a<b"},
    {"__le", "local mt={__le=function(a,b) return coroutine.yield('y') end} local a,b=setmetatable({},mt),setmetatable({},mt) return a<=b"},
    {"__eq", "local mt={__eq=function(a,b) return coroutine.yield('y') end} local a,b=setmetatable({},mt),setmetatable({},mt) return a==b"},
    {"__concat", "local t=setmetatable({}, {__concat=function() return coroutine.yield('y') end}) return t..'x'"},
    {"__add", "local t=setmetatable({}, {__add=function() return coroutine.yield('y') end}) return t+1"},
    {"__unm", "local t=setmetatable({}, {__unm=function() return coroutine.yield('y') end}) return -t"},
    {"__len(5.2)", "local t=setmetatable({}, {__len=function() return coroutine.yield('y') end}) return #t"},
    {"for-in-iterator", "local r for v in function(s,c) if c then return nil end return coroutine.yield('y') end do r=v end return r"},
    {"table.foreach", "local r table.foreach({1}, function(k,v) r=coroutine.yield('y') end) return r"},
    {"table.sort-cmp", "local r local t={3,1,2} table.sort(t, function(a,b) r=coroutine.yield('y') return a<b end) return r"},
    {"string.gsub-cb", "local r=('a'):gsub('a', function() return coroutine.yield('y') end) return r"},
    {"tostring-__tostring", "local t=setmetatable({}, {__tostring=function() return coroutine.yield('y') end}) return tostring(t)"},
    {"string.format-%s-__tostring", "local t=setmetatable({}, {__tostring=function() return coroutine.yield('y') end}) return string.format('%s',t)"},
    {"print-__tostring", "local t=setmetatable({}, {__tostring=function() return coroutine.yield('y') end}) print(t) return 'printed'"},
    {"C-lua_call", "return ccall(function() return coroutine.yield('y') end)"},
    {"C-lua_pcall", "local ok,v=cpcall(function() return coroutine.yield('y') end) return v"},
    {"C-function-lua_yield", "return cyield('y')"},
    {"load-reader", "local done local f=load(function() if done then return nil end done=true return coroutine.yield('y') end) return f and f() or 'nofunc'"},
    {"select/unpack", "return select(1, coroutine.yield('y'))"},
    {"coroutine.wrap-nested", "local w=coroutine.wrap(function() return coroutine.yield('inner') end) local a=w() local b=coroutine.yield('y') return b"},
    {"require-like-dofile", "local f=load('return coroutine.yield(\"y\")') return f()"},
    {"__pairs(5.2)", "local t=setmetatable({}, {__pairs=function(t) return function() return nil end, t, nil end}) local n=0 for k,v in pairs(t) do n=n+1 end return '__pairs-not-yield-test n='..n"},
    {NULL, NULL}
  };
  int k, st, st2;
  char err[512];
  (void)argc; (void)argv;
  for (k = 0; C[k].n; k++) {
    lua_State *L = newstate_std(), *co;
    co = lua_newthread(L);
    luaL_loadstring(co, C[k].code);
    st = lua_resume(co, 0);
    if (st == LUA_YIELD) {
      lua_settop(co, 0);
      lua_pushstring(co, "resumed-value");
      st2 = lua_resume(co, 1);
      if (st2 == 0)
        R("C %-28s YIELD-OK final=%s", C[k].n, lua_tostring(co, -1) ? lua_tostring(co, -1) : (lua_toboolean(co,-1)?"true":"false/nil"));
      else {
        snprintf(err, sizeof(err), "%s", lua_tostring(co, -1)); oneline(err);
        R("C %-28s YIELD-then-%s [%s]", C[k].n, stname(st2), err);
      }
    } else if (st == 0) {
      R("C %-28s NO-YIELD-RETURNED %s", C[k].n, lua_tostring(co, -1) ? lua_tostring(co, -1) : "?");
    } else {
      snprintf(err, sizeof(err), "%s", lua_tostring(co, -1)); oneline(err);
      R("C %-28s FAIL %s [%s]", C[k].n, stname(st), err);
    }
    lua_close(L);
  }
  return 0;
}

typedef struct { size_t used, cap, peak; long fails; } MemCap;
static void *cap_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  MemCap *m = (MemCap *)ud;
  if (nsize == 0) {
    if (ptr) { m->used -= osize; free(ptr); }
    return NULL;
  }
  if (ptr == NULL) osize = 0;
  if (nsize > osize && m->used + (nsize - osize) > m->cap) { m->fails++; if (getenv("TRACEOOM")) printf("  [alloc fail o=%zu n=%zu used=%zu cap=%zu]\n", osize, nsize, m->used, m->cap); return NULL; }
  {
    void *p = realloc(ptr, nsize);
    if (!p) { m->fails++; return NULL; }
    m->used = m->used - osize + nsize;
    if (m->used > m->peak) m->peak = m->used;
    return p;
  }
}

/* ------------------------------------------------------ test d: errors */

#ifdef __cplusplus
static int dtor_ran = 0;
struct Guard { ~Guard() { dtor_ran++; } };
static int l_cpp_raii_err(lua_State *L)
{
  Guard g;
  luaL_error(L, "error through C++ frame with RAII object");
  return 0;
}
static int l_cpp_throw(lua_State *L)
{
  (void)L;
  throw std::runtime_error("c++ exception from lua_CFunction");
  return 0;
}
static int l_cpp_trycatch_luacall(lua_State *L)
{
  try {
    lua_call(L, lua_gettop(L) - 1, 0);
  } catch (std::exception &e) {
    lua_pushstring(L, "caught std::exception");
    return 1;
  } catch (...) {
    lua_pushstring(L, "caught (...) -> rethrow");
    throw;
  }
  lua_pushstring(L, "no exception seen by try/catch");
  return 1;
}
#endif

static jmp_buf panic_jb;
static int panic_count;
static int my_panic(lua_State *L)
{
  panic_count++;
  fprintf(stdout, "R D panic handler called: %s\n", lua_tostring(L, -1));
  fflush(stdout);
  longjmp(panic_jb, 1);
  return 0;
}

static int test_d(int argc, char **argv)
{
  const char *sub = argc > 2 ? argv[2] : "all";
  char err[512];
  int st;
  if (!strcmp(sub, "all")) {
    /* d1: luaL_error from a C function during lua_resume */
    {
      lua_State *L = newstate_std(), *co = lua_newthread(L);
      luaL_loadstring(co, "local x = 1 cerr() return x");
      st = lua_resume(co, 0);
      snprintf(err, sizeof(err), "%s", lua_tostring(co, -1));
      R("D d1 luaL_error-in-C-during-resume st=%s msg=[%s] costatus=%d", stname(st), err, lua_status(co));
      st = dostr(L, "return 1+1", err, sizeof(err));
      R("D d1 main state usable after: %s", st ? err : "ok");
      /* resume dead coroutine again */
      st = lua_resume(co, 0);
      R("D d1 resume-dead-coroutine-again st=%s msg=[%s]", stname(st), lua_tostring(co, -1));
      lua_close(L);
    }
    /* d2: non-string error objects through resume */
    {
      lua_State *L = newstate_std(), *co = lua_newthread(L);
      luaL_loadstring(co, "error({code=7})");
      st = lua_resume(co, 0);
      R("D d2 table-error-through-resume st=%s type=%s", stname(st), luaL_typename(co, -1));
      lua_close(L);
    }
    /* d3: error in xpcall handler */
    {
      lua_State *L = newstate_std();
      st = dostr(L, "local ok,e=xpcall(function() error('a') end, function(e) error('b') end) return tostring(ok)..' '..tostring(e)", err, sizeof(err));
      R("D d3 error-in-xpcall-handler st=%s ret=%s", stname(st), st ? err : lua_tostring(L, -1));
      lua_close(L);
    }
    /* d4: lua_pcall with errfunc that errors -> LUA_ERRERR */
    {
      lua_State *L = newstate_std();
      luaL_loadstring(L, "return function(e) error('handler fails') end");
      lua_pcall(L, 0, 1, 0);
      luaL_loadstring(L, "error('x')");
      st = lua_pcall(L, 0, 0, 1);
      R("D d4 lua_pcall-errfunc-errors st=%s msg=[%s]", stname(st), lua_tostring(L, -1));
      lua_close(L);
    }
#ifdef __cplusplus
    /* d5: error unwinding through a C++ frame with a destructor */
    {
      lua_State *L = newstate_std();
      lua_register(L, "raii", l_cpp_raii_err);
      dtor_ran = 0;
      st = dostr(L, "raii()", err, sizeof(err));
      R("D d5 C++ luaL_error-through-RAII-frame st=%s msg=[%s] destructor_ran=%d", stname(st), err, dtor_ran);
      lua_close(L);
    }
    /* d6: C++ exception thrown from a lua_CFunction inside lua_pcall */
    {
      lua_State *L = newstate_std();
      lua_register(L, "cppthrow", l_cpp_throw);
      st = dostr(L, "cppthrow()", err, sizeof(err));
      R("D d6 C++-throw-inside-lua_pcall st=%s msg=[%s]", stname(st), err);
      st = dostr(L, "return 1", err, sizeof(err));
      R("D d6 state usable after: %s", st ? err : "ok");
      lua_close(L);
    }
    /* d7: try/catch around lua_call when Lua errors */
    {
      lua_State *L = newstate_std();
      lua_register(L, "trycatch", l_cpp_trycatch_luacall);
      st = dostr(L, "return trycatch(function() error('lua error') end)", err, sizeof(err));
      R("D d7 C++ try/catch around lua_call, Lua error: st=%s msg=[%s]", stname(st), st ? err : lua_tostring(L, -1));
      lua_close(L);
    }
    /* d8: try { lua_error outside any pcall } catch(...) */
    {
      lua_State *L = newstate_std();
      int caught = 0;
      try {
        lua_pushstring(L, "unprotected");
        lua_error(L);
      } catch (...) {
        caught = 1;
      }
      R("D d8 C++ catch(...) around unprotected lua_error: caught=%d", caught);
      if (caught) {
        st = dostr(L, "return 1", err, sizeof(err));
        R("D d8 state usable after catch: %s", st ? err : "ok");
      }
    }
#endif
    return 0;
  }
  if (!strcmp(sub, "unprotected")) {
    lua_State *L = luaL_newstate();
    R("D unprotected: calling lua_error with no protected frame (" LANG ")");
    lua_pushstring(L, "boom");
    lua_error(L);
    R("D unprotected: returned?!");
    return 0;
  }
  if (!strcmp(sub, "atpanic")) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_atpanic(L, my_panic);
    if (setjmp(panic_jb) == 0) {
      R("D atpanic: calling lua_error unprotected (" LANG ")");
      lua_pushstring(L, "boom");
      lua_error(L);
      R("D atpanic: lua_error returned?!");
    } else {
      R("D atpanic: longjmp'd out of panic, panic_count=%d; trying to reuse the state", panic_count);
      st = dostr(L, "local t={} for i=1,1e5 do t[i]=i end return #t", err, sizeof(err));
      R("D atpanic: reuse st=%s %s", stname(st), st ? err : "");
    }
    return 0;
  }
  if (!strcmp(sub, "unprotected-oom")) {
    /* API calls outside protection when the (capped) allocator refuses */
    static MemCap mc = {0, 8u << 20, 0, 0};
    lua_State *L = lua_newstate(cap_alloc, &mc);
    int i;
    R("D unprotected-oom: 8MB-capped state; lua_createtable outside any pcall until OOM");
    for (i = 0; i < 1000; i++) {
      lua_createtable(L, 1 << 16, 0);  /* ~1 MB each */
      lua_rawseti(L, LUA_REGISTRYINDEX, i + 10);
    }
    R("D unprotected-oom: survived?!");
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------ test e: memory */

static int cp_gc(lua_State *L) { lua_gc(L, LUA_GCCOLLECT, 0); return 0; }
static int test_e(int argc, char **argv)
{
  MemCap m = {0, 32u << 20, 0, 0};
  lua_State *L = lua_newstate(cap_alloc, &m);
  char err[512];
  int st, k;
  static const struct { const char *n, *code; } C[] = {
    {"table-of-tables", "local t={} for i=1,1e9 do t[i]={i} end"},
    {"string.rep-1GB", "local s=string.rep('x', 2^30) return #s"},
    {"string-growth", "local s='x' while true do s=s..s end"},
    {"jit-trace-alloc", "local t={} local i=0 while true do i=i+1 t[i]=i end"},
    {"table.concat-big", "local t={} for i=1,1e6 do t[i]=('y'):rep(40) end return #table.concat(t)"},
    {"many-unique-strings", "local t={} for i=1,1e8 do t[i]=tostring(i)..'_padding_padding' end"},
    {NULL, NULL}
  };
  R("E lua_newstate(custom allocator) on GC64 -> %s", L ? "OK (non-NULL)" : "NULL");
  if (!L) return 1;
  luaL_openlibs(L);
  reg_helpers(L);
  R("E after openlibs used=%.1f KB", m.used / 1024.0);
  for (k = 0; C[k].n; k++) {
    m.peak = m.used; m.fails = 0;
    { double t0 = now_ms();
    st = dostr(L, C[k].code, err, sizeof(err));
    R("E %-20s st=%s msg=[%s] peak=%.1fMB fails=%ld ms=%.0f", C[k].n, stname(st), st ? err : "", m.peak / 1048576.0, m.fails, now_ms()-t0); }
    lua_settop(L, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    st = dostr(L, "local t={} for i=1,1e5 do t[i]=i..'' end return #t", err, sizeof(err));
    R("E   after GC used=%.1fMB; state usable: %s", m.used / 1048576.0, st ? err : "ok");
    lua_settop(L, 0);
  }
  /* OOM inside a coroutine */
  {
    lua_State *co = lua_newthread(L);
    luaL_loadstring(co, "local t={} for i=1,1e9 do t[i]={i} end");
    st = lua_resume(co, 0);
    R("E OOM-inside-lua_resume st=%s msg=[%s] used=%.1fMB", stname(st), lua_tostring(co, -1), m.used / 1048576.0);
    lua_pop(L, 1);
    if (argc > 2 && !strcmp(argv[2], "unprotected-gc")) {
      R("E calling lua_gc(COLLECT) unprotected while at the cap...");
      lua_gc(L, LUA_GCCOLLECT, 0);
      R("E unprotected lua_gc survived");
    } else {
      st = lua_cpcall(L, cp_gc, NULL);
      R("E lua_gc(COLLECT) inside lua_cpcall at cap: st=%s %s used=%.1fMB", stname(st), st ? lua_tostring(L, -1) : "", m.used / 1048576.0);
      if (st) lua_pop(L, 1);
      st = lua_cpcall(L, cp_gc, NULL);
      R("E second protected lua_gc: st=%s used=%.1fMB", stname(st), m.used / 1048576.0);
      if (st) lua_pop(L, 1);
      m.cap += 256u << 10;  /* give the GC 256 KB of headroom */
      st = lua_cpcall(L, cp_gc, NULL);
      R("E protected lua_gc after raising cap by 256KB: st=%s used=%.1fMB", stname(st), m.used / 1048576.0);
      if (st) lua_pop(L, 1);
      m.cap -= 256u << 10;
      st = dostr(L, "local t={} for i=1,1e5 do t[i]=i..'' end return #t", err, sizeof(err));
      R("E   state usable afterwards: %s", st ? err : "ok");
      lua_settop(L, 0);
    }
  }
  /* GC accounting: what lua_gc(COUNT) reports vs allocator */
  R("E lua_gc COUNT=%d KB, allocator used=%.0f KB", lua_gc(L, LUA_GCCOUNT, 0), m.used / 1024.0);
  lua_close(L);
  R("E after lua_close allocator used=%zu bytes (expect 0)", m.used);

  /* Allocator speed: custom (msvcrt realloc) vs builtin lj_alloc */
  {
    const char *bench = "local t0=now() for r=1,20 do local t={} for i=1,2e5 do t[i]={i,i} end end return now()-t0";
    MemCap m2 = {0, (size_t)4 << 30, 0, 0};
    lua_State *L1 = luaL_newstate(), *L2 = lua_newstate(cap_alloc, &m2);
    luaL_openlibs(L1); luaL_openlibs(L2); reg_helpers(L1); reg_helpers(L2);
    luaL_loadstring(L1, bench); lua_pcall(L1, 0, 1, 0);
    luaL_loadstring(L2, bench); lua_pcall(L2, 0, 1, 0);
    R("E alloc-bench luaL_newstate(lj_alloc)=%.0f ms  lua_newstate(realloc+cap)=%.0f ms",
      lua_tonumber(L1, -1), lua_tonumber(L2, -1));
    lua_close(L1); lua_close(L2);
  }
  return 0;
}

/* -------------------------------------------------- test f: stack depth */

static const struct { const char *n, *code; } FCASES[] = {
  {"lua-recursion", "local function f(n) return 1+f(n+1) end return f(1)"},
  {"pcall-recursion", "local d=0 local function f() d=d+1 return pcall(f) end f() return d"},
  {"__index-fn-recursion", "local t t=setmetatable({}, {__index=function(s,k) return t[k] end}) return t.x"},
  {"__concat-recursion", "local t t=setmetatable({}, {__concat=function(a,b) return t..b end}) return t..'x'"},
  {"gsub-recursion", "local function f() return (('x'):gsub('x', f)) end return f()"},
  {"sort-cmp-recursion", "local function f() table.sort({2,1}, function(a,b) f() return a<b end) end f()"},
  {"tostring-mm-recursion", "local t t=setmetatable({}, {__tostring=function(x) return (tostring(t)) end}) return tostring(t)"},
  {"tostring-mm-tailcall", "local t t=setmetatable({}, {__tostring=function(x) return tostring(t) end}) return tostring(t)"},
  {"strformat-mm-recursion", "local t t=setmetatable({}, {__tostring=function(x) return string.format('%s', t) end}) return tostring(t)"},
  {"ccall-recursion", "local function f() return ccall(f) end return f()"},
  {"coroutine-nesting", "local d=0 local function f() d=d+1 return coroutine.wrap(f)() end local ok,e=pcall(f) return tostring(e)..' depth='..d"},
  {"coroutine-resume-nesting", "local d=0 local function f() d=d+1 local co=coroutine.create(f) local ok,e=coroutine.resume(co) if not ok then error(e,0) end return e end local ok,e=pcall(f) return tostring(e)..' depth='..d"},
  {"load-reader-recursion", "local function r() load(r) return nil end load(r)"},
  {"parser-paren-nesting", "local f,e=load('return '..('('):rep(1e5)..'1'..(')'):rep(1e5)) return tostring(e)"},
  {"parser-table-nesting", "local f,e=load('return '..('{'):rep(1e5)..('}'):rep(1e5)) return tostring(e)"},
  {"parser-func-nesting", "local f,e=load(('return function() '):rep(1e4)..('end '):rep(1e4)) return tostring(e)"},
  {"unpack-huge", "local t={} for i=1,1e6 do t[i]=i end return select('#', unpack(t))"},
  {"xpcall-handler-recursion", "local function h(e) return xpcall(error, h) end return tostring(xpcall(error, h))"},
  {"string.rep-sep", "return #string.rep('ab', 1e7, ',')"},
  {NULL, NULL}
};

static int f_case;
static DWORD WINAPI f_thread(LPVOID p)
{
  lua_State *L = newstate_std();
  char err[512];
  int st;
  double t0 = now_ms();
  (void)p;
  st = dostr(L, FCASES[f_case].code, err, sizeof(err));
  R("F %-26s st=%s %s ms=%.0f", FCASES[f_case].n, stname(st), st ? err : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "ok"), now_ms() - t0);
  lua_close(L);
  return 0;
}

static int test_f(int argc, char **argv)
{
  int i, kb = argc > 3 ? atoi(argv[3]) : 1024;
  HANDLE th;
  f_case = -1;
  for (i = 0; FCASES[i].n; i++) if (!strcmp(FCASES[i].n, argv[2])) f_case = i;
  if (f_case < 0) {
    for (i = 0; FCASES[i].n; i++) printf("%s\n", FCASES[i].n);
    return 2;
  }
  th = CreateThread(NULL, (SIZE_T)kb * 1024, f_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (WaitForSingleObject(th, 20000) != WAIT_OBJECT_0) { R("F %s HUNG", argv[2]); ExitProcess(3); }
  return 0;
}

/* ------------------------------------------------- test g: bytecode */

static int test_g(int argc, char **argv)
{
  lua_State *L = newstate_std();
  char err[512];
  int st, k;
  static const struct { const char *n, *code; } C[] = {
    {"load(dump)", "local d=string.dump(function() return 'bc-ran' end) return load(d)()"},
    {"load(dump,n,'t')", "local d=string.dump(function() return 'bc-ran' end) local f,e=load(d,'x','t') return f and f() or ('refused: '..e)"},
    {"load(dump,n,'b')", "local d=string.dump(function() return 'bc-ran' end) local f,e=load(d,'x','b') return f and f() or ('refused: '..e)"},
    {"loadstring(dump)", "local d=string.dump(function() return 'bc-ran' end) return loadstring(d)()"},
    {"load(reader->dump)", "local d=string.dump(function() return 'bc-ran' end) local g=false return load(function() if g then return nil end g=true return d end)()"},
    {"load(reader->dump,'t')", "local d=string.dump(function() return 'bc-ran' end) local g=false local f,e=load(function() if g then return nil end g=true return d end, 'x', 't') return f and f() or ('refused: '..e)"},
    {"loadfile(bytecode-on-host-disk)", "local d=string.dump(function() return 'bc-ran-from-host-file' end) local fh=io.open('bc.tmp','wb') fh:write(d) fh:close() return loadfile('bc.tmp')()"},
    {"loadfile(file,'t')", "local f,e=loadfile('bc.tmp','t') return f and f() or ('refused: '..e)"},
    {"dofile(bytecode)", "return dofile('bc.tmp')"},
    {"require(bytecode via package.path)", "package.path='./?.tmp' package.loaded.bc=nil return require('bc')"},
    {"string.dump(nil?)", "local d=string.dump string.dump=nil local r=('x'):rep(3)..' '..type(string.dump)..' '..tostring(('abc'):upper())..' '..tostring(('%d'):format(5)) string.dump=d return r"},
    {"string.buffer-load", "local ok,buf=pcall(require,'string.buffer') if not ok then return 'no string.buffer: '..buf end local b=buf.new() b:put(string.dump(function() return 'bc-ran-via-buffer' end)) local f,e=load(b) return f and f() or e"},
    {"package.loadlib", "return tostring(package.loadlib)..' '..tostring(package.cpath)"},
    {"jit.util.ircalladdr", "local u=require('jit.util') return string.format('ircalladdr(0)=0x%x funcinfo.source=%s', u.ircalladdr(0), tostring(u.funcinfo(print).ffid))"},
    {"ffi-peek", "local ffi=require('ffi') local p=ffi.cast('uint8_t*', ffi.cast('void*', 0x7ffe0000)) return 'ffi read KUSER_SHARED_DATA byte: '..p[0x26c]"},
    {"getfenv(0)/registry", "return type(debug.getregistry())..' '..tostring(debug.getregistry()._LOADED~=nil)"},
    {NULL, NULL}
  };
  (void)argc; (void)argv;
  for (k = 0; C[k].n; k++) {
    st = dostr(L, C[k].code, err, sizeof(err));
    R("G %-34s st=%s %s", C[k].n, stname(st), st ? err : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "-"));
    lua_settop(L, 0);
  }
  /* luaL_loadbufferx from C with mode "t" */
  {
    size_t len;
    const char *d;
    must(L, "string.dump = string.dump or nil");
    luaL_loadstring(L, "return string.dump(function() return 1 end)");
    lua_pcall(L, 0, 1, 0);
    d = lua_tolstring(L, -1, &len);
    if (d) {
      st = luaL_loadbufferx(L, d, len, "=x", "t");
      R("G C:luaL_loadbufferx(dump,'t') st=%s msg=[%s]", stname(st), st ? lua_tostring(L, -1) : "");
    } else R("G C:luaL_loadbufferx skipped (string.dump removed by earlier case)");
  }
  lua_close(L);
  remove("bc.tmp");
  return 0;
}

/* Bytecode fuzz: mutate N bytes of a dumped function and run it. */
static int test_gfuzz(int argc, char **argv)
{
  int seed = atoi(argv[2]);
  lua_State *L = newstate_std();
  char code[2048], err[512];
  int st;
  snprintf(code, sizeof(code),
    "math.randomseed(%d)\n"
    "local function victim(a,b) local t={a,b,'str',1.5} for i=1,#t do t[i]=tostring(t[i])..i end return table.concat(t,',') end\n"
    "local d={string.dump(victim):byte(1,-1)}\n"
    "for k=1,3 do local p=math.random(6,#d) d[p]=math.random(0,255) end\n"
    "local s=string.char(unpack(d))\n"
    "local f,e=load(s) if not f then return 'load-refused: '..e end\n"
    "local ok,r=pcall(f,1,2) return (ok and 'ran: ' or 'lua-error: ')..tostring(r)", seed);
  st = dostr(L, code, err, sizeof(err));
  R("GF seed=%d st=%s %s", seed, stname(st), st ? err : lua_tostring(L, -1));
  return 0;
}

/* --------------------------------------------- test h: thread affinity */

typedef struct { lua_State *L, *co; int step; double result; int st; char err[256]; } HJob;
static const char *H_CODE =
  "local acc=0\n"
  "for r=1,R do\n"
  "  local t={} for i=1,2000 do t[i]=i*1.5 end\n"
  "  for i=1,#t do acc=acc+t[i]%7 end\n"
  "  local s={} for i=1,200 do s[#s+1]=tostring(i) end acc=acc+#table.concat(s)\n"
  "  local co=coroutine.wrap(function() for i=1,100 do coroutine.yield(i) end end)\n"
  "  for i=1,100 do acc=acc+co() end\n"
  "  acc=acc+math.floor(math.sin(r)*1000)\n"
  "  if r%50==0 then acc=acc+cf(0) end\n"
  "  if r==150 and YIELD then coroutine.yield() end\n"
  "end\n"
  "return acc";

static DWORD WINAPI h_step(LPVOID p)
{
  HJob *j = (HJob *)p;
  if (j->step == 0) {
    j->L = newstate_std();
    must(j->L, "R=300 YIELD=true");
    j->co = lua_newthread(j->L);
    luaL_loadstring(j->co, H_CODE);
    yields = 0;
    /* explicit coroutine.yield at r==150 (YIELD=true) */
    j->st = lua_resume(j->co, 0);
  } else if (j->step == 2) {
    char err[256];
    lua_settop(j->L, 0);
    must(j->L, "YIELD=nil local s=0 for i=1,1e7 do s=s+i%3 end assert(s==10000000)");
    j->st = dostr(j->L, H_CODE, err, sizeof(err));
    if (j->st) snprintf(j->err, sizeof(j->err), "%s", err); else j->result = lua_tonumber(j->L, -1);
  } else {
    j->st = lua_resume(j->co, 0);
    if (j->st == 0) j->result = lua_tonumber(j->co, -1);
    if (j->st > 1) { snprintf(j->err, sizeof(j->err), "%s", lua_tostring(j->co, -1)); }
  }
  return 0;
}

typedef struct { int id; int iters; double expect; int bad; lua_State *L; volatile LONG busy; } HWork;
static DWORD WINAPI h_worker(LPVOID p)
{
  HWork *w = (HWork *)p;
  int i;
  for (i = 0; i < w->iters; i++) {
    lua_State *L = newstate_std();
    char err[256];
    w->L = L;
    must(L, "R=300");
    if (dostr(L, H_CODE, err, sizeof(err))) { w->bad++; R("H worker %d error %s", w->id, err); }
    else if (lua_tonumber(L, -1) != w->expect) { w->bad++; R("H worker %d WRONG %.17g", w->id, lua_tonumber(L, -1)); }
    w->L = NULL;
    lua_close(L);
  }
  return 0;
}

/* Interrupt-race stress: each worker runs JIT-heavy code with diverse loops;
** a separate thread fires lua_sethook(error) at random times. */
static const char *HS_CODE =
  "local acc=0\n"
  "for r=1,1e9 do\n"
  "  local t={} for i=1,50+r%37 do t[i]=i*1.5 end\n"
  "  for i=1,#t do if i%3==0 then acc=acc+t[i] elseif i%5==0 then acc=acc-1 else acc=acc+0.5 end end\n"
  "  local s='' for i=1,5 do s=s..i end acc=acc+#s\n"
  "  for k,v in pairs(t) do acc=acc+k end\n"
  "  acc=acc+(cf(r)%3)\n"
  "end";
typedef struct { int id; volatile lua_State *L; volatile LONG phase; long runs, interrupted, other; } HSWork;
static volatile LONG hs_stop;
static void hs_hook(lua_State *L, lua_Debug *ar) { (void)ar; lua_sethook(L, NULL, 0, 0); luaL_error(L, "interrupted"); }
static DWORD WINAPI hs_worker(LPVOID p)
{
  HSWork *w = (HSWork *)p;
  while (!hs_stop) {
    lua_State *L = newstate_std();
    char err[256];
    int st;
    st = luaL_loadstring(L, HS_CODE);
    w->L = L;
    st = lua_pcall(L, 0, 0, 0);
    w->L = NULL;
    /* make sure interrupter isn't touching L anymore */
    while (InterlockedCompareExchange(&w->phase, 0, 0) == 1) Sleep(0);
    if (st) {
      snprintf(err, sizeof(err), "%s", lua_tostring(L, -1));
      if (strstr(err, "interrupted")) w->interrupted++; else { w->other++; R("H stress worker %d unexpected error: %s", w->id, err); }
    }
    w->runs++;
    lua_close(L);
  }
  return 0;
}

static int test_h(int argc, char **argv)
{
  int nthreads = argc > 2 ? atoi(argv[2]) : 32;
  int stress_secs = argc > 3 ? atoi(argv[3]) : 20;
  /* h1: create on thread A, resume on B, finish on C */
  {
    HJob j;
    HANDLE th;
    lua_State *Lr = newstate_std();
    double ref;
    must(Lr, "R=300");
    luaL_loadstring(Lr, H_CODE); lua_pcall(Lr, 0, 1, 0); ref = lua_tonumber(Lr, -1); lua_close(Lr);
    memset(&j, 0, sizeof(j));
    j.step = 0; th = CreateThread(NULL, 0, h_step, &j, 0, NULL); WaitForSingleObject(th, INFINITE);
    R("H h1 thread A: created+ran, st=%s (expected YIELD from hook)", stname(j.st));
    j.step = 1; th = CreateThread(NULL, 0, h_step, &j, 0, NULL); WaitForSingleObject(th, INFINITE);
    R("H h1 thread B: resumed, st=%s result=%.17g ref=%.17g %s", stname(j.st), j.result, ref, (j.st == 0 && j.result == ref) ? "CORRECT" : "MISMATCH");
    j.step = 2; th = CreateThread(NULL, 0, h_step, &j, 0, NULL); WaitForSingleObject(th, INFINITE);
    R("H h1 thread C: same state, new JIT code + re-run of old traces, st=%s result=%.17g %s", stname(j.st), j.result, (j.st == 0 && j.result == ref) ? "CORRECT" : "MISMATCH");
    lua_close(j.L);
    {
      /* run thread A/B/C again with a different reuse pattern */
      HWork w; memset(&w, 0, sizeof(w)); w.iters = 1; w.expect = ref;
      th = CreateThread(NULL, 0, h_worker, &w, 0, NULL); WaitForSingleObject(th, INFINITE);
      R("H h1 fresh state on new thread bad=%d", w.bad);
    }
    /* h2: N independent states concurrently */
    {
      HWork *ws = (HWork *)calloc(nthreads, sizeof(HWork));
      HANDLE *hs = (HANDLE *)calloc(nthreads, sizeof(HANDLE));
      int i, bad = 0;
      double t0 = now_ms();
      for (i = 0; i < nthreads; i++) { ws[i].id = i; ws[i].iters = 5; ws[i].expect = ref; hs[i] = CreateThread(NULL, 0, h_worker, &ws[i], 0, NULL); }
      for (i = 0; i < nthreads; i++) { WaitForSingleObject(hs[i], INFINITE); bad += ws[i].bad; }
      R("H h2 %d threads x 5 states each, concurrent JIT workloads: bad=%d ms=%.0f", nthreads, bad, now_ms() - t0);
      free(ws); free(hs);
    }
  }
  /* h3: async lua_sethook race stress */
  {
    HSWork *ws = (HSWork *)calloc(nthreads, sizeof(HSWork));
    HANDLE *hs = (HANDLE *)calloc(nthreads, sizeof(HANDLE));
    int i;
    long runs = 0, intr = 0, other = 0, shots = 0;
    double t0 = now_ms();
    unsigned rng = 12345;
    hs_stop = 0;
    for (i = 0; i < nthreads; i++) { ws[i].id = i; hs[i] = CreateThread(NULL, 0, hs_worker, &ws[i], 0, NULL); }
    while (now_ms() - t0 < stress_secs * 1000.0) {
      for (i = 0; i < nthreads; i++) {
        rng = rng * 1103515245u + 12345u;
        if ((rng >> 16) % 4 == 0) {
          InterlockedExchange(&ws[i].phase, 1);
          {
            lua_State *L = (lua_State *)ws[i].L;
            if (L) { lua_sethook(L, hs_hook, LUA_MASKCOUNT, 1); shots++; }
          }
          InterlockedExchange(&ws[i].phase, 0);
        }
      }
      Sleep(1 + (rng >> 20) % 7);
    }
    hs_stop = 1;
    /* make sure everyone exits: keep interrupting until done */
    for (;;) {
      int alive = 0;
      for (i = 0; i < nthreads; i++) {
        if (WaitForSingleObject(hs[i], 0) != WAIT_OBJECT_0) {
          alive++;
          InterlockedExchange(&ws[i].phase, 1);
          { lua_State *L = (lua_State *)ws[i].L; if (L) lua_sethook(L, hs_hook, LUA_MASKCOUNT, 1); }
          InterlockedExchange(&ws[i].phase, 0);
        }
      }
      if (!alive) break;
      if (now_ms() - t0 > (stress_secs + 10) * 1000.0) { R("H h3 STUCK workers=%d (hung, uninterruptible)", alive); break; }
      Sleep(10);
    }
    for (i = 0; i < nthreads; i++) { runs += ws[i].runs; intr += ws[i].interrupted; other += ws[i].other; }
    R("H h3 sethook-race stress %d threads %ds: shots=%ld runs=%ld interrupted=%ld other_err=%ld (no crash if you see this)", nthreads, stress_secs, shots, runs, intr, other);
  }
  return 0;
}

/* ------------------------------------------- test i: count hook cost */

static volatile long i_calls;
static void hook_count_noop(lua_State *L, lua_Debug *ar) { (void)L; (void)ar; i_calls++; }

static const struct { const char *n, *code; } IBENCH[] = {
  {"numloop", "local s=0 for i=1,3e8 do s=s+(i%7)*0.5 end return s"},
  {"fib", "local function fib(n) if n<2 then return n end return fib(n-1)+fib(n-2) end return fib(35)"},
  {"table", "local s=0 for r=1,200 do local t={} for i=1,1e5 do t[i]=i end for i=1,#t do s=s+t[i] end end return s"},
  {"string", "local n=0 for r=1,60 do local t={} for i=1,5e4 do t[#t+1]=tostring(i) end n=n+#table.concat(t) end return n"},
  {"sieve", "local n=0 for r=1,30 do local p={} for i=2,1e6 do p[i]=true end for i=2,1000 do if p[i] then for j=i*i,1e6,i do p[j]=false end end end for i=2,1e6 do if p[i] then n=n+1 end end end return n"},
  {"strops", "local n=0 for r=1,3e5 do local s=string.format('%d:%s', r, 'abc') s=s:upper():gsub('A','x') n=n+#s+(s:find('x',1,true) or 0) end return n"},
  {"mandel", "local s=0 for y=0,799 do for x=0,799 do local cr,ci=x/150-1.5,y/150-1 local zr,zi,k=0,0,0 while k<100 and zr*zr+zi*zi<4 do zr,zi=zr*zr-zi*zi+cr,2*zr*zi+ci k=k+1 end s=s+k end end return s"},
  {NULL, NULL}
};

static int test_i(int argc, char **argv)
{
  static const struct { const char *n; int jit; int count; } CFG[] = {
    {"jit-on  nohook", 1, 0}, {"jit-on  hook1e3", 1, 1000}, {"jit-on  hook1e4", 1, 10000}, {"jit-on  hook1e5", 1, 100000},
    {"jit-off nohook", 0, 0}, {"jit-off hook1e3", 0, 1000}, {"jit-off hook1e4", 0, 10000}, {"jit-off hook1e5", 0, 100000},
    {NULL, 0, 0}
  };
  int b, c;
  const char *only = argc > 2 ? argv[2] : NULL;
  for (b = 0; IBENCH[b].n; b++) {
    if (only && strcmp(only, IBENCH[b].n)) continue;
    for (c = 0; CFG[c].n; c++) {
      lua_State *L = newstate_std();
      double t0, t;
      char err[256];
      int st;
      if (!CFG[c].jit) must(L, "jit.off()");
      i_calls = 0;
      if (CFG[c].count) lua_sethook(L, hook_count_noop, LUA_MASKCOUNT, CFG[c].count);
      t0 = now_ms();
      st = dostr(L, IBENCH[b].code, err, sizeof(err));
      t = now_ms() - t0;
      R("I %-8s %-16s ms=%8.1f hookcalls=%9ld result=%s", IBENCH[b].n, CFG[c].n, t, i_calls, st ? err : lua_tostring(L, -1));
      lua_close(L);
    }
  }
  return 0;
}

/* ------------------------------------------------------- test j: misc */

static volatile LONG j_done;
static lua_State *j_L;
static DWORD WINAPI j_interrupter(LPVOID p)
{
  int i;
  (void)p;
  Sleep(300);
  t_set = now_ms();
  lua_sethook(j_L, hook_err, LUA_MASKCOUNT, 1);
  for (i = 0; i < 400; i++) { if (j_done) return 0; Sleep(5); }
  R("J HUNG (async lua_sethook did not interrupt within 2s) fired=%d", t_fire != 0);
  ExitProcess(3);
  return 0;
}

static int l_mkud(lua_State *L)
{
  lua_newuserdata(L, 16);
  if (luaL_newmetatable(L, "native_handle")) {
    lua_pushcfunction(L, l_cf);  /* harmless C __gc */
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  return 1;
}

static int test_j(int argc, char **argv)
{
  const char *sub = argv[2];
  char err[512];
  int st;
  (void)argc;
  if (!strcmp(sub, "close-suspended")) {
    lua_State *L = newstate_std(), *co = lua_newthread(L);
    luaL_loadstring(co, "local t=setmetatable({}, {}) for i=1,10 do coroutine.yield(i) end");
    st = lua_resume(co, 0);
    R("J close-suspended: coroutine st=%s; calling lua_close", stname(st));
    lua_close(L);
    R("J close-suspended: lua_close returned OK");
    return 0;
  }
  if (!strcmp(sub, "gc-at-close")) {
    lua_State *L = newstate_std();
    must(L, "local p=newproxy(true) getmetatable(p).__gc=function() io.write('R J __gc ran at lua_close\\n') io.flush() end _G.keep=p");
    lua_close(L);
    R("J gc-at-close: lua_close returned");
    return 0;
  }
  /* The following are expected to HANG: use watchdog thread. */
  j_L = newstate_std();
  if (argc > 3 && !strcmp(argv[3], "off")) must(j_L, "jit.off()");
  if (strcmp(sub, "strings") && strcmp(sub, "user-debug-sethook-clears"))
    CreateThread(NULL, 0, j_interrupter, NULL, 0, NULL);
  if (!strcmp(sub, "gc-loop-at-close")) {
    must(j_L, "local p=newproxy(true) getmetatable(p).__gc=function() while true do end end _G.keep=p");
    lua_close(j_L);
    j_done = 1;
    R("J gc-loop-at-close: lua_close returned (interrupted? fired=%d)", t_fire != 0);
    return 0;
  }
  if (!strcmp(sub, "gc-loop-collect")) {
    st = dostr(j_L, "do local p=newproxy(true) getmetatable(p).__gc=function() while true do end end end collectgarbage() return 'collected'", err, sizeof(err));
    j_done = 1;
    R("J gc-loop-collect st=%s %s fired=%d", stname(st), st ? err : "", t_fire != 0);
    return 0;
  }
  if (!strcmp(sub, "gc-loop-alloc")) {
    /* finalizer triggered implicitly by allocation, no collectgarbage */
    st = dostr(j_L, "do local p=newproxy(true) getmetatable(p).__gc=function() while true do end end end local t={} for i=1,1e8 do t[i%1000]={} end return 'done'", err, sizeof(err));
    j_done = 1;
    R("J gc-loop-alloc st=%s %s fired=%d", stname(st), st ? err : "", t_fire != 0);
    return 0;
  }
  if (!strcmp(sub, "native-ud-gc-hijack")) {
    lua_register(j_L, "mkud", l_mkud);
    st = dostr(j_L, "do local u=mkud() getmetatable(u).__gc=function() while true do end end end collectgarbage() return 'collected'", err, sizeof(err));
    j_done = 1;
    R("J native-ud-gc-hijack st=%s %s fired=%d", stname(st), st ? err : "", t_fire != 0);
    return 0;
  }
  if (!strcmp(sub, "jit-attach-loop")) {
    st = dostr(j_L, "jit.attach(function() while true do end end, 'trace') local s=0 for i=1,1e6 do s=s+i end return s", err, sizeof(err));
    j_done = 1;
    R("J jit-attach-loop st=%s %s fired=%d", stname(st), st ? err : "", t_fire != 0);
    return 0;
  }
  if (!strcmp(sub, "user-debug-sethook-clears")) {
    /* Machine uses a permanent hook; user code calls debug.sethook() */
    j_done = 1;  /* not using interrupter here */
    perm_calls = 0;
    lua_sethook(j_L, hook_perm, LUA_MASKCOUNT, 1000);
    st = dostr(j_L, "jit.off() local co=coroutine.create(function() debug.sethook() end) coroutine.resume(co) local s=0 for i=1,1e6 do s=s+i end return s", err, sizeof(err));
    R("J user-debug-sethook-in-coroutine st=%s machine hook now=%p mask=%d perm_calls=%ld", stname(st), (void *)lua_gethook(j_L), lua_gethookmask(j_L), perm_calls);
    return 0;
  }
  if (!strcmp(sub, "strings")) {
    j_done = 1;
    st = dostr(j_L, "local ok,e=pcall(string.rep,'x',2^31) return 'rep 2^31: '..tostring(ok)..' len='..tostring(ok and #e or e)", err, sizeof(err));
    R("J %s", st ? err : lua_tostring(j_L, -1)); lua_settop(j_L, 0);
    st = dostr(j_L, "local t0=now() local ok,e=pcall(string.rep,'x',2^31-1024) local r='rep 2^31-1024: '..tostring(ok)..' '..(ok and #e or tostring(e))..string.format(' %.0f ms', now()-t0) e=nil collectgarbage() return r", err, sizeof(err));
    R("J %s", st ? err : lua_tostring(j_L, -1)); lua_settop(j_L, 0);
    st = dostr(j_L, "collectgarbage() local m0=collectgarbage('count') local t0=now() local t={} for i=1,2e6 do t[i]='k'..i end local t1=now() collectgarbage() return string.format('2e6 unique strings: %.0f ms, mem delta=%.0f MB', t1-t0, (collectgarbage('count')-m0)/1024)", err, sizeof(err));
    R("J %s", st ? err : lua_tostring(j_L, -1)); lua_settop(j_L, 0);
    st = dostr(j_L, "local t0=now() local s=('x'):rep(1e6) local t={} for i=1,2000 do t[i]=s:sub(1,1e6-i) end return string.format('2000 near-identical 1MB strings: %.0f ms', now()-t0)", err, sizeof(err));
    R("J %s", st ? err : lua_tostring(j_L, -1)); lua_settop(j_L, 0);
    return 0;
  }
  if (!strcmp(sub, "hook-inside-gc-finalizer-normal")) {
    /* a long but finite finalizer: does the async hook fire inside it? */
    st = dostr(j_L, "do local p=newproxy(true) getmetatable(p).__gc=function() local t=now() while now()-t<1000 do end end end collectgarbage() local x=0 for i=1,10 do x=x+i end return 'after gc'", err, sizeof(err));
    j_done = 1;
    R("J finalizer-1s-busy st=%s %s fired=%d lat_ms=%.0f (hook delayed until finalizer finished)", stname(st), st ? err : lua_tostring(j_L, -1), t_fire != 0, t_fire ? t_fire - t_set : -1.0);
    return 0;
  }
  R("bad j sub");
  return 2;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: rt a|b|c|d|e|f|g|gfuzz|h|i|j ...\n"); return 2; }
  switch (argv[1][0]) {
  case 'a': return test_a(argc, argv);
  case 'b': return test_b(argc, argv);
  case 'c': return test_c(argc, argv);
  case 'd': return test_d(argc, argv);
  case 'e': return test_e(argc, argv);
  case 'f': return test_f(argc, argv);
  case 'g': return argv[1][1] == 'f' ? test_gfuzz(argc, argv) : test_g(argc, argv);
  case 'h': return test_h(argc, argv);
  case 'i': return test_i(argc, argv);
  case 'j': return test_j(argc, argv);
  }
  return 2;
}
