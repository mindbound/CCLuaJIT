/* oomwedge.c: why does a full GC fail with ERRMEM after OOM inside a coroutine? */
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
typedef struct { size_t used, cap; size_t lastn, lasto; int trace; } MemCap;
static void *cap_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  MemCap *m = (MemCap *)ud;
  if (nsize == 0) { if (ptr) { m->used -= osize; free(ptr); } return NULL; }
  if (ptr == NULL) osize = 0;
  if (nsize > osize && m->used + (nsize - osize) > m->cap) { m->lastn = nsize; m->lasto = osize; if (m->trace) printf("  FAIL alloc o=%zu n=%zu used=%zu\n", osize, nsize, m->used); return NULL; }
  { void *p = realloc(ptr, nsize); if (!p) return NULL; m->used = m->used - osize + nsize; return p; }
}
static int cp_gc(lua_State *L) { lua_gc(L, LUA_GCCOLLECT, 0); return 0; }
int main(int argc, char **argv) {
  MemCap m = {0, 32u << 20, 0, 0, 0};
  lua_State *L = lua_newstate(cap_alloc, &m), *co;
  int st;
  luaL_openlibs(L);
  co = lua_newthread(L);
  luaL_loadstring(co, argc > 1 ? argv[1] : "local t={} for i=1,1e9 do t[i]={i} end");
  st = lua_resume(co, 0);
  printf("resume st=%d msg=%s used=%zu lastfail n=%zu o=%zu\n", st, lua_tostring(co, -1), m.used, m.lastn, m.lasto);
  printf("co stack top=%d\n", lua_gettop(co));
  lua_pop(L, 1);
  m.trace = 1;
  st = lua_cpcall(L, cp_gc, NULL);
  printf("gc1 st=%d used=%zu\n", st, m.used); if (st) lua_pop(L, 1);
  m.cap += (size_t)atoi(getenv("HEAD")) << 10;
  st = lua_cpcall(L, cp_gc, NULL);
  printf("gc2 (cap+64MB) st=%d used=%zu\n", st, m.used); if (st) lua_pop(L, 1);
  m.cap -= (size_t)atoi(getenv("HEAD")) << 10;
  st = lua_cpcall(L, cp_gc, NULL);
  printf("gc3 st=%d used=%zu\n", st, m.used); if (st) lua_pop(L, 1);
  lua_close(L);
  return 0;
}
