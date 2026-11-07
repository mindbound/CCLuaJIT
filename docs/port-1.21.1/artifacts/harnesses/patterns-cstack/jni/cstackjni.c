#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <jni.h>
#include <stdio.h>
#include <stdint.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

static JNIEnv *genv;  /* Only valid during run() on the calling thread. */
static jclass gcls;
static jmethodID gmid;

static int l_ccall(lua_State *L) { lua_call(L, lua_gettop(L) - 1, LUA_MULTRET); return lua_gettop(L); }

/* jcall(n): JNI upcall into Java (javaDepth(n)). Java exceptions (e.g. a
** StackOverflowError) become Lua errors after the JNI call has returned. */
static int l_jcall(lua_State *L)
{
  jint r = (*genv)->CallStaticIntMethod(genv, gcls, gmid, (jint)luaL_checkinteger(L, 1));
  if ((*genv)->ExceptionCheck(genv)) {
    (*genv)->ExceptionClear(genv);
    return luaL_error(L, "Java exception in upcall");
  }
  lua_pushinteger(L, r);
  return 1;
}

JNIEXPORT jstring JNICALL Java_CStackJni_run(JNIEnv *env, jclass cls, jstring jsrc, jint marginKB, jboolean guard)
{
  const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
  lua_State *L = luaL_newstate();
  ULONG_PTR lo, hi;
  char buf[300];
  int st;
  genv = env; gcls = cls;
  gmid = (*env)->GetStaticMethodID(env, cls, "javaDepth", "(I)I");
  luaL_openlibs(L);
  lua_register(L, "ccall", l_ccall);
  lua_register(L, "jcall", l_jcall);
  GetCurrentThreadStackLimits(&lo, &hi);
  if (guard) luaJIT_setcstacklimit(L, (void *)lo, (void *)(lo + (ULONG_PTR)marginKB * 1024));
  lua_pushinteger(L, 0); lua_setglobal(L, "D");
  st = luaL_loadstring(L, src);
  if (!st) st = lua_pcall(L, 0, 1, 0);
  lua_getglobal(L, "D");
  snprintf(buf, sizeof(buf), "%s depth=%d (thread stack %lu KB): %.90s", st ? "error" : "ok", (int)lua_tointeger(L, -1),
	   (unsigned long)((hi - lo) / 1024), lua_isstring(L, -2) ? lua_tostring(L, -2) : "?");
  lua_close(L);
  (*env)->ReleaseStringUTFChars(env, jsrc, src);
  return (*env)->NewStringUTF(env, buf);
}
