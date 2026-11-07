/* Bare lua_resume/coroutine.yield round trip from C, plus the same driven
** from Lua through a trivial C function (cost split for the wrapper). */
#include <stdio.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart / f.QuadPart; }
#else
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif
static int cresume(lua_State *L)
{
  lua_State *co = lua_tothread(L, 1);
  int st = lua_resume(co, 0), n = lua_gettop(co);
  (void)st;
  lua_pushboolean(L, 1);
  lua_xmove(co, L, n);
  return n + 1;
}
int main(void)
{
  int i, N = 5000000, k;
  for (k = 0; k < 3; k++) {
    lua_State *L = luaL_newstate(), *co;
    double t0, t1, t2;
    luaL_openlibs(L);
    luaL_dostring(L, "return function() local i = 0 while true do i = i + 1 coroutine.yield(i) end end");
    co = lua_newthread(L);
    lua_pushvalue(L, -2); lua_xmove(L, co, 1);
    t0 = now();
    for (i = 0; i < N; i++) { lua_resume(co, 0); lua_settop(co, 0); }
    t1 = now();
    lua_pushcfunction(L, cresume); lua_setglobal(L, "cresume");
    luaL_dostring(L, "local co = coroutine.create(function() local i = 0 while true do i = i + 1 coroutine.yield(i) end end)\n"
		     "local r = cresume local s = 0 for i = 1, 5000000 do local ok, v = r(co) s = s + v end");
    t2 = now();
    printf("C loop lua_resume: %.1f ns/op   Lua->C fn->lua_resume: %.1f ns/op\n", (t1 - t0) * 1e9 / N, (t2 - t1) * 1e9 / N);
    lua_close(L);
  }
  return 0;
}
