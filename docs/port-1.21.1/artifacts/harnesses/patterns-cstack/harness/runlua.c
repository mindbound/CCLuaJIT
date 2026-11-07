/*
** runlua.c -- run a Lua file (args passed as varargs) in a fresh state with a
** permanent count hook. With STRESS_MS=<n>, a second thread requests an
** interrupt every n ms; the hook then grows the Lua stack, creates garbage,
** runs a full GC and returns (it never raises an error or yields). With
** patch 07 such requests are also serviced inside C pattern matching, so the
** script's output must stay identical to an unstressed run.
** Counts of hook runs in C / Lua context go to stderr.
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

static lua_State *gL;
static volatile LONG pending, stop, n_c, n_lua;

static int rearm;  /* STRESS_REARM=1: keep the request pending forever. */
static long ncalls;

static void hook(lua_State *L, lua_Debug *ar)
{
  lua_Debug a; int i;
  (void)ar;
  if (rearm) {  /* Request stays pending: the hook runs at every poll. */
    luaJIT_interrupt(L);
    if (lua_getstack(L, 0, &a) && lua_getinfo(L, "S", &a) && a.what[0] == 'C') n_c++; else n_lua++;
    luaL_checkstack(L, 200, "stress");
    for (i = 0; i < 200; i++) lua_pushinteger(L, i);
    lua_pop(L, 200);
    if (++ncalls % 512 == 0) { lua_pushfstring(L, "g%d", (int)ncalls); lua_pop(L, 1); lua_gc(L, LUA_GCCOLLECT, 0); }
    return;
  }
  if (!InterlockedExchange(&pending, 0)) return;
  if (lua_getstack(L, 0, &a) && lua_getinfo(L, "S", &a) && a.what[0] == 'C')
    InterlockedIncrement(&n_c);
  else
    InterlockedIncrement(&n_lua);
  luaL_checkstack(L, 3000, "stress");
  for (i = 0; i < 3000; i++) lua_pushfstring(L, "g%d", i);
  lua_pop(L, 3000);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static DWORD WINAPI periodic(LPVOID p)
{
  int ms = (int)(INT_PTR)p;
  while (!stop) { Sleep(ms); InterlockedExchange(&pending, 1); luaJIT_interrupt(gL); }
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L; int i, st, ms = getenv("STRESS_MS") ? atoi(getenv("STRESS_MS")) : 0;
  HANDLE th = NULL;
  timeBeginPeriod(1);
  if (argc < 2) { fprintf(stderr, "usage: runlua file [args]\n"); return 2; }
  L = gL = luaL_newstate();
  luaL_openlibs(L);
  rearm = getenv("STRESS_REARM") != NULL;
  lua_sethook(L, hook, LUA_MASKCOUNT, 10000);
  if (rearm) luaJIT_interrupt(L);
  if (luaL_loadfile(L, argv[1])) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
  for (i = 2; i < argc; i++) lua_pushstring(L, argv[i]);
  if (ms > 0) th = CreateThread(NULL, 0, periodic, (LPVOID)(INT_PTR)ms, 0, NULL);
  st = lua_pcall(L, argc - 2, 0, 0);
  stop = 1;
  if (th) WaitForSingleObject(th, INFINITE);
  fflush(stdout);
  if (st) fprintf(stderr, "error: %s\n", lua_tostring(L, -1));
  fprintf(stderr, "hook runs with request: in C %ld, in Lua %ld\n", n_c, n_lua);
  lua_close(L);
  return st ? 1 : 0;
}
