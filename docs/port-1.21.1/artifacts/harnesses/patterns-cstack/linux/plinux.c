/* Linux check of patch 07: interrupt pathological C pattern matching from another thread. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
static lua_State *gL; static volatile int pending, fired; static struct timespec treq, tfire;
static double us(struct timespec a, struct timespec b) { return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3; }
static void hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  if (!__sync_lock_test_and_set(&pending, 0)) return;
  clock_gettime(CLOCK_MONOTONIC, &tfire); fired = 1;
  luaL_error(L, "interrupted (yieldable=%d)", lua_isyieldable(L));
}
static void *intr(void *p) { (void)p; usleep(300000); clock_gettime(CLOCK_MONOTONIC, &treq); pending = 1; luaJIT_interrupt(gL); return NULL; }
int main(void) {
  static const char *cases[] = {
    "return string.find(string.rep('a', 40), string.rep('a*', 20) .. 'b')",
    "return string.gsub(string.rep('a', 1e6), '.-b', '')",
    "for m in string.gmatch(string.rep('a', 40), string.rep('a*', 20) .. 'b') do end",
    "local ok, e = pcall(string.match, string.rep('a', 40), string.rep('(a*)', 15) .. 'b') return 'pcall caught: ' .. tostring(e)",
    "local t = {} for i = 1, 3e6 do t[i] = (i * 7919) % 1000003 end table.sort(t, function(a, b) return a < b end) return 'sorted (Lua comparator: ordinary hook)'",
    NULL };
  for (int i = 0; cases[i]; i++) {
    pthread_t th; int st;
    gL = luaL_newstate(); luaL_openlibs(gL); lua_sethook(gL, hook, LUA_MASKCOUNT, 10000);
    fired = 0; pending = 0;
    luaL_loadstring(gL, cases[i]);
    pthread_create(&th, NULL, intr, NULL);
    st = lua_pcall(gL, 0, 1, 0);
    pthread_join(th, NULL);
    printf("case %d: status=%d fired=%d latency=%.1f us msg=%s\n", i, st, fired, fired ? us(treq, tfire) : -1.0, lua_tostring(gL, -1));
    lua_settop(gL, 0);
    st = luaL_dostring(gL, "return (('hello world'):gsub('o', '0'))");
    printf("  usable afterwards: %s\n", lua_tostring(gL, -1));
    lua_close(gL);
  }
  return 0;
}
