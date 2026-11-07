/* allocwrap.c: counting/capping wrapper around LuaJIT's internal allocator
** (lua_getallocf + lua_setallocf), vs. msvcrt realloc; leak check across
** create/close cycles. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
typedef struct { lua_Alloc f; void *ud; size_t used, cap; } Wrap;
static void *wrap_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  Wrap *w = (Wrap *)ud; size_t o = ptr ? osize : 0;
  if (nsize > o && w->used + (nsize - o) > w->cap) return NULL;
  { void *p = w->f(w->ud, ptr, osize, nsize);
    if (p || nsize == 0) w->used = w->used - o + nsize;
    return p; }
}
static void *crt_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  Wrap *w = (Wrap *)ud; size_t o = ptr ? osize : 0;
  if (nsize == 0) { if (ptr) { w->used -= osize; free(ptr); } return NULL; }
  if (nsize > o && w->used + (nsize - o) > w->cap) return NULL;
  { void *p = realloc(ptr, nsize); if (p) w->used = w->used - o + nsize; return p; }
}
static double now_ms(void){static LARGE_INTEGER f;LARGE_INTEGER c;if(!f.QuadPart)QueryPerformanceFrequency(&f);QueryPerformanceCounter(&c);return (double)c.QuadPart*1000.0/(double)f.QuadPart;}
static size_t priv(void){PROCESS_MEMORY_COUNTERS_EX pm; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&pm,sizeof(pm)); return pm.PrivateUsage;}
static const char *BENCH = "local s=0 for r=1,20 do local t={} for i=1,2e5 do t[i]={i,i} end s=s+#t end return s";
static lua_State *mk(int mode, Wrap *w) {
  lua_State *L;
  if (mode == 0) L = luaL_newstate();
  else if (mode == 1) { L = luaL_newstate(); w->f = lua_getallocf(L, &w->ud); w->used = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024 + lua_gc(L, LUA_GCCOUNTB, 0); lua_setallocf(L, wrap_alloc, w); }
  else L = lua_newstate(crt_alloc, w);
  luaL_openlibs(L); return L;
}
static void cl(lua_State *L, int mode, Wrap *w) { if (mode == 1) lua_setallocf(L, w->f, w->ud); lua_close(L); }
int main(void) {
  const char *names[] = {"luaL_newstate (lj_alloc)", "lj_alloc + counting wrapper", "lua_newstate(msvcrt realloc)"};
  int mode, i;
  for (mode = 0; mode < 3; mode++) {
    Wrap w = {0, 0, 0, (size_t)1 << 32};
    lua_State *L = mk(mode, &w); double t0 = now_ms();
    luaL_loadstring(L, BENCH); lua_pcall(L, 0, 1, 0);
    printf("R AW bench %-30s %.0f ms\n", names[mode], now_ms() - t0);
    cl(L, mode, &w);
  }
  for (mode = 0; mode < 3; mode++) {
    size_t before = priv();
    for (i = 0; i < 300; i++) {
      Wrap w = {0, 0, 0, (size_t)64 << 20};
      lua_State *L = mk(mode, &w);
      luaL_loadstring(L, "local t={} for i=1,2e5 do t[i]={i} end return #t"); lua_pcall(L, 0, 1, 0);
      cl(L, mode, &w);
    }
    printf("R AW leakcheck %-30s private bytes delta after 300 create/close cycles: %+.1f MB\n", names[mode], ((double)priv() - (double)before) / 1048576.0);
  }
  { /* cap enforcement with the wrapper */
    Wrap w = {0, 0, 0, (size_t)32 << 20}; lua_State *L = mk(1, &w); int st;
    luaL_loadstring(L, "local t={} for i=1,1e9 do t[i]={i} end"); st = lua_pcall(L, 0, 0, 0);
    printf("R AW wrapper cap=32MB: st=%d msg=%s used=%.1fMB\n", st, lua_tostring(L, -1), w.used / 1048576.0);
    lua_pop(L, 1); lua_gc(L, LUA_GCCOLLECT, 0);
    printf("R AW after gc used=%.1fMB gccount=%dKB\n", w.used / 1048576.0, lua_gc(L, LUA_GCCOUNT, 0));
    cl(L, 1, &w);
  }
  return 0;
}
