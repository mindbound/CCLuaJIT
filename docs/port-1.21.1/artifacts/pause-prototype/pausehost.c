/*
** pausehost.c - CC:T-like host for testing the pause token protocol.
**
** Usage: pausehost <test.lua> [case ...]
**        pausehost --native-nest     (crash demo: native coroutine nesting)
**        pausehost --wrapper-nest    (wrapper nesting with MAXDEPTH=1e9)
** Environment:
**   PAUSE_MIN_US, PAUSE_MAX_US  pause request interval (default 300..1500 us)
**   NAIVE=1                     control: plain hook-yield, native coroutine lib
**   JIT=0                       interpreter only
**   HOOKN                       count hook period (default 10000)
**   MAXDEPTH                    coroutine nesting limit (default 200)
**   STACK_KB                    VM thread stack reservation (default 1024)
**
** The test file returns { cases = { name = {main=, events=, ...}, ... },
** order = { names... } }. For every case the host runs a reference without
** pause requests, then the same case with a second thread requesting a pause
** every PAUSE_MIN_US..PAUSE_MAX_US, and compares results, host-level yields
** and delivered events. Event delivery follows CobaltLuaMachine.handleEvent:
** resume top with (name, args...); the first yielded value, if a string, is
** the filter; non-matching events (except "terminate") are consumed without
** running Lua; on PAUSE resume with no values and keep the filter.
*/
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"

#include "cclj_co.h"

static int opt_pmin = 300, opt_pmax = 1500, opt_naive = 0, opt_jit = 1;
static int opt_poke_us = 250, opt_rearm = 0, opt_defer_hookn = 0, opt_ckpt = 0;
static int opt_hookn = 10000, opt_maxdepth = 200, opt_stack_kb = 1024, opt_floor_kb = 256;
static char test_dir[MAX_PATH];

/* -- Requester thread ------------------------------------------------------ */
static CRITICAL_SECTION g_mlock;     /* Guards g_cur against cclj_close(). */
static CcljMachine *volatile g_cur;
static volatile LONG g_pauses_on;
static volatile LONG g_pmin, g_pmax;
static volatile LONG g_oneshot_what;
static volatile LONGLONG g_oneshot_at;
static volatile LONG g_stop;
static volatile LONG g_nreq;

static unsigned rng_state = 12345;
static unsigned rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static THREAD_FN(requester)
{
  LONGLONG next = 0, next_poke = 0;
  (void)p;
  while (!g_stop) {
    LONGLONG now = cclj_qpc();
    /* Take the lock only when something is due: a lock word written in a
    ** tight loop on another core slowed the VM thread down 3x on Linux. */
    if (!((g_pauses_on && now >= next) || (opt_poke_us > 0 && now >= next_poke) ||
	  (g_oneshot_what && now >= g_oneshot_at))) {
      YieldProcessor();
      continue;
    }
    EnterCriticalSection(&g_mlock);
    if (g_cur && g_cur->running) {
      if (g_pauses_on && now >= next) {
	cclj_request(g_cur, CCLJ_REQ_PAUSE);
	InterlockedIncrement(&g_nreq);
	{
	  LONG span = g_pmax - g_pmin;
	  double us = g_pmin + (span > 0 ? (double)(rng() % (unsigned)span) : 0);
	  next = now + (LONGLONG)(us / cclj_qpc_us(1000000) * 1e6);
	}
      }
      if (opt_poke_us > 0 && now >= next_poke) {  /* Deferred-pause ticker. */
	cclj_poke(g_cur);
	next_poke = now + (LONGLONG)(opt_poke_us / cclj_qpc_us(1000000) * 1e6);
      }
      if (g_oneshot_what && now >= g_oneshot_at) {
	cclj_request(g_cur, g_oneshot_what);
	g_oneshot_what = 0;
      }
    }
    LeaveCriticalSection(&g_mlock);
    YieldProcessor();
  }
  return 0;
}

/* -- host.* functions for test scripts -------------------------------------- */
static CcljMachine *M(lua_State *L) { return (CcljMachine *)lua_touserdata(L, lua_upvalueindex(1)); }

static int h_now(lua_State *L) { lua_pushnumber(L, cclj_qpc_us(cclj_qpc())); return 1; }
static int h_region(lua_State *L) { M(L)->region = (int)luaL_checkinteger(L, 1); return 0; }
static int h_depth(lua_State *L) { lua_pushinteger(L, M(L)->depth); return 1; }
static int h_tag(lua_State *L)
{
  CcljMachine *m = M(L);
  if (m->ntags < CCLJ_MAX_TAGS) {
    m->tag_co[m->ntags] = L;
    m->tag_level[m->ntags] = (int)luaL_checkinteger(L, 1);
    m->ntags++;
  }
  return 0;
}
static int h_stack_used(lua_State *L)
{
  char probe;
  lua_pushinteger(L, (lua_Integer)(M(L)->stack_base - (uintptr_t)&probe));
  return 1;
}
static int h_request(lua_State *L)
{
  const char *w = luaL_checkstring(L, 1);
  double delay = luaL_optnumber(L, 2, 0);
  int what = !strcmp(w, "hard") ? CCLJ_REQ_HARD : !strcmp(w, "soft") ? CCLJ_REQ_SOFT : CCLJ_REQ_PAUSE;
  g_oneshot_at = cclj_qpc() + (LONGLONG)(delay * (cclj_qpc_us(1000000) > 0 ? 1e6 / cclj_qpc_us(1000000) : 1));
  g_oneshot_what = what;
  return 0;
}
static int h_pauses(lua_State *L)
{
  g_pauses_on = lua_toboolean(L, 1);
  if (lua_gettop(L) >= 3) { g_pmin = (LONG)lua_tointeger(L, 2); g_pmax = (LONG)lua_tointeger(L, 3); }
  return 0;
}
static int h_print(lua_State *L)
{
  int i, n = lua_gettop(L);
  for (i = 1; i <= n; i++) {
    const char *s;
    lua_getglobal(L, "tostring"); lua_pushvalue(L, i); lua_call(L, 1, 1);
    s = lua_tostring(L, -1);
    fputs(i > 1 ? "\t" : "", stdout); fputs(s ? s : "?", stdout);
    lua_pop(L, 1);
  }
  fputs("\n", stdout); fflush(stdout);
  return 0;
}
static int h_include(lua_State *L)
{
  char path[MAX_PATH * 2];
  snprintf(path, sizeof(path), "%s/lib/%s.lua", test_dir, luaL_checkstring(L, 1));
  if (luaL_loadfile(L, path)) return lua_error(L);
  lua_call(L, 0, LUA_MULTRET);
  return lua_gettop(L) - 1;
}
static void setnum(lua_State *L, const char *k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
static int h_stats(lua_State *L)
{
  CcljMachine *m = M(L);
  int i;
  lua_newtable(L);
  setnum(L, "npause", m->npause);
  setnum(L, "nhookpause", m->nhookpause);
  setnum(L, "nprop", m->nprop);
  setnum(L, "nfailprop", m->nfailprop);
  setnum(L, "ndefer", m->ndefer);
  setnum(L, "nsoft", m->nsoft);
  setnum(L, "nresume", m->nresume);
  setnum(L, "depth_hw", m->depth_hw);
  setnum(L, "ndepth_err", m->ndepth_err);
  setnum(L, "nfloor_err", m->nfloor_err);
  setnum(L, "ncheckpoint", m->ncheckpoint);
  setnum(L, "max_defer_us", m->max_defer_us);
  setnum(L, "max_pause_lat_us", m->max_pause_lat_us);
  setnum(L, "avg_pause_lat_us", m->nhookpause ? m->sum_pause_lat_us / m->nhookpause : 0);
  setnum(L, "stack_hw", m->min_sp == ~(uintptr_t)0 ? 0 : (double)(m->stack_base - m->min_sp));
  lua_newtable(L);
  for (i = 0; i < CCLJ_MAX_REGIONS; i++) { lua_pushnumber(L, m->rpause[i]); lua_rawseti(L, -2, i); }
  lua_setfield(L, -2, "rpause");
  lua_newtable(L);
  for (i = 0; i < CCLJ_MAX_REGIONS; i++) { lua_pushnumber(L, m->rdefer[i]); lua_rawseti(L, -2, i); }
  lua_setfield(L, -2, "rdefer");
  lua_newtable(L);
  for (i = 0; i < m->ntags; i++) {
    lua_rawgeti(L, -1, m->tag_level[i]);
    lua_pushnumber(L, lua_tonumber(L, -1) + m->tagpause[i]);
    lua_rawseti(L, -3, m->tag_level[i]);
    lua_pop(L, 1);
  }
  lua_setfield(L, -2, "tagpause");
  return 1;
}

static void open_host(lua_State *L, CcljMachine *m)
{
  static const luaL_Reg fns[] = {
    {"now", h_now}, {"region", h_region}, {"depth", h_depth}, {"tag", h_tag},
    {"stack_used", h_stack_used}, {"request", h_request}, {"pauses", h_pauses},
    {"print", h_print}, {"include", h_include}, {"stats", h_stats}, {NULL, NULL}
  };
  const luaL_Reg *r;
  lua_newtable(L);
  for (r = fns; r->name; r++) {
    lua_pushlightuserdata(L, m); lua_pushcclosure(L, r->func, 1); lua_setfield(L, -2, r->name);
  }
  lua_getfield(L, LUA_REGISTRYINDEX, "cclj.native_resume"); lua_setfield(L, -2, "native_resume");
  lua_getfield(L, LUA_REGISTRYINDEX, "cclj.native_wrap"); lua_setfield(L, -2, "native_wrap");
  lua_pushboolean(L, m->naive); lua_setfield(L, -2, "naive");
  lua_pushinteger(L, m->max_depth); lua_setfield(L, -2, "max_depth");
  lua_setglobal(L, "host");
}

/* -- Running one case --------------------------------------------------------- */
typedef struct Result {
  int kind;            /* 0 finished, 1 error, 2 timeout, 3 stall, 4 setup error */
  char text[4096];     /* Results (tab separated) or error message. */
  long nok, nevents, ndropped, npause;
  double ms, abort_lat_us;
  char check[1024];    /* Output of case.check, if any. */
  int check_ok;
} Result;

static const char *kindname[] = {"FINISHED", "ERROR", "TIMEOUT", "STALL", "SETUP"};

static void values_to_text(lua_State *co, char *buf, size_t sz)
{
  int i, n = lua_gettop(co);
  size_t pos = 0;
  buf[0] = 0;
  for (i = 1; i <= n && pos + 2 < sz; i++) {
    const char *s; char tmp[64];
    switch (lua_type(co, i)) {
    case LUA_TSTRING: case LUA_TNUMBER: s = lua_tostring(co, i); break;
    case LUA_TBOOLEAN: s = lua_toboolean(co, i) ? "true" : "false"; break;
    case LUA_TNIL: s = "nil"; break;
    default: snprintf(tmp, sizeof(tmp), "<%s>", luaL_typename(co, i)); s = tmp;
    }
    pos += snprintf(buf + pos, sz - pos, "%s%s", i > 1 ? "\t" : "", s);
  }
}

static int get_case(lua_State *L, const char *file, const char *name)
{
  if (luaL_loadfile(L, file) || lua_pcall(L, 0, 1, 0)) return 0;
  lua_getfield(L, -1, "cases");
  lua_getfield(L, -1, name);
  if (!lua_istable(L, -1)) { lua_pushfstring(L, "no case %s", name); return 0; }
  lua_replace(L, -3);
  lua_pop(L, 1);
  return 1;  /* case table on top */
}

static int case_int(lua_State *L, int idx, const char *k, int def)
{
  int v;
  lua_getfield(L, idx, k);
  v = lua_isnil(L, -1) ? def : lua_isboolean(L, -1) ? lua_toboolean(L, -1) : (int)lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}

static void run_case(const char *file, const char *name, int pauses, Result *res)
{
  CcljMachine *m = cclj_new(opt_naive, opt_maxdepth);
  lua_State *L = m->L, *top;
  int st, ci, evfn = 0, filter_ref = LUA_NOREF, hard_while_paused;
  long evi = 0;
  char filter[256]; int has_filter = 0;
  LONGLONG t0;
  plat_uptr lo, hi;
  char probe;
  memset(res, 0, sizeof(*res));
  plat_stack_limits(&lo, &hi);
  m->stack_base = (uintptr_t)hi;
  m->defer_rearm = opt_rearm;
  m->defer_hookn = opt_defer_hookn;
  m->stack_floor = (uintptr_t)lo + (uintptr_t)opt_floor_kb * 1024;  /* Recomputed per handleEvent in a real host. */
  (void)probe;
  luaL_openlibs(L);
  m->use_checkpoint = opt_ckpt;
  cclj_install(m, opt_hookn);
  open_host(L, m);
  if (!opt_jit) luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
  if (!get_case(L, file, name)) {
    res->kind = 4; snprintf(res->text, sizeof(res->text), "%s", lua_tostring(L, -1));
    goto done;
  }
  ci = lua_gettop(L);
  lua_getfield(L, ci, "events");
  if (lua_isfunction(L, -1)) evfn = luaL_ref(L, LUA_REGISTRYINDEX); else lua_pop(L, 1);
  hard_while_paused = case_int(L, ci, "hard_while_paused", 0);
  if (!case_int(L, ci, "pauses", 1)) pauses = 0;
  m->max_depth = case_int(L, ci, "max_depth", opt_maxdepth);
  m->stack_floor = (uintptr_t)lo + (uintptr_t)case_int(L, ci, "floor_kb", opt_floor_kb) * 1024;
  g_pmin = case_int(L, ci, "pause_min_us", opt_pmin);
  g_pmax = case_int(L, ci, "pause_max_us", opt_pmax);
  top = lua_newthread(L);
  m->top = top;
  lua_setfield(L, LUA_REGISTRYINDEX, "cclj.top");  /* Anchor. */
  lua_getfield(L, ci, "main");
  lua_xmove(L, top, 1);
  lua_settop(L, 0);
  (void)filter_ref;
  g_oneshot_what = 0;
  g_pauses_on = pauses;
  EnterCriticalSection(&g_mlock); g_cur = m; LeaveCriticalSection(&g_mlock);
  t0 = cclj_qpc();
  st = cclj_resume_top(m, 0);
  for (;;) {
    if (m->aborting) {
      res->kind = 2;
      res->abort_lat_us = cclj_qpc_us(cclj_qpc() - m->t_req_hard);
      snprintf(res->text, sizeof(res->text), "hard abort, status %d", st);
      break;
    }
    if (st == LUA_YIELD && m->pausing) {
      m->npause++;
      if (hard_while_paused && m->npause == hard_while_paused) cclj_request(m, CCLJ_REQ_HARD);
      if (m->req & CCLJ_REQ_HARD) {  /* close() while paused: never resume. */
	res->kind = 2;
	res->abort_lat_us = cclj_qpc_us(cclj_qpc() - m->t_req_hard);
	snprintf(res->text, sizeof(res->text), "hard abort while paused");
	break;
      }
      st = cclj_resume_top(m, 0);
      continue;
    }
    if (st == LUA_YIELD) {
      int nargs = 0;
      res->nok++;
      has_filter = lua_gettop(top) >= 1 && lua_type(top, 1) == LUA_TSTRING;
      if (has_filter) snprintf(filter, sizeof(filter), "%s", lua_tostring(top, 1));
      lua_settop(top, 0);
      for (;;) {
	const char *ev;
	if (!evfn) break;  /* No event source: resume with no values. */
	InterlockedAnd(&m->req, ~CCLJ_REQ_PAUSE);
	lua_rawgeti(L, LUA_REGISTRYINDEX, evfn);
	lua_pushinteger(L, ++evi);
	if (lua_pcall(L, 1, LUA_MULTRET, 0)) {
	  res->kind = 4; snprintf(res->text, sizeof(res->text), "events(): %s", lua_tostring(L, -1));
	  goto stop;
	}
	ev = lua_tostring(L, 1);
	if (!ev) {  /* Out of events while the program still waits. */
	  res->kind = 3; snprintf(res->text, sizeof(res->text), "stalled waiting for %s after %ld events",
				  has_filter ? filter : "any event", res->nevents);
	  goto stop;
	}
	if (has_filter && strcmp(ev, filter) && strcmp(ev, "terminate")) {
	  res->ndropped++; lua_settop(L, 0); continue;  /* Consumed, as in CC:T. */
	}
	res->nevents++;
	nargs = lua_gettop(L);
	lua_xmove(L, top, nargs);
	break;
      }
      st = cclj_resume_top(m, nargs);
      continue;
    }
    if (st == LUA_OK) { res->kind = 0; values_to_text(top, res->text, sizeof(res->text)); break; }
    res->kind = 1;
    snprintf(res->text, sizeof(res->text), "%s", lua_isstring(top, -1) ? lua_tostring(top, -1) : "(non-string error)");
    break;
  }
stop:
  res->ms = cclj_qpc_us(cclj_qpc() - t0) / 1000;
  res->npause = m->npause;
  EnterCriticalSection(&g_mlock); g_cur = NULL; LeaveCriticalSection(&g_mlock);
  g_pauses_on = 0;
  g_oneshot_what = 0;
  /* Optional case.check(kind, text, stats) on the main thread (not after abort). */
  if (res->kind != 2 && !m->aborting) {
    lua_settop(L, 0);
    InterlockedAnd(&m->req, 0);
    if (get_case(L, file, name)) {
      lua_getfield(L, -1, "check");
      if (lua_isfunction(L, -1)) {
	lua_pushstring(L, kindname[res->kind]);
	lua_pushstring(L, res->text);
	lua_pushlightuserdata(L, m); lua_pushcclosure(L, h_stats, 1); lua_call(L, 0, 1);
	lua_pushboolean(L, pauses);
	if (lua_pcall(L, 4, 2, 0)) {
	  snprintf(res->check, sizeof(res->check), "check() error: %s", lua_tostring(L, -1));
	} else {
	  res->check_ok = lua_toboolean(L, -2);
	  snprintf(res->check, sizeof(res->check), "%s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
	}
      } else {
	res->check_ok = 1;
      }
    }
  } else {
    res->check_ok = 1;
  }
  /* Stats line for the log. */
  printf("    [%s] %s %.0f ms: okyields=%ld events=%ld dropped=%ld pauses=%ld hookpauses=%ld prop=%ld "
	 "failprop=%ld defer=%ld soft=%ld resumes=%ld depth_hw=%d deptherr=%ld floorerr=%ld "
	 "pauselat(avg/max)=%.1f/%.1f us maxdefer=%.1f us ckpt=%ld stack_hw=%lu reqs=%ld\n",
	 pauses ? "paused" : "ref   ", kindname[res->kind], res->ms, res->nok, res->nevents, res->ndropped,
	 m->npause, m->nhookpause, m->nprop, m->nfailprop, m->ndefer, m->nsoft, m->nresume,
	 m->depth_hw, m->ndepth_err, m->nfloor_err,
	 m->nhookpause ? m->sum_pause_lat_us / m->nhookpause : 0.0, m->max_pause_lat_us, m->max_defer_us, m->ncheckpoint,
	 m->min_sp == ~(uintptr_t)0 ? 0ul : (unsigned long)(m->stack_base - m->min_sp), (long)g_nreq);
  if (res->check[0]) printf("    [%s] check: %s %s\n", pauses ? "paused" : "ref   ",
			    res->check_ok ? "ok" : "FAILED", res->check);
  g_nreq = 0;
done:
  cclj_close(m);
  fflush(stdout);
}

typedef struct Job { const char *file; int ncase; char **cases; int rc; } Job;

static int list_cases(const char *file, char names[][64], int max)
{
  lua_State *L = luaL_newstate();
  int n = 0, i;
  luaL_openlibs(L);
  luaL_dostring(L, "host = setmetatable({}, {__index = function() return function() return {} end end})");
  if (luaL_loadfile(L, file) || lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "load %s: %s\n", file, lua_tostring(L, -1)); lua_close(L); return -1;
  }
  lua_getfield(L, -1, "order");
  for (i = 1; n < max; i++) {
    lua_rawgeti(L, -1, i);
    if (!lua_isstring(L, -1)) break;
    snprintf(names[n++], 64, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_close(L);
  return n;
}

static THREAD_FN(vm_thread)
{
  Job *job = (Job *)p;
  static char names[64][64];
  int n, i, fails = 0;
  plat_uptr lo, hi;
  plat_stack_limits(&lo, &hi);
  printf("VM thread stack reservation: %lu KB\n", (unsigned long)((hi - lo) / 1024));
  if (job->ncase > 0) {
    for (n = 0; n < job->ncase && n < 64; n++) snprintf(names[n], 64, "%s", job->cases[n]);
  } else if ((n = list_cases(job->file, names, 64)) < 0) {
    job->rc = 2; return 0;
  }
  for (i = 0; i < n; i++) {
    Result ref, pz;
    int ok = 1, cmp, expect = 0, max_abort_ms = 10;
    char error_contains[256] = "";
    lua_State *L = luaL_newstate();
    char why[512] = "";
    /* Per-case options (pauses at all? compare with reference?). */
    luaL_openlibs(L); luaL_dostring(L, "host = setmetatable({}, {__index = function() return function() return {} end end})");
    cmp = 1;
    if (get_case(L, job->file, names[i])) {
      int ci = lua_gettop(L);
      const char *ex;
      cmp = case_int(L, ci, "compare_ref", 1);
      max_abort_ms = case_int(L, ci, "max_abort_ms", 10);
      lua_getfield(L, ci, "expect"); ex = lua_tostring(L, -1);
      if (ex) expect = !strcmp(ex, "error") ? 1 : !strcmp(ex, "timeout") ? 2 : 0;
      lua_getfield(L, ci, "error_contains");
      if (lua_isstring(L, -1)) snprintf(error_contains, sizeof(error_contains), "%s", lua_tostring(L, -1));
    }
    lua_close(L);
    printf("CASE %s:%s\n", job->file, names[i]);
    fflush(stdout);
    if (cmp) run_case(job->file, names[i], 0, &ref);
    run_case(job->file, names[i], 1, &pz);
    if (cmp) {
      if (ref.kind != pz.kind) { ok = 0; snprintf(why, sizeof(why), "kind %s vs ref %s", kindname[pz.kind], kindname[ref.kind]); }
      else if (strcmp(ref.text, pz.text)) { ok = 0; snprintf(why, sizeof(why), "result differs from reference"); }
      else if (ref.nok != pz.nok) { ok = 0; snprintf(why, sizeof(why), "host-level yields %ld vs ref %ld", pz.nok, ref.nok); }
      else if (ref.nevents != pz.nevents || ref.ndropped != pz.ndropped) {
	ok = 0; snprintf(why, sizeof(why), "events %ld/%ld dropped vs ref %ld/%ld", pz.nevents, pz.ndropped, ref.nevents, ref.ndropped);
      }
      if (!ref.check_ok) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " ref check failed"); }
    }
    if (!pz.check_ok) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " check failed"); }
    if (pz.kind != expect) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " expected %s got %s", kindname[expect], kindname[pz.kind]); }
    if (error_contains[0] && !strstr(pz.text, error_contains)) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " error text lacks '%s'", error_contains); }
    if (pz.kind == 2 && pz.abort_lat_us > max_abort_ms * 1000.0) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " abort latency %.1f us > %d ms", pz.abort_lat_us, max_abort_ms); }
    if (pz.kind == 3 || pz.kind == 4) { ok = 0; snprintf(why + strlen(why), sizeof(why) - strlen(why), " %.200s", pz.text); }
    if (!ok) {
      printf("    ref   : %.300s\n    paused: %.300s\n", cmp ? ref.text : "-", pz.text);
    } else if (pz.kind == 2) {
      printf("    abort latency %.1f us\n", pz.abort_lat_us);
    } else {
      printf("    result: %.200s\n", pz.text);
    }
    printf("RESULT %s %s %s\n", names[i], ok ? "PASS" : "FAIL", why);
    fflush(stdout);
    if (!ok) fails++;
  }
  job->rc = fails ? 1 : 0;
  return 0;
}

/* -- Native nesting crash demo ------------------------------------------------- */
static uintptr_t nn_base;
static int nn_mark(lua_State *L)
{
  char probe;
  printf("depth %d stack %lu\n", (int)lua_tointeger(L, 1), (unsigned long)(nn_base - (uintptr_t)&probe));
  fflush(stdout);
  return 0;
}
static THREAD_FN(nest_thread)
{
  int wrapper = (int)(intptr_t)p;
  CcljMachine *m = cclj_new(0, wrapper ? 1000000000 : 200);
  lua_State *L = m->L;
  plat_uptr lo, hi;
  plat_stack_limits(&lo, &hi);
  nn_base = hi;
  printf("stack reservation %lu KB, %s coroutine.resume\n", (unsigned long)((hi - lo) / 1024),
	 wrapper ? "wrapper (no limit)" : "native");
  luaL_openlibs(L);
  if (wrapper) cclj_install(m, opt_hookn);
  lua_pushcfunction(L, nn_mark); lua_setglobal(L, "mark");
  if (luaL_dostring(L,
      "local d = 0\n"
      "local function nest()\n"
      "  d = d + 1\n"
      "  if d % 100 == 0 then mark(d) end\n"
      "  local ok, err = coroutine.resume(coroutine.create(nest))\n"
      "  if not ok then error(err, 0) end\n"
      "end\n"
      "return coroutine.resume(coroutine.create(nest))"))
    printf("error: %s\n", lua_tostring(L, -1));
  else
    printf("returned %s %s\n", lua_toboolean(L, -2) ? "true" : "false", lua_tostring(L, -1));
  return 0;
}

int main(int argc, char **argv)
{
  plat_thread th, rq;
  Job job;
  const char *e;
  char *slash;
  plat_timer_init();
  setvbuf(stdout, NULL, _IOFBF, 1 << 16);
  if ((e = getenv("PAUSE_MIN_US"))) opt_pmin = atoi(e);
  if ((e = getenv("PAUSE_MAX_US"))) opt_pmax = atoi(e);
  if ((e = getenv("NAIVE"))) opt_naive = atoi(e);
  if ((e = getenv("JIT"))) opt_jit = atoi(e);
  if ((e = getenv("HOOKN"))) opt_hookn = atoi(e);
  if ((e = getenv("POKE_US"))) opt_poke_us = atoi(e);
  if ((e = getenv("DEFER_REARM"))) opt_rearm = atoi(e);
  if ((e = getenv("DEFER_HOOKN"))) opt_defer_hookn = atoi(e);
  if ((e = getenv("CHECKPOINT"))) opt_ckpt = atoi(e);
  if ((e = getenv("MAXDEPTH"))) opt_maxdepth = atoi(e);
  if ((e = getenv("STACK_KB"))) opt_stack_kb = atoi(e);
  if ((e = getenv("STACK_FLOOR_KB"))) opt_floor_kb = atoi(e);
  if (argc < 2) { fprintf(stderr, "usage: pausehost <test.lua> [case...] | --native-nest | --wrapper-nest\n"); return 2; }
  if (!strcmp(argv[1], "--native-nest") || !strcmp(argv[1], "--wrapper-nest")) {
    th = plat_thread_start(nest_thread, (void *)(intptr_t)!strcmp(argv[1], "--wrapper-nest"),
			   (size_t)opt_stack_kb * 1024);
    plat_thread_join(th);
    fflush(stdout);
    return 0;
  }
  snprintf(test_dir, sizeof(test_dir), "%s", argv[1]);
  for (slash = test_dir; *slash; slash++) if (*slash == '\\') *slash = '/';
  slash = strrchr(test_dir, '/');
  if (slash) *slash = 0; else strcpy(test_dir, ".");
  printf("pausehost: %s jit=%s naive=%d hookn=%d maxdepth=%d pause=%d..%d us poke=%d us defer_hookn=%d rearm=%d checkpoint=%d floor=%d KB\n",
	 argv[1], opt_jit ? "on" : "off", opt_naive, opt_hookn, opt_maxdepth, opt_pmin, opt_pmax, opt_poke_us, opt_defer_hookn, opt_rearm, opt_ckpt, opt_floor_kb);
  InitializeCriticalSection(&g_mlock);
  job.file = argv[1]; job.ncase = argc - 2; job.cases = argv + 2; job.rc = 0;
  rq = plat_thread_start(requester, NULL, 0);
  /* The Lua-running thread gets an explicit 1 MB stack, like CC:T workers. */
  th = plat_thread_start(vm_thread, &job, (size_t)opt_stack_kb * 1024);
  plat_thread_join(th);
  g_stop = 1;
  plat_thread_join(rq);
  fflush(stdout);
  return job.rc;
}
