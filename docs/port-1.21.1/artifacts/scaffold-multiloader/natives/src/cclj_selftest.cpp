// Minimal JNI probe proving that LuaJIT can be statically linked into a single
// self-contained JNI library (one file per platform) and driven from Java 21.
//
// Java side: gay.vereena.cclj.natives.NativeProbe
//   static native String eval(String chunk);   // runs a Lua chunk, returns tostring(first result) or "ERR: ..."
//   static native String version();            // LUAJIT_VERSION + jit.status()
#include <jni.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"
}

static jstring to_jstring(JNIEnv *env, const char *s, size_t len) {
    // Build via byte[] + new String(bytes, UTF_8) would be more correct for arbitrary bytes; NewStringUTF is fine
    // for this ASCII probe.
    (void)len;
    return env->NewStringUTF(s);
}

extern "C" JNIEXPORT jstring JNICALL
Java_gay_vereena_cclj_natives_NativeProbe_eval(JNIEnv *env, jclass, jstring jchunk) {
    const char *chunk = env->GetStringUTFChars(jchunk, nullptr);
    lua_State *L = luaL_newstate();
    if (L == nullptr) {
        env->ReleaseStringUTFChars(jchunk, chunk);
        return env->NewStringUTF("ERR: luaL_newstate returned NULL");
    }
    luaL_openlibs(L);
    int rc = luaL_loadbuffer(L, chunk, strlen(chunk), "=probe");
    env->ReleaseStringUTFChars(jchunk, chunk);
    if (rc == 0) rc = lua_pcall(L, 0, 1, 0);

    jstring result;
    if (rc != 0) {
        size_t len = 0;
        const char *msg = lua_tolstring(L, -1, &len);
        lua_pushfstring(L, "ERR: %s", msg ? msg : "(non-string error)");
        result = env->NewStringUTF(lua_tostring(L, -1));
    } else {
        // tostring(result)
        lua_getglobal(L, "tostring");
        lua_insert(L, -2);
        lua_call(L, 1, 1);
        size_t len = 0;
        const char *s = lua_tolstring(L, -1, &len);
        result = to_jstring(env, s, len);
    }
    lua_close(L);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_gay_vereena_cclj_natives_NativeProbe_version(JNIEnv *env, jclass) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    const char *code =
        "local t = {jit.status()} "
        "local parts = {} for i = 2, #t do parts[#parts+1] = tostring(t[i]) end "
        "return string.format('%s | %s %s | jit=%s | %s', jit.version, jit.os, jit.arch, tostring(t[1]), table.concat(parts, ' '))";
    if (luaL_dostring(L, code) != 0) {
        jstring err = env->NewStringUTF(lua_tostring(L, -1));
        lua_close(L);
        return err;
    }
    jstring r = env->NewStringUTF(lua_tostring(L, -1));
    lua_close(L);
    return r;
}
