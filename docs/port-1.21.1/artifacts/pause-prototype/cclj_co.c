/*
** cclj_co.c - prototype of the CCLuaJIT "pause token protocol" (PLAN.md P10).
**
** Problem: CC:T asks a machine to PAUSE when its time slice expires and later
** continues it with handleEvent(null, null). A LuaJIT count hook can only
** lua_yield() the innermost running coroutine, and CC:T user code always runs
** in nested coroutines (bios -> parallel -> multishell -> shell -> program).
**
** Protocol implemented here:
**  1. coroutine.resume / coroutine.wrap are replaced. The C part
**     (cclj_raw_resume) does the real lua_resume() of the child. A tiny Lua
**     continuation (prelude below) loops when the C part reports "resumed
**     after a pause" by returning *zero* values (a real resume always
**     returns at least true/false).
**  2. Another thread requests a pause: req |= PAUSE; luaJIT_interrupt(L).
**  3. The count hook services it. If the running coroutine can yield AND every
**     wrapper on the active resume chain was called from a yieldable context
**     (m->nonyield == 0), it sets m->pausing and hook-yields. Otherwise it
**     leaves the request pending (deferral): interpreted code meets the hook
**     again within hookcount instructions, compiled code when a ticker calls
**     cclj_poke() (every ~250 us while a pause is pending). Optional
**     checkpoint wrappers serve it right after xpcall/load(reader) return.
**  4. Every wrapper whose child returns LUA_YIELD while m->pausing is set
**     yields its own coroutine with zero values (C-level lua_yield). This
**     repeats up the chain until the host's lua_resume(top) returns
**     LUA_YIELD with m->pausing set: that is MachineResult.PAUSE.
**  5. The host clears m->pausing and resumes top with zero values. Each
**     wrapper's C function "returns" those zero values, the Lua continuation
**     sees ok == nil and re-resumes its child with zero values, down to the
**     hook-yielded coroutine. A hook-yield discards resume values and
**     continues exactly where it stopped.
**  6. Hard abort: hook re-arms with count 1 and yields (or errors when not
**     yieldable). Wrappers that see m->aborting yield upward (or error) at
**     once, so no Lua code runs on the way up.
**  7. The wrapper also enforces a coroutine nesting limit (C stack safety).
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"     /* Internal: co->cframe, G(L)->intr, mainthread. */

#include "cclj_co.h"

#ifdef _WIN32
static LARGE_INTEGER qpf;
LONGLONG cclj_qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double cclj_qpc_us(LONGLONG ticks)
{
  if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
  return (double)ticks * 1e6 / (double)qpf.QuadPart;
}
#else
LONGLONG cclj_qpc(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (LONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
double cclj_qpc_us(LONGLONG ticks) { return (double)ticks / 1000.0; }
#endif

/* -- Machine lookup ------------------------------------------------------- */
/* The machine pointer is the allocator ud (the real host wraps lj_alloc for
** the memory cap anyway, PLAN.md P9), so the hook finds it without globals. */
typedef struct AllocWrap { lua_Alloc f; void *ud; CcljMachine *m; } AllocWrap;
static AllocWrap g_aw[1];  /* Prototype: one live machine at a time. */

static void *cclj_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  AllocWrap *aw = (AllocWrap *)ud;
  return aw->f(aw->ud, ptr, osize, nsize);
}

static CcljMachine *machine_of(lua_State *L)
{
  void *ud;
  lua_getallocf(L, &ud);
  return ((AllocWrap *)ud)->m;
}

/* -- Requests (any thread) ----------------------------------------------- */
void cclj_request(CcljMachine *m, int what)
{
  EnterCriticalSection(&m->lock);
  if (m->L) {
    LONGLONG now = cclj_qpc();
    if (what & CCLJ_REQ_PAUSE) m->t_req_pause = now;
    if (what & CCLJ_REQ_HARD) m->t_req_hard = now;
    InterlockedOr(&m->req, what);
    luaJIT_interrupt(m->L);  /* Two plain stores; never lua_sethook here. */
  }
  LeaveCriticalSection(&m->lock);
}

/* Any thread, periodically: re-interrupt while a deferred pause is pending,
** so compiled code inside a non-yieldable region re-checks at a bounded rate. */
void cclj_poke(CcljMachine *m)
{
  if (!(m->req & CCLJ_REQ_PAUSE)) return;  /* Racy pre-check; re-checked below. */
  EnterCriticalSection(&m->lock);
  if (m->L && m->running && (m->req & CCLJ_REQ_PAUSE)) luaJIT_interrupt(m->L);
  LeaveCriticalSection(&m->lock);
}

/* -- Hooks (VM thread) ---------------------------------------------------- */
static void hard_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (lua_isyieldable(L)) lua_yield(L, 0);  /* pcall cannot catch a yield. */
  luaL_error(L, "Too long without yielding");  /* Re-raised every instruction. */
}

static int find_tag(CcljMachine *m, lua_State *L)
{
  int i;
  for (i = 0; i < m->ntags; i++) if (m->tag_co[i] == L) return i;
  return -1;
}

static void cclj_hook(lua_State *L, lua_Debug *ar);

/* Bookkeeping when a pause starts (hook or checkpoint), VM thread. */
static void begin_pause(CcljMachine *m, lua_State *L)
{
  LONGLONG now = cclj_qpc();
  double lat = cclj_qpc_us(now - m->t_req_pause);
  int t;
  if (m->deferring) {  /* Back to the normal period (VM thread: safe). */
    m->deferring = 0;
    lua_sethook(L, cclj_hook, LUA_MASKCOUNT, m->hookn);
  }
  InterlockedAnd(&m->req, ~CCLJ_REQ_PAUSE);
  m->pausing = 1;
  m->nhookpause++;
  if (lat > m->max_pause_lat_us) m->max_pause_lat_us = lat;
  m->sum_pause_lat_us += lat;
  if (m->defer_t0) {
    double d = cclj_qpc_us(now - m->defer_t0);
    if (d > m->max_defer_us) m->max_defer_us = d;
    m->defer_t0 = 0;
  }
  if ((unsigned)m->region < CCLJ_MAX_REGIONS) m->rpause[m->region]++;
  if ((t = find_tag(m, L)) >= 0) m->tagpause[t]++;
}

/* Explicit pause point for sandbox wrappers, called right after a native
** C-call boundary (xpcall, load with a reader) returns: serves a deferred
** pause at once instead of waiting for the next hook run. It is a C-function
** yield; when resumed (with no values) it returns no values. */
static int cclj_checkpoint(lua_State *L)
{
  CcljMachine *m = (CcljMachine *)lua_touserdata(L, lua_upvalueindex(1));
  if ((m->req & CCLJ_REQ_PAUSE) && !m->pausing && !m->aborting &&
      m->nonyield == 0 && lua_isyieldable(L)) {
    m->ncheckpoint++;
    begin_pause(m, L);
    return lua_yield(L, 0);
  }
  return 0;
}

static void cclj_hook(lua_State *L, lua_Debug *ar)
{
  CcljMachine *m = machine_of(L);
  LONG r = m->req;
  if (!r) return;  /* Periodic call, nothing pending: ~free. */
  if (r & CCLJ_REQ_HARD) {
    m->aborting = 1;
    lua_sethook(L, hard_hook, LUA_MASKCOUNT, 1);  /* VM thread: safe. */
    hard_hook(L, ar);
  }
  if (r & CCLJ_REQ_SOFT) {
    InterlockedAnd(&m->req, ~CCLJ_REQ_SOFT);
    m->nsoft++;
    luaL_where(L, 0);
    lua_pushliteral(L, "Too long without yielding");
    lua_concat(L, 2);
    lua_error(L);
  }
  if (r & CCLJ_REQ_PAUSE) {
    if (m->pausing) return;  /* Not reachable: no Lua runs while pausing. */
    if (m->naive) {
      /* Control: the naive scheme (hook-yield whatever coroutine runs). */
      if (lua_isyieldable(L)) {
	InterlockedAnd(&m->req, ~CCLJ_REQ_PAUSE);
	m->nhookpause++;
	if (L == m->top) m->pausing = 1;  /* Only then can the host tell. */
	lua_yield(L, 0);
      }
      return;
    }
    if (m->nonyield == 0 && lua_isyieldable(L)) {
      begin_pause(m, L);
      lua_yield(L, 0);  /* Hook-yield: transparent on resume. */
    }
    /* Cannot pause here (C-call boundary on the chain): defer. The request
    ** stays pending. Interpreted code sees the hook again after <= hookcount
    ** instructions; compiled code only when cclj_poke() re-interrupts it
    ** (a ticker calls it every ~250 us while a pause is pending).
    ** (v1 re-armed G(L)->intr here: every trace entry then exited to the
    ** hook, which made JIT code inside sort/gsub callbacks 4-7x slower.) */
    m->ndefer++;
    if (!m->defer_t0) m->defer_t0 = cclj_qpc();
    if ((unsigned)m->region < CCLJ_MAX_REGIONS) m->rdefer[m->region]++;
    if (m->defer_rearm) G(L)->intr = 1;  /* v1 behaviour, for comparison only. */
    if (m->defer_hookn && !m->deferring) {
      /* Check interpreted code more often until the pause is taken, so a
      ** short yieldable gap between two C-boundary calls is not missed. */
      m->deferring = 1;
      lua_sethook(L, cclj_hook, LUA_MASKCOUNT, m->defer_hookn);
    }
  }
}

/* -- The resume wrapper (VM thread) -------------------------------------- */

static const char *co_statusname(lua_State *L, lua_State *co)
{
  if (co == L) return "running";
  if (co->cframe != NULL) return "normal";
  return "dead";
}

/* Shared core of coroutine.resume (wrap == 0) and coroutine.wrap (wrap == 1).
** Returns: true, values... | false, err (resume only) | zero values when the
** caller was resumed after a pause (the Lua continuation then loops). */
static int raw_resume(lua_State *L, int wrap)
{
  CcljMachine *m = (CcljMachine *)lua_touserdata(L, lua_upvalueindex(1));
  lua_State *co;
  int narg, status, ny;
  char probe;
  if (lua_type(L, 1) != LUA_TTHREAD) {
    luaL_where(L, 2);
    lua_pushfstring(L, "bad argument #1 to '%s' (coroutine expected, got %s)",
		    wrap ? "wrap" : "resume", luaL_typename(L, 1));
    lua_concat(L, 2);
    return lua_error(L);
  }
  co = lua_tothread(L, 1);
  narg = lua_gettop(L) - 1;
  /* Same conditions as LuaJIT's ffh_resume(), plus the main thread. */
  if (co->cframe != NULL || co->status > LUA_YIELD ||
      (co->status == LUA_OK && co->top == co->base) ||
      co == mainthread(G(L))) {
    const char *msg = lua_pushfstring(L, "cannot resume %s coroutine", co_statusname(L, co));
    if (wrap) { luaL_where(L, 2); lua_insert(L, -2); lua_concat(L, 2); return lua_error(L); }
    lua_pushboolean(L, 0); lua_insert(L, -2); (void)msg;
    return 2;
  }
  if (m->depth >= m->max_depth || (uintptr_t)&probe < m->stack_floor) {
    if (m->depth >= m->max_depth) {
      m->ndepth_err++;
      lua_pushfstring(L, "coroutine nesting too deep (limit %d)", m->max_depth);
    } else {
      m->nfloor_err++;
      lua_pushliteral(L, "C stack overflow");
    }
    if (wrap) { luaL_where(L, 2); lua_insert(L, -2); lua_concat(L, 2); return lua_error(L); }
    lua_pushboolean(L, 0); lua_insert(L, -2);
    return 2;
  }
  if (!lua_checkstack(co, narg + LUA_MINSTACK)) {
    lua_pushliteral(L, "too many arguments to resume");
    if (wrap) return lua_error(L);
    lua_pushboolean(L, 0); lua_insert(L, -2);
    return 2;
  }
  lua_xmove(L, co, narg);
  ny = !lua_isyieldable(L);
  if ((uintptr_t)&probe < m->min_sp) m->min_sp = (uintptr_t)&probe;
  for (;;) {
    m->nonyield += ny;
    if (++m->depth > m->depth_hw) m->depth_hw = m->depth;
    m->nresume++;
    status = lua_resume(co, narg);
    m->nonyield -= ny;
    m->depth--;
    if (m->aborting) {  /* Hard abort: get out without running Lua code. */
      lua_settop(co, 0);
      if (!ny && lua_isyieldable(L)) return lua_yield(L, 0);
      lua_pushliteral(L, "Too long without yielding");
      return lua_error(L);
    }
    if (status == LUA_YIELD && m->pausing) {
      lua_settop(co, 0);  /* A paused child yields no values. */
      if (!ny) {
	m->nprop++;
	return lua_yield(L, 0);  /* Propagate the pause to our resumer. */
      }
      /* Cannot propagate (the hook's nonyield check makes this unreachable):
      ** cancel this pause, keep it requested, continue the child at once. */
      m->pausing = 0;
      m->nfailprop++;
      InterlockedOr(&m->req, CCLJ_REQ_PAUSE);
      G(L)->intr = 1;
      narg = 0;
      continue;
    }
    break;
  }
  if (status == LUA_OK || status == LUA_YIELD) {
    int nres = lua_gettop(co);
    if (!lua_checkstack(L, nres + 1)) {
      lua_settop(co, 0);
      lua_pushliteral(L, "too many results to resume");
      if (wrap) return lua_error(L);
      lua_pushboolean(L, 0); lua_insert(L, -2);
      return 2;
    }
    lua_pushboolean(L, 1);
    lua_xmove(co, L, nres);
    return nres + 1;
  }
  /* Error: the coroutine is dead; its frames stay for debug.traceback(co). */
  lua_xmove(co, L, 1);
  if (wrap) return lua_error(L);  /* Cobalt semantics: re-raise unchanged. */
  lua_pushboolean(L, 0); lua_insert(L, -2);
  return 2;
}

static int cclj_raw_resume(lua_State *L) { return raw_resume(L, 0); }
static int cclj_raw_wresume(lua_State *L) { return raw_resume(L, 1); }

/* Lua continuation. A real resume returns at least true/false, so a nil
** first value means "this coroutine was resumed by the host after a pause":
** re-resume the child with no values (tail calls: no stack growth). */
static const char prelude[] =
  "local raw_resume, raw_wresume, create, type, error = ...\n"
  "local function cont(co, ok, ...)\n"
  "  if ok == nil then return cont(co, raw_resume(co)) end\n"
  "  return ok, ...\n"
  "end\n"
  "local function resume(co, ...)\n"
  "  return cont(co, raw_resume(co, ...))\n"
  "end\n"
  "local function wcont(co, ok, ...)\n"
  "  if ok == nil then return wcont(co, raw_wresume(co)) end\n"
  "  return ...\n"
  "end\n"
  "local function wrap(f)\n"
  "  if type(f) ~= 'function' then\n"
  "    error(\"bad argument #1 to 'wrap' (function expected, got \" .. type(f) .. ')', 2)\n"
  "  end\n"
  "  local co = create(f)\n"
  "  return function(...) return wcont(co, raw_wresume(co, ...)) end\n"
  "end\n"
  "return resume, wrap\n";

/* Optional: xpcall and load(reader) re-wrapped so that a pause deferred
** inside their non-yieldable callbacks is served as soon as they return. */
static const char prelude_ckpt[] =
  "local checkpoint, xpcall, load, type = ...\n"
  "local function after(...) checkpoint() return ... end\n"
  "local function xpcall_(f, h, ...) return after(xpcall(f, h, ...)) end\n"
  "local function load_(chunk, ...)\n"
  "  if type(chunk) == 'function' then return after(load(chunk, ...)) end\n"
  "  return load(chunk, ...)\n"
  "end\n"
  "return xpcall_, load_\n";

void cclj_install(CcljMachine *m, int hookcount)
{
  lua_State *L = m->L;
  lua_getglobal(L, "coroutine");
  /* Keep the natives in the registry for the benchmark only. */
  lua_getfield(L, -1, "resume"); lua_setfield(L, LUA_REGISTRYINDEX, "cclj.native_resume");
  lua_getfield(L, -1, "wrap"); lua_setfield(L, LUA_REGISTRYINDEX, "cclj.native_wrap");
  if (!m->naive) {
    if (luaL_loadbuffer(L, prelude, sizeof(prelude) - 1, "=[cclj coroutine]")) {
      fprintf(stderr, "prelude: %s\n", lua_tostring(L, -1)); exit(1);
    }
    lua_pushlightuserdata(L, m); lua_pushcclosure(L, cclj_raw_resume, 1);
    lua_pushlightuserdata(L, m); lua_pushcclosure(L, cclj_raw_wresume, 1);
    lua_getfield(L, -4, "create");
    lua_getglobal(L, "type");
    lua_getglobal(L, "error");
    lua_call(L, 5, 2);
    lua_setfield(L, -3, "wrap");
    lua_setfield(L, -2, "resume");
  }
  lua_pop(L, 1);
  if (!m->naive && m->use_checkpoint) {
    if (luaL_loadbuffer(L, prelude_ckpt, sizeof(prelude_ckpt) - 1, "=[cclj checkpoint]")) {
      fprintf(stderr, "prelude: %s\n", lua_tostring(L, -1)); exit(1);
    }
    lua_pushlightuserdata(L, m); lua_pushcclosure(L, cclj_checkpoint, 1);
    lua_getglobal(L, "xpcall");
    lua_getglobal(L, "load");
    lua_getglobal(L, "type");
    lua_call(L, 4, 2);
    lua_setglobal(L, "load");
    lua_setglobal(L, "xpcall");
  }
  m->hookn = hookcount;
  lua_sethook(L, cclj_hook, LUA_MASKCOUNT, hookcount);  /* VM thread, once. */
}

CcljMachine *cclj_new(int naive, int max_depth)
{
  CcljMachine *m = (CcljMachine *)calloc(1, sizeof(CcljMachine));
  lua_State *L = luaL_newstate();
  InitializeCriticalSection(&m->lock);
  g_aw->f = lua_getallocf(L, &g_aw->ud);
  g_aw->m = m;
  lua_setallocf(L, cclj_alloc, g_aw);
  m->naive = naive;
  m->max_depth = max_depth;
  m->min_sp = ~(uintptr_t)0;
  m->L = L;
  return m;
}

void cclj_close(CcljMachine *m)
{
  lua_State *L = m->L;
  EnterCriticalSection(&m->lock);
  m->L = NULL;  /* No more luaJIT_interrupt() after this point. */
  LeaveCriticalSection(&m->lock);
  if (L) lua_close(L);
  DeleteCriticalSection(&m->lock);
  free(m);
}

/* Resume the top coroutine. On return: LUA_YIELD + m->pausing -> PAUSE;
** LUA_YIELD otherwise -> OK (filter = first value if string); 0 -> bios
** returned; error -> error; m->aborting -> TIMEOUT. */
int cclj_resume_top(CcljMachine *m, int nargs)
{
  int st;
  InterlockedAnd(&m->req, ~CCLJ_REQ_PAUSE);  /* Like TimeoutState.reset(). */
  m->pausing = 0;
  m->defer_t0 = 0;
  if (m->deferring && !m->aborting) {
    m->deferring = 0;
    lua_sethook(m->L, cclj_hook, LUA_MASKCOUNT, m->hookn);
  }
  m->running = 1;
  st = lua_resume(m->top, nargs);
  m->running = 0;
  return st;
}
