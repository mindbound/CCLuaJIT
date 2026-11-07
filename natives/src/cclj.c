/*
** libcclj: CCLuaJIT's native library. JNI glue plus a statically linked, patched LuaJIT (natives/build-natives.sh).
**
** M0 contents: only the self-test that the test class io.github.mindbound.cclj.natives.NativeProbe calls
** (./gradlew :common:nativeProbe). The machine bridge (docs/port-1.21.1/DESIGN.md sections 3 and 4) comes later.
**
** Rules for every function in this file (DESIGN.md section 2):
** - Plain C. A Lua error unwinds the C stack (DWARF unwinding on Linux, SEH on Windows): there are no destructors
**   or catch blocks that could run or swallow it. Code that a Lua error can unwind through (hooks, C functions
**   called from Lua) needs unwind tables: build-natives.sh compiles with -funwind-tables.
** - Nothing raises outside a protected call. The self-test body runs in one lua_cpcall, and each check runs its
**   Lua code in its own lua_pcall, so a failing check cannot hide the others. Outside the lua_cpcall, only
**   luaL_newstate, lua_close and non-allocating stack reads are used.
** - Only the Java_* functions are exported (JNIEXPORT; the build uses -fvisibility=hidden and, on Linux,
**   -Wl,--exclude-libs,ALL so LuaJIT's lua_* symbols stay internal).
** - No mutable static state: the JVM may call into the library from several threads at once.
** - A thread that the library starts (the interrupt check's second thread) is joined before the call that started
**   it returns, and never calls into the JVM.
*/
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include <jni.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

/* Set by build-natives.sh. */
#ifndef CCLJ_PLATFORM
#define CCLJ_PLATFORM "unknown"
#endif
#ifndef CCLJ_LUAJIT_COMMIT
#define CCLJ_LUAJIT_COMMIT "unknown"
#endif

#define SELFTEST_CHUNKNAME "=cclj selftest"

/* -- Report: one key=value per line ---------------------------------------- */

#define REPORT_SIZE 8192
#define REPORT_RESERVE 32  /* Always room left for the final "ok=false\n". */

typedef struct Report {
  char buf[REPORT_SIZE];
  size_t len;
  int ok;         /* No check has failed so far. */
  int truncated;  /* A line did not fit. */
} Report;

/* Appends "key=value\n". The value is escaped to printable ASCII ("\\", "\n", "\xNN"), so the report is one line
** per key and valid (modified) UTF-8 for NewStringUTF whatever bytes a Lua string holds.
*/
static void rep_add(Report *r, const char *key, const char *val)
{
  static const char hex[] = "0123456789abcdef";
  char line[1024];
  size_t n = 0;
  const size_t cap = sizeof(line) - 1;  /* Keep one byte for the '\n'. */
  const unsigned char *p;
  for (p = (const unsigned char *)key; *p && n < cap; p++) line[n++] = (char)*p;
  if (n < cap) line[n++] = '=';
  for (p = (const unsigned char *)(val ? val : "(null)"); *p && n + 4 <= cap; p++) {
    if (*p == '\\') { line[n++] = '\\'; line[n++] = '\\'; }
    else if (*p == '\n') { line[n++] = '\\'; line[n++] = 'n'; }
    else if (*p >= 0x20 && *p < 0x7f) line[n++] = (char)*p;
    else { line[n++] = '\\'; line[n++] = 'x'; line[n++] = hex[*p >> 4]; line[n++] = hex[*p & 15]; }
  }
  line[n++] = '\n';
  if (r->len + n + REPORT_RESERVE >= sizeof(r->buf)) { r->truncated = 1; return; }
  memcpy(r->buf + r->len, line, n);
  r->len += n;
  r->buf[r->len] = '\0';
}

/* Appends "key=ok: detail" or "key=FAIL: detail". A failure clears the overall ok flag. */
static void rep_check(Report *r, const char *key, int pass, const char *detail)
{
  char v[900];
  snprintf(v, sizeof(v), "%s%s%s", pass ? "ok" : "FAIL", (detail && *detail) ? ": " : "", detail ? detail : "");
  if (!pass) r->ok = 0;
  rep_add(r, key, v);
}

/* The string at idx, or dflt if the value is not a string. Never converts: lua_tolstring on a number allocates
** and may raise, which is not allowed outside a protected call.
*/
static const char *str_at(lua_State *L, int idx, const char *dflt)
{
  return lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : dflt;
}

/* -- Checks (all run inside selftest_body, i.e. under lua_cpcall) ---------- */

/* Runs a Lua chunk that returns (passed, detail). Load and runtime errors count as failures. */
static void lua_check(lua_State *L, Report *r, const char *key, const char *src)
{
  int top = lua_gettop(L);
  int st = luaL_loadbuffer(L, src, strlen(src), SELFTEST_CHUNKNAME);
  if (st == 0) st = lua_pcall(L, 0, 2, 0);
  if (st != 0) rep_check(r, key, 0, str_at(L, -1, "(error object is not a string)"));
  else rep_check(r, key, lua_toboolean(L, -2), str_at(L, -1, ""));
  lua_settop(L, top);
}

/* (b) Version, platform and the JIT state. */
static const char check_luajit[] =
  "return jit ~= nil and jit.version ~= nil,"
  " jit and (jit.version .. ' (' .. jit.os .. ' ' .. jit.arch .. ')') or 'no jit module'";

/* (a) The JIT compiler is on (jit.status() also lists the CPU features and optimizations). */
static const char check_jit[] =
  "local st = {jit.status()}\n"
  "return st[1] == true, (st[1] and 'on' or 'off') .. ' ' .. table.concat(st, ' ', 2)";

/* DESIGN.md section 2: the LUAJIT_SECURITY_* defaults are kept (prng, strhash and mcode on). */
static const char check_security[] =
  "local p, h, i, m = jit.security('prng'), jit.security('strhash'), jit.security('strid'), jit.security('mcode')\n"
  "return p > 0 and h > 0 and m > 0, ('prng=%d strhash=%d strid=%d mcode=%d'):format(p, h, i, m)";

/* Sandbox build flag LUAJIT_DISABLE_FFI. Looks at package.loaded and package.preload only: require('ffi') would also
** run the C searchers, which may load a stray ffi.dll or ffi.so from the working directory.
*/
static const char check_ffi[] =
  "local present = package.loaded.ffi ~= nil or package.preload.ffi ~= nil\n"
  "return not present, present and 'the ffi library is present' or 'the ffi library is absent'";

/* (b) A computation, and proof that its loop was compiled to a trace. */
static const char check_compute[] =
  "local s = 0\n"
  "for i = 1, 1e6 do s = s + i % 7 end\n"
  "local util, traces = require('jit.util'), 0\n"
  "while util.traceinfo(traces + 1) do traces = traces + 1 end\n"
  "return s == 2999998 and traces > 0,\n"
  "  ('sum(i %% 7, i = 1..1e6) = %d (expected 2999998), %d trace(s) compiled'):format(s, traces)";

/* (e) Patch 03: no loader accepts bytecode. */
static const char check_bytecode[] =
  "local bc = string.dump(function() return 1 end)\n"
  "local f1, e1 = load(bc)\n"
  "local f2, e2 = loadstring(bc)\n"
  "local refused = f1 == nil and f2 == nil and tostring(e1):find('binary chunk', 1, true) ~= nil\n"
  "return refused, 'load -> ' .. tostring(f1) .. ', ' .. tostring(e1) .. '; loadstring -> ' .. tostring(f2)";

/* The count hook of the interrupt check: any call is an interrupt (see check_interrupt). Raising an error from a
** hook is fine: the hook runs inside the lua_pcall that runs the loop.
*/
static void selftest_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  luaL_error(L, "cclj selftest: interrupted");
}

/* (c) Patch 01: an interrupt from another thread stops a JIT-compiled loop. This is how CC:T's timeout will reach a
** busy computer: its monitor thread calls luaJIT_interrupt(L) while the VM thread runs compiled code, the trace's
** poll exits to the interpreter, and the count hook raises an error. The check does exactly that:
** - the VM thread installs a count hook with the largest count, so in practice only an interrupt makes it fire;
** - Lua calls started() and then runs a long loop that stores its counter in a table. The loop is compiled within
**   its first few hundred iterations;
** - a second thread waits for started(), sleeps INTR_DELAY_MS and calls luaJIT_interrupt(L) once.
** It passes only if the hook stopped the loop before its end and the loop was compiled (jit.util sees a new trace).
** Without the poll in compiled code, the loop runs to its end (1-2 s) and the hook fires only after it: FAIL.
** (Calling luaJIT_interrupt on the VM thread proves less: its hookcount = 1 hint makes the hook fire at the next
** interpreted instruction, before any trace is entered.) The VM thread joins the second thread before it goes on,
** so no interrupt can reach a later check.
*/
#define INTR_LOOP_N 2e9     /* Iterations: far more than fit in INTR_DELAY_MS, even compiled. */
#define INTR_DELAY_MS 50    /* From started() to luaJIT_interrupt(). */
#define INTR_WAIT_MS 10000  /* The second thread gives up, without interrupting, if started() is not called by then. */

static const char intr_src[] =
  "local started, n = ...\n"
  "local util = require('jit.util')\n"
  "local function traces() local k = 0 while util.traceinfo(k + 1) do k = k + 1 end return k end\n"
  "local t, before = {0}, traces()\n"
  "local ok, err = pcall(function()\n"
  "  started()\n"
  "  for i = 1, n do t[1] = i end\n"
  "end)\n"
  "return ok, err, t[1], traces() - before";

typedef struct Interrupter {
  lua_State *L;
  int started;  /* Set by started() (VM thread): the loop starts now. */
  int done;     /* Set by the VM thread once the Lua code has returned: the second thread must not wait longer. */
  int sent;     /* Set by the second thread after luaJIT_interrupt(). All three use __atomic builtins. */
} Interrupter;

static void sleep_ms(int ms)
{
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
}

/* The second thread. It calls nothing in the library but luaJIT_interrupt, which is safe from any thread. */
static void interrupter_run(Interrupter *it)
{
  int waited = 0;
  while (!__atomic_load_n(&it->started, __ATOMIC_ACQUIRE)) {
    if (__atomic_load_n(&it->done, __ATOMIC_ACQUIRE) || waited >= INTR_WAIT_MS) return;
    sleep_ms(1);
    waited++;
  }
  sleep_ms(INTR_DELAY_MS);
  luaJIT_interrupt(it->L);
  __atomic_store_n(&it->sent, 1, __ATOMIC_RELEASE);
}

#ifdef _WIN32
static DWORD WINAPI interrupter_main(LPVOID arg) { interrupter_run((Interrupter *)arg); return 0; }
#else
static void *interrupter_main(void *arg) { interrupter_run((Interrupter *)arg); return NULL; }
#endif

/* started(), called from Lua: the Interrupter is upvalue 1. */
static int intr_started(lua_State *L)
{
  Interrupter *it = (Interrupter *)lua_touserdata(L, lua_upvalueindex(1));
  __atomic_store_n(&it->started, 1, __ATOMIC_RELEASE);
  return 0;
}

static void check_interrupt(lua_State *L, Report *r)
{
  char d[700];
  Interrupter it;
  int top = lua_gettop(L), st, sent;
#ifdef _WIN32
  HANDLE th;
#else
  pthread_t th;
#endif
  it.L = L; it.started = 0; it.done = 0; it.sent = 0;
  /* Everything that can raise (allocations) comes first. Once the thread runs, only lua_pcall and lua_sethook are
  ** called until the join, and neither raises, so no error can unwind past the join and leave the thread with a
  ** pointer into a dead stack frame.
  */
  if (luaL_loadbuffer(L, intr_src, sizeof(intr_src) - 1, SELFTEST_CHUNKNAME)) {
    rep_check(r, "interrupt", 0, str_at(L, -1, "(error object is not a string)"));
    lua_settop(L, top);
    return;
  }
  lua_pushlightuserdata(L, &it);
  lua_pushcclosure(L, intr_started, 1);
  lua_pushnumber(L, INTR_LOOP_N);
#ifdef _WIN32
  th = CreateThread(NULL, 0, interrupter_main, &it, 0, NULL);
  if (th == NULL) {
#else
  if (pthread_create(&th, NULL, interrupter_main, &it) != 0) {
#endif
    rep_check(r, "interrupt", 0, "could not start the interrupting thread");
    lua_settop(L, top);
    return;
  }
  lua_sethook(L, selftest_hook, LUA_MASKCOUNT, 0x7fffffff);
  st = lua_pcall(L, 2, 4, 0);
  __atomic_store_n(&it.done, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
#else
  pthread_join(th, NULL);
#endif
  lua_sethook(L, NULL, 0, 0);
  sent = __atomic_load_n(&it.sent, __ATOMIC_ACQUIRE);
  if (st != 0) {
    snprintf(d, sizeof(d), "the check itself failed (status %d): %s", st,
	     str_at(L, -1, "(error object is not a string)"));
    rep_check(r, "interrupt", 0, d);
  } else {
    int ok = lua_toboolean(L, -4);
    const char *err = str_at(L, -3, "(error object is not a string)");
    double reached = (double)lua_tonumber(L, -2), traces = (double)lua_tonumber(L, -1);
    if (!sent) {
      rep_check(r, "interrupt", 0, "the second thread sent no interrupt: started() was not called in time");
    } else if (ok) {
      snprintf(d, sizeof(d), "the loop ran to its end (%.0f iterations) and the hook never fired", reached);
      rep_check(r, "interrupt", 0, d);
    } else if (strstr(err, "cclj selftest: interrupted") == NULL) {
      snprintf(d, sizeof(d), "the loop stopped with an unexpected error: %s", err);
      rep_check(r, "interrupt", 0, d);
    } else if (reached >= INTR_LOOP_N) {
      rep_check(r, "interrupt", 0, "the hook fired only after the loop had run to its end: compiled code does not "
		"poll for interrupts");
    } else if (traces < 1) {
      snprintf(d, sizeof(d), "the loop stopped at iteration %.0f, but it was not JIT-compiled, so the poll in "
	       "compiled code was not tested", reached);
      rep_check(r, "interrupt", 0, d);
    } else {
      snprintf(d, sizeof(d), "luaJIT_interrupt() from a second thread, %d ms after the loop started, stopped the "
	       "compiled loop (%.0f new trace(s)) at iteration %.0f of %.0f with \"%s\"", INTR_DELAY_MS, traces,
	       reached, INTR_LOOP_N, err);
      rep_check(r, "interrupt", 1, d);
    }
  }
  lua_settop(L, top);
}

/* (d) Patch 08. Linking luaJIT_setcstacklimit proves the symbol is there. To show that the guard works, set a red
** zone that contains the current C stack pointer: lua_pcall must then return "C stack overflow" (patch 08 raises
** it inside a protected frame) instead of running the chunk. Then switch the guard off (lo == hi) and run the
** same chunk normally. Nothing else re-enters the VM while the zone is set.
*/
static void check_cstacklimit(lua_State *L, Report *r)
{
  char d[700], msg[300];
  volatile char here = 0;  /* An address in this frame. lua_pcall's frame is below it (the stack grows down). */
  uintptr_t sp = (uintptr_t)&here;
  int top = lua_gettop(L), st1, st2;
  if (luaL_loadbuffer(L, "return 1", 8, SELFTEST_CHUNKNAME)) {
    rep_check(r, "cstacklimit", 0, str_at(L, -1, "(error object is not a string)"));
    lua_settop(L, top);
    return;
  }
  luaJIT_setcstacklimit(L, (const void *)(sp - (256u << 10)), (const void *)(sp + 4096));
  st1 = lua_pcall(L, 0, 1, 0);
  luaJIT_setcstacklimit(L, NULL, NULL);
  snprintf(msg, sizeof(msg), "%s", st1 ? str_at(L, -1, "(error object is not a string)") : "no error");
  lua_settop(L, top);
  st2 = luaL_loadbuffer(L, "return 1", 8, SELFTEST_CHUNKNAME);
  if (st2 == 0) st2 = lua_pcall(L, 0, 1, 0);
  if (st1 == LUA_ERRRUN && strstr(msg, "C stack overflow") != NULL && st2 == 0 && lua_tonumber(L, -1) == 1) {
    snprintf(d, sizeof(d), "lua_pcall inside a red zone set by luaJIT_setcstacklimit() returned \"%s\"; "
	     "with the zone cleared the same call runs", msg);
    rep_check(r, "cstacklimit", 1, d);
  } else {
    snprintf(d, sizeof(d), "in the red zone: status %d \"%s\"; zone cleared: status %d \"%s\"", st1, msg, st2,
	     st2 ? str_at(L, -1, "(error object is not a string)") : "ok");
    rep_check(r, "cstacklimit", 0, d);
  }
  lua_settop(L, top);
}

/* The whole self-test. Runs under lua_cpcall; the Report is the light userdata argument. */
static int selftest_body(lua_State *L)
{
  Report *r = (Report *)lua_touserdata(L, 1);
  lua_settop(L, 0);
  luaL_openlibs(L);
  rep_check(r, "state", 1, "luaL_newstate and luaL_openlibs");
  lua_check(L, r, "luajit", check_luajit);
  lua_check(L, r, "jit", check_jit);
  lua_check(L, r, "security", check_security);
  lua_check(L, r, "ffi", check_ffi);
  lua_check(L, r, "compute", check_compute);
  check_interrupt(L, r);
  check_cstacklimit(L, r);
  lua_check(L, r, "bytecode", check_bytecode);
  return 0;
}

/* -- JNI ------------------------------------------------------------------- */

/* NativeProbe.selfTest(): the report, one key=value per line, ending with "ok=true" or "ok=false". */
JNIEXPORT jstring JNICALL Java_io_github_mindbound_cclj_natives_NativeProbe_selfTest(JNIEnv *env, jclass cls)
{
  Report *r = (Report *)malloc(sizeof(Report));
  lua_State *L;
  jstring res;
  (void)cls;
  if (r == NULL) return (*env)->NewStringUTF(env, "selftest=FAIL: out of memory\nok=false\n");
  r->len = 0; r->buf[0] = '\0'; r->ok = 1; r->truncated = 0;
  rep_add(r, "cclj.platform", CCLJ_PLATFORM);
  rep_add(r, "cclj.luajit_commit", CCLJ_LUAJIT_COMMIT);
  L = luaL_newstate();
  if (L == NULL) {
    rep_check(r, "state", 0, "luaL_newstate returned NULL");
  } else {
    int st = lua_cpcall(L, selftest_body, r);
    if (st != 0) {
      char d[700];
      snprintf(d, sizeof(d), "the self-test stopped (status %d): %s", st,
	       str_at(L, -1, "(error object is not a string)"));
      rep_check(r, "selftest", 0, d);
    }
    lua_close(L);
  }
  if (r->truncated) r->ok = 0;
  snprintf(r->buf + r->len, sizeof(r->buf) - r->len, "%sok=%s\n", r->truncated ? "truncated=true\n" : "",
	   r->ok ? "true" : "false");
  res = (*env)->NewStringUTF(env, r->buf);  /* NULL with an OutOfMemoryError pending if that fails. */
  free(r);
  return res;
}
