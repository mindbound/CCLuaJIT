/*
** pharness.c -- interrupting long-running C library functions (patch 07).
**
** A second thread calls luaJIT_interrupt() while the VM thread runs a
** pathological pattern match (or table.sort) in C. The VM thread keeps a
** count hook installed permanently (count HOOKN, default 1e4), as in the
** host protocol of reports/preempt-patch.md section 7.
**
** Usage: pharness <mode> <case|list> [delay_ms]
**   modes (C = case runs in C when the request arrives):
**   error     hook raises "interrupted"; host lua_pcall must return it.
**   pcall     case wrapped in a Lua pcall: the error must be catchable,
**             the chunk then returns normally and the state stays usable.
**   hard      hard abort: the hook re-arms itself with count 1 and raises
**             errors; the case runs in 'while true do pcall(case) end'.
**   hardyield like hard, but runs in a coroutine and yields when possible.
**   pause     finite case in a coroutine, request every 2 ms; the hook
**             yields when lua_isyieldable(), else re-arms the request with
**             luaJIT_interrupt(). Result must equal the reference.
**   yieldbad  hook calls lua_yield() without checking: inside C this must
**             become a catchable "attempt to yield across C-call boundary".
**   gcstress  finite case, request every 1 ms; hook grows the Lua stack,
**             pushes garbage and runs a full GC, then returns. Result must
**             equal the reference (checks MatchState/luaL_Buffer safety).
** Build: gcc -O2 -I<luajit>/src pharness.c <luajit>/src/libluajit.a -lwinmm
*/
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"   /* Internal: only to sample G(L)->vmstate. */

typedef struct Case { const char *name; const char *src; } Case;

/* Endless (or practically endless) cases. */
static const Case cases[] = {
  {"find_backtrack", "return string.find(string.rep('a', 40), string.rep('a*', 20) .. 'b')"},
  {"match_backtrack", "return string.match(string.rep('a', 40), string.rep('a*', 20) .. 'b')"},
  {"gmatch_backtrack", "for m in string.gmatch(string.rep('a', 40), string.rep('a*', 20) .. 'b') do end"},
  {"gsub_backtrack", "return string.gsub(string.rep('a', 40), string.rep('a*', 20) .. 'b', 'x')"},
  {"captures_backtrack", "return string.find(string.rep('a', 40), string.rep('(a*)', 15) .. 'b')"},
  {"minexpand_lazy", "return string.find(string.rep('a', 1e4), '.-.-.-.-.-b')"},
  {"gsub_quadratic", "return string.gsub(string.rep('a', 1e6), '.-b', '')"},
  {"gmatch_quadratic", "local n = 0 for m in string.gmatch(string.rep('a', 1e6), 'a-b') do n = n + 1 end return n"},
  {"balance_quadratic", "return string.find(string.rep('(', 1e6), '%b()')"},
  {"maxexpand_scan_200MB", "return string.find(string.rep('a', 2e8), '.*b')"},
  {"frontier_backtrack", "return string.find(string.rep('a', 40), '%f[a]' .. string.rep('a*', 20) .. 'b')"},
  {"anchored_backtrack", "return string.find(string.rep('a', 40), '^' .. string.rep('a*', 20) .. 'b$')"},
  {"sort_killer_100k",
   "local a, n = {}, 0 for l in io.lines(KILLER) do n = n + 1 a[n] = tonumber(l) end\n"
   "table.sort(a) return a[1]"},
  {"gsub_fn_repl_backtrack",
   "return string.gsub(string.rep('a', 40), string.rep('a*', 20) .. 'b', function(x) return x end)"},
  /* Q_: stock lj_str_find (plain find) is O(n*m) (about 90 s here). It is not
  ** polled (it also runs on traces); patch 07 bounds it with Two-Way instead,
  ** so these must simply finish quickly. */
  {"Q_plain_find_quadratic",
   "return string.find(string.rep('a', 4e6), string.rep('a', 2e6) .. 'b', 1, true)"},
  {"Q_plain_find_periodic",
   "local n = string.rep('a', 1e6) .. 'b' .. string.rep('a', 1e6) return string.find(string.rep('a', 4e6), n, 1, true)"},
  {"Q_plain_find_jit",
   "local h, n = string.rep('a', 4e6), string.rep('a', 2e6) .. 'b' local r\n"
   "local subj = {} for i = 1, 99 do subj[i] = 'xyz' end subj[100] = h\n"
   "for i = 1, 100 do r = string.find(subj[i], n, 1, true) end return r"},
  {NULL, NULL}
};

/* Finite cases (about 0.3-2 s each) for the pause and gcstress modes. */
static const Case fcases[] = {
  {"f_gsub",
   "local s = string.rep(string.rep('a', 18) .. 'c', 4) .. string.rep('a', 18) .. 'b'\n"
   "local r, n = string.gsub(s, string.rep('a*', 9) .. 'b', '<%0>')\n"
   "return r .. '|' .. n"},
  {"f_gsub_captures",
   "local s = string.rep(string.rep('a', 18) .. 'c', 4) .. string.rep('a', 18) .. 'b' .. string.rep('xy', 300)\n"
   "local r, n = string.gsub(s, string.rep('(a*)', 9) .. 'b', '%9%3%1')\n"
   "return r .. '|' .. n"},
  {"f_find",
   "return table.concat({string.find(string.rep('a', 19) .. 'b', string.rep('(a*)', 11) .. 'c')}, ',')"
   " .. '|' .. table.concat({string.find(string.rep('a', 19) .. 'b', string.rep('(a*)', 11) .. 'b')}, ',')"},
  {"f_gmatch",
   "local t = {} for a, b in string.gmatch(string.rep(string.rep('a', 14) .. 'b', 8), '(a*)a*a*a*a*a*a*a*(b)') do\n"
   "  t[#t + 1] = #a .. b end\n"
   "local u = {} for w in string.gmatch(string.rep('a', 22), string.rep('a*', 9) .. 'c') do u[#u+1] = w end\n"
   "return table.concat(t, ',') .. '|' .. #u"},
  {"f_balance",
   "local s = string.rep('(', 30000) .. string.rep(')', 15000) .. 'x'\n"
   "local n = 0 for m in string.gmatch(s, '%b()') do n = n + #m end\n"
   "return n .. '|' .. tostring(string.find(s, '%b()x'))"},
  {"f_sort",
   "local a, n = {}, 0 for l in io.lines(KILLER32) do n = n + 1 a[n] = tonumber(l) end\n"
   "table.sort(a) local h = 0 for i = 1, n do h = (h * 31 + a[i]) % 1000000007 end return h"},
  {NULL, NULL}
};

enum { M_ERROR, M_PCALL, M_HARD, M_HARDYIELD, M_PAUSE, M_YIELDBAD, M_GCSTRESS };
static int mode;
static lua_State *gL;
static int delay_ms = 300;
static int hookn = 10000;
static volatile LONG pending, fired, done, stop;
static volatile LONGLONG t_req, t_fire;
static volatile LONG n_hook_c, n_hook_c_yieldable, n_hook_lua, n_yield, n_defer, n_stress;
static LARGE_INTEGER freq;

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

static void hook_hard(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (mode == M_HARDYIELD && lua_isyieldable(L)) lua_yield(L, 0);
  luaL_error(L, "hard abort");
}

static int top_is_c(lua_State *L)
{
  lua_Debug a;
  if (!lua_getstack(L, 0, &a)) return 0;
  if (!lua_getinfo(L, "S", &a)) return 0;
  return a.what[0] == 'C';
}

static void hook_permanent(lua_State *L, lua_Debug *ar)
{
  int inc, y;
  (void)ar;
  if (!InterlockedExchange(&pending, 0)) return;  /* Periodic call. */
  inc = top_is_c(L); y = lua_isyieldable(L);
  if (inc) { InterlockedIncrement(&n_hook_c); if (y) InterlockedIncrement(&n_hook_c_yieldable); }
  else InterlockedIncrement(&n_hook_lua);
  if (!fired) t_fire = qpc();
  InterlockedIncrement(&fired);
  switch (mode) {
  case M_ERROR: case M_PCALL:
    luaL_error(L, "interrupted");
    break;
  case M_HARD: case M_HARDYIELD:
    lua_sethook(L, hook_hard, LUA_MASKCOUNT, 1);  /* VM thread: safe. */
    hook_hard(L, ar);
    break;
  case M_PAUSE:
    if (y) { InterlockedIncrement(&n_yield); lua_yield(L, 0); return; }
    InterlockedIncrement(&n_defer);
    InterlockedExchange(&pending, 1);  /* Re-arm: serviced at a later poll/boundary. */
    luaJIT_interrupt(L);
    return;
  case M_YIELDBAD:
    lua_yield(L, 0);  /* Inside C: must raise an error, not corrupt the state. */
    return;
  case M_GCSTRESS: {
    int i;
    InterlockedIncrement(&n_stress);
    luaL_checkstack(L, 4000, "stress");  /* Forces reallocation of the Lua stack. */
    for (i = 0; i < 4000; i++) lua_pushfstring(L, "garbage %d", i);
    lua_pop(L, 4000);
    lua_gc(L, LUA_GCCOLLECT, 0);
    return;
    }
  }
}

static void request(void)
{
  InterlockedExchange(&pending, 1);
  luaJIT_interrupt(gL);
}

/* One-shot interrupter. */
static DWORD WINAPI interrupter(LPVOID p)
{
  char b1[32], b2[32]; int32_t st;
  (void)p;
  Sleep(delay_ms);
  if (done) return 0;
  st = G(gL)->vmstate;
  t_req = qpc();
  request();
  printf("  state at request: %s\n", vmstate_str(st, b1));
  while (!fired && !done && us(qpc() - t_req) < 3e6) Sleep(1);
  if (!fired && !done) {
    printf("  RESULT HANG: hook did not run within 3 s (vmstate now %s)\n", vmstate_str(G(gL)->vmstate, b2));
    fflush(stdout);
    ExitProcess(3);
  }
  return 0;
}

/* Periodic interrupter (pause, gcstress). */
static DWORD WINAPI periodic(LPVOID p)
{
  int ms = (int)(INT_PTR)p;
  while (!stop) { Sleep(ms); if (stop) break; request(); }
  return 0;
}

static lua_State *newstate(void)
{
  lua_State *L = luaL_newstate();
  char buf[600];
  const char *dir = getenv("PH_DIR") ? getenv("PH_DIR") : ".";
  luaL_openlibs(L);
  snprintf(buf, sizeof(buf), "%s/lua/killer100k.txt", dir);
  lua_pushstring(L, buf); lua_setglobal(L, "KILLER");
  snprintf(buf, sizeof(buf), "%s/lua/killer32k.txt", dir);
  lua_pushstring(L, buf); lua_setglobal(L, "KILLER32");
  lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);  /* VM thread, once. */
  return L;
}

static const Case *findcase(const Case *t, const char *n)
{
  for (; t->name; t++) if (!strcmp(t->name, n)) return t;
  return NULL;
}

/* Check that the state still works after an interrupt. */
static void usable(lua_State *L)
{
  int st;
  lua_settop(L, 0);
  st = luaL_dostring(L, "local t = {} for w in ('a1 b22 c333'):gmatch('%a(%d+)') do t[#t+1] = w end\n"
			"return table.concat(t, ',') .. ' ' .. (('hello world'):gsub('o', '0'))");
  printf("  state usable afterwards: %s\n", st ? lua_tostring(L, -1) : lua_tostring(L, -1));
}

static int run_oneshot(const Case *c)
{
  lua_State *L; HANDLE th; int st; LONGLONG t0; char buf[4096];
  L = gL = newstate();
  printf("CASE %s [%s]\n", c->name, mode == M_ERROR ? "error" : mode == M_PCALL ? "pcall" :
	 mode == M_HARD ? "hard" : mode == M_HARDYIELD ? "hardyield" : "yieldbad");
  if (mode == M_PCALL)
    snprintf(buf, sizeof(buf), "local ok, e = pcall(function() %s end) return 'pcall returned ' .. tostring(ok) .. ', ' .. tostring(e)", c->src);
  else if (mode == M_HARD || mode == M_HARDYIELD)
    snprintf(buf, sizeof(buf), "while true do pcall(function() %s end) end", c->src);
  else
    snprintf(buf, sizeof(buf), "%s", c->src);
  if (luaL_loadstring(L, buf)) { printf("  load error %s\n", lua_tostring(L, -1)); return 1; }
  th = CreateThread(NULL, 0, interrupter, NULL, 0, NULL);
  t0 = qpc();
  if (mode == M_HARDYIELD || mode == M_YIELDBAD) {
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -2); lua_xmove(L, co, 1);
    st = lua_resume(co, 0);
    done = 1;
    if (!fired && st == 0 && !strncmp(c->name, "Q_", 2)) {
      printf("  RESULT OK: finished in %.1f ms before the request (no longer pathological)\n", us(qpc() - t0) / 1000);
      WaitForSingleObject(th, INFINITE);
      lua_close(L);
      return 0;
    }
    printf("  RESULT %s: lua_resume -> %d (%s) msg=\"%s\" latency=%.1f us hook-in-C=%ld (yieldable %ld) hook-in-Lua=%ld\n",
	   (mode == M_HARDYIELD ? st == LUA_YIELD :
	    (st == LUA_ERRRUN && lua_isstring(co, -1) && strstr(lua_tostring(co, -1), "C-call boundary"))) ? "OK" : "FAIL",
	   st, st == LUA_YIELD ? "yield" : st ? "error" : "ok",
	   st && st != LUA_YIELD && lua_isstring(co, -1) ? lua_tostring(co, -1) : "",
	   us(t_fire - t_req), n_hook_c, n_hook_c_yieldable, n_hook_lua);
    WaitForSingleObject(th, INFINITE);
    if (mode == M_YIELDBAD) { lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn); usable(L); }
    lua_close(L);
    return 0;
  }
  st = lua_pcall(L, 0, 1, 0);
  done = 1;
  {
    int ok;
    const char *msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    if (!fired && st == 0 && !strncmp(c->name, "Q_", 2)) {
      printf("  RESULT OK: finished in %.1f ms before the request (no longer pathological)\n", us(qpc() - t0) / 1000);
      WaitForSingleObject(th, INFINITE);
      lua_close(L);
      return 0;
    }
    if (mode == M_ERROR) ok = st == LUA_ERRRUN && strstr(msg, "interrupted");
    else if (mode == M_PCALL) ok = st == 0 && strstr(msg, "pcall returned false") && strstr(msg, "interrupted");
    else ok = st == LUA_ERRRUN && strstr(msg, "hard abort");
    printf("  RESULT %s: status=%d msg=\"%s\" latency=%.1f us (ran %.0f ms) hook-in-C=%ld (yieldable %ld) hook-in-Lua=%ld\n",
	   ok ? "OK" : "FAIL", st, msg, us(t_fire - t_req), us(qpc() - t0) / 1000,
	   n_hook_c, n_hook_c_yieldable, n_hook_lua);
  }
  WaitForSingleObject(th, INFINITE);
  lua_sethook(L, hook_permanent, LUA_MASKCOUNT, hookn);
  usable(L);
  lua_close(L);
  fflush(stdout);
  return 0;
}

static int run_periodic(const Case *c)
{
  lua_State *L, *co; HANDLE th; int st, resumes = 0; LONGLONG t0, t1, t2;
  char ref[8192], res[8192];
  L = gL = newstate();
  printf("CASE %s [%s]\n", c->name, mode == M_PAUSE ? "pause" : "gcstress");
  /* Reference run without requests. */
  t0 = qpc();
  if (luaL_dostring(L, c->src)) { printf("  RESULT FAIL: reference error %s\n", lua_tostring(L, -1)); return 1; }
  t1 = qpc();
  snprintf(ref, sizeof(ref), "%s", lua_tostring(L, -1));
  lua_pop(L, 1);
  /* Interrupted run inside a coroutine. */
  co = lua_newthread(L);
  if (luaL_loadstring(co, c->src)) return 1;
  th = CreateThread(NULL, 0, periodic, (LPVOID)(INT_PTR)(mode == M_PAUSE ? 2 : 1), 0, NULL);
  st = lua_resume(co, 0); resumes++;
  while (st == LUA_YIELD) { st = lua_resume(co, 0); resumes++; }
  t2 = qpc();
  stop = 1; WaitForSingleObject(th, INFINITE);
  if (st) { printf("  RESULT FAIL: status %d: %s\n", st, lua_tostring(co, -1)); return 1; }
  snprintf(res, sizeof(res), "%s", lua_tostring(co, -1));
  printf("  reference %.0f ms, interrupted run %.0f ms, resumes=%d hook-runs=%ld in-C=%ld (yieldable %ld) in-Lua=%ld"
	 " yields=%ld deferred=%ld stress=%ld\n",
	 us(t1 - t0) / 1000, us(t2 - t1) / 1000, resumes, fired, n_hook_c, n_hook_c_yieldable, n_hook_lua,
	 n_yield, n_defer, n_stress);
  printf("  result: %.60s%s (len %d)\n", res, strlen(res) > 60 ? "..." : "", (int)strlen(res));
  printf("  RESULT %s: result %s reference%s\n", (!strcmp(ref, res) && n_hook_c_yieldable == 0) ? "OK" : "FAIL",
	 strcmp(ref, res) ? "DIFFERS from" : "matches",
	 n_hook_c_yieldable ? " (BUG: C-context hook saw yieldable)" : "");
  lua_close(L);
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv)
{
  static const char *modes[] = {"error", "pcall", "hard", "hardyield", "pause", "yieldbad", "gcstress", NULL};
  const Case *c;
  int i;
  QueryPerformanceFrequency(&freq);
  timeBeginPeriod(1);
  if (getenv("HOOKN")) hookn = atoi(getenv("HOOKN"));
  if (argc < 3) { fprintf(stderr, "usage: pharness <mode> <case|list> [delay_ms]\n"); return 2; }
  for (mode = -1, i = 0; modes[i]; i++) if (!strcmp(modes[i], argv[1])) mode = i;
  if (mode < 0) { fprintf(stderr, "bad mode\n"); return 2; }
  if (argc > 3) delay_ms = atoi(argv[3]);
  if (!strcmp(argv[2], "list")) {
    for (c = (mode == M_PAUSE || mode == M_GCSTRESS) ? fcases : cases; c->name; c++) puts(c->name);
    return 0;
  }
  if (mode == M_PAUSE || mode == M_GCSTRESS) {
    if (!(c = findcase(fcases, argv[2]))) { fprintf(stderr, "unknown case\n"); return 2; }
    return run_periodic(c);
  }
  if (!(c = findcase(cases, argv[2]))) { fprintf(stderr, "unknown case\n"); return 2; }
  return run_oneshot(c);
}
