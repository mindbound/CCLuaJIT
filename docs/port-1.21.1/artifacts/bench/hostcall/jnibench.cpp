// Host-call overhead benchmark: LuaJIT -> C, LuaJIT -> C -> JNI -> Java (several marshalling styles).
// The "boxed" paths copy the logic of CCLuaJIT's invoke_java_fn / to_java_values / to_lua_values
// (src/main/cpp/main.cpp on branch 1.20.1_CCT, lines ~259-466) as faithfully as possible.
#include <jni.h>
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"
}

#define JV JNI_VERSION_1_8

static JavaVM *jvm;
static JNIEnv *g_env;           // cached env for the "lean" paths (single-threaded benchmark)
static jobject g_machine;       // JniBench instance: plays the role of LuaJITMachine and of the API object

static jclass bench_class, object_class, number_class, double_class, boolean_class, string_class, bytearray_class;
static jmethodID decode_string_id, doublevalue_id, double_valueof_id, boolean_valueof_id, booleanvalue_id;
static jmethodID call_method_id, call_port_id;
static jfieldID yield_requested_id;
static jmethodID lean_nop_id, lean_num_id, lean_str_id, lean_bytes_id, lean_buf_id, lean_strout_id, lean_bytesout_id, lean_blit3_id;

static jclass gref(JNIEnv *env, const char *n) { jclass c = env->FindClass(n); return c ? (jclass) env->NewGlobalRef(c) : 0; }

// ---------------------------------------------------------------------------------------------
// Faithful copies of CCLuaJIT conversion helpers (subset: nil/number/boolean/string/byte[])
// ---------------------------------------------------------------------------------------------
static jobject to_java_value(JNIEnv *env, lua_State *L) {
    int t = lua_type(L, -1);
    switch (t) {
        case LUA_TNIL: case LUA_TNONE: lua_pop(L, 1); return 0;
        case LUA_TNUMBER: {
            lua_Number n = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return env->CallStaticObjectMethod(double_class, double_valueof_id, n);
        }
        case LUA_TBOOLEAN: {
            int b = lua_toboolean(L, -1);
            lua_pop(L, 1);
            return env->CallStaticObjectMethod(boolean_class, boolean_valueof_id, b);
        }
        case LUA_TSTRING: {
            size_t len;
            const char *cstr = lua_tolstring(L, -1, &len);
            jbyteArray bytes = env->NewByteArray(len);
            env->SetByteArrayRegion(bytes, 0, len, (jbyte *) cstr);
            jobject result = env->CallStaticObjectMethod(bench_class, decode_string_id, bytes);
            lua_pop(L, 1);
            return result;
        }
        default:
            luaL_error(L, "unsupported type in benchmark");
    }
    return 0;
}

static jobjectArray to_java_values(JNIEnv *env, lua_State *L, int n) {
    jobjectArray result = env->NewObjectArray(n, object_class, 0);
    for (int i = n; i > 0; i--) env->SetObjectArrayElement(result, i - 1, to_java_value(env, L));
    return result;
}

static void to_lua_value(JNIEnv *env, lua_State *L, jobject value) {
    if (value == 0) {
        lua_pushnil(L);
    } else if (env->IsInstanceOf(value, number_class)) {
        jdouble n = env->CallDoubleMethod(value, doublevalue_id);
        lua_pushnumber(L, n);
    } else if (env->IsInstanceOf(value, boolean_class)) {
        jboolean b = env->CallBooleanMethod(value, booleanvalue_id);
        lua_pushboolean(L, b);
    } else if (env->IsInstanceOf(value, string_class)) {
        jstring str = (jstring) value;
        const char *cstr = env->GetStringUTFChars(str, JNI_FALSE);
        lua_pushstring(L, cstr);
        env->ReleaseStringUTFChars(str, cstr);
    } else if (env->IsInstanceOf(value, bytearray_class)) {
        jbyteArray a = (jbyteArray) value;
        jsize alen = env->GetArrayLength(a);
        jbyte *ca = env->GetByteArrayElements(a, JNI_FALSE);
        char *rca = (char *) malloc(alen + 1);
        memcpy(rca, ca, alen);
        rca[alen] = 0;
        lua_pushstring(L, rca);
        free(rca);
        env->ReleaseByteArrayElements(a, ca, JNI_ABORT);
    } else {
        luaL_error(L, "unsupported java type in benchmark");
    }
}

static void to_lua_values(JNIEnv *env, lua_State *L, jobjectArray values) {
    jsize len = env->GetArrayLength(values);
    for (jsize i = 0; i < len; i++) {
        jobject elem = env->GetObjectArrayElement(values, i);
        to_lua_value(env, L, elem);
        env->DeleteLocalRef(elem);
    }
}

// Shared body of CCLuaJIT's invoke_java_fn. `fixed` adds Push/PopLocalFrame so local refs don't leak.
static int invoke_common(lua_State *L, jmethodID mid, int index, bool fixed) {
    JNIEnv *env;
    if (jvm->GetEnv((void **) &env, JV) != JNI_OK) return luaL_error(L, "no env");
    if (fixed) env->PushLocalFrame(16);
    jobjectArray arguments = to_java_values(env, L, lua_gettop(L));
    jobjectArray results = (jobjectArray) env->CallObjectMethod(g_machine, mid, g_machine, index, arguments);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (fixed) env->PopLocalFrame(0);
        return luaL_error(L, "java exception");
    }
    jsize c = 0;
    if (results) {
        to_lua_values(env, L, results);
        c = env->GetArrayLength(results);
    }
    if (env->GetBooleanField(g_machine, yield_requested_id)) {
        env->SetBooleanField(g_machine, yield_requested_id, 0);
    }
    if (fixed) env->PopLocalFrame(0);
    return c;
}

// index: 0 = num(x)->x+1, 1 = echo(args), 2 = strout()->100-char String, 3 = strin(s), 4 = nop(), 5 = bytes0to255()
// boxed_leaky: exactly CCLuaJIT (no local-ref cleanup); boxed: same + local frame; port_*: Java side goes through
// CC:T 1.121.0 LuaMethod.apply(target, context, new ObjectArguments(args)).getResult()
static int boxed_leaky_num(lua_State *L) { return invoke_common(L, call_method_id, 0, false); }
static int boxed_num(lua_State *L) { return invoke_common(L, call_method_id, 0, true); }
static int boxed_echo(lua_State *L) { return invoke_common(L, call_method_id, 1, true); }
static int boxed_strout(lua_State *L) { return invoke_common(L, call_method_id, 2, true); }
static int boxed_strin(lua_State *L) { return invoke_common(L, call_method_id, 3, true); }
static int boxed_nop(lua_State *L) { return invoke_common(L, call_method_id, 4, true); }
static int boxed_bytes256(lua_State *L) { return invoke_common(L, call_method_id, 5, true); }
static int port_num(lua_State *L) { return invoke_common(L, call_port_id, 0, true); }
static int port_strout(lua_State *L) { return invoke_common(L, call_port_id, 2, true); }
static int port_strin(lua_State *L) { return invoke_common(L, call_port_id, 3, true); }
static int port_nop(lua_State *L) { return invoke_common(L, call_port_id, 4, true); }
static int boxed_blit3(lua_State *L) { return invoke_common(L, call_method_id, 6, true); }
static int port_blit3(lua_State *L) { return invoke_common(L, call_port_id, 6, true); }

// ---------------------------------------------------------------------------------------------
// Lean JNI paths: primitive signatures, cached env, no Object[] / boxing
// ---------------------------------------------------------------------------------------------
static int lean_nop(lua_State *L) {
    g_env->CallStaticVoidMethod(bench_class, lean_nop_id);
    return 0;
}
static int lean_num(lua_State *L) {
    double d = lua_tonumber(L, 1);
    lua_pushnumber(L, g_env->CallStaticDoubleMethod(bench_class, lean_num_id, d));
    return 1;
}
// string in as java.lang.String with bytes mapped 1:1 to chars 0-255 (CC:T's convention)
static int lean_str(lua_State *L) {
    size_t len;
    const unsigned char *s = (const unsigned char *) lua_tolstring(L, 1, &len);
    jchar buf[512];
    jchar *b = len <= 512 ? buf : (jchar *) malloc(len * sizeof(jchar));
    for (size_t i = 0; i < len; i++) b[i] = s[i];
    jstring js = g_env->NewString(b, (jsize) len);
    if (b != buf) free(b);
    g_env->CallStaticVoidMethod(bench_class, lean_str_id, js);
    g_env->DeleteLocalRef(js);
    return 0;
}
// string in as byte[] copy
static int lean_bytes(lua_State *L) {
    size_t len;
    const char *s = lua_tolstring(L, 1, &len);
    jbyteArray a = g_env->NewByteArray((jsize) len);
    g_env->SetByteArrayRegion(a, 0, (jsize) len, (const jbyte *) s);
    g_env->CallStaticVoidMethod(bench_class, lean_bytes_id, a);
    g_env->DeleteLocalRef(a);
    return 0;
}
// string in as zero-copy direct ByteBuffer over the Lua string (callee must not retain it)
static int lean_buf(lua_State *L) {
    size_t len;
    const char *s = lua_tolstring(L, 1, &len);
    jobject bb = g_env->NewDirectByteBuffer((void *) s, (jlong) len);
    g_env->CallStaticVoidMethod(bench_class, lean_buf_id, bb);
    g_env->DeleteLocalRef(bb);
    return 0;
}
// String out -> GetStringRegion -> bytes (chars 0-255) -> lua_pushlstring
static int lean_strout(lua_State *L) {
    jstring js = (jstring) g_env->CallStaticObjectMethod(bench_class, lean_strout_id);
    jsize len = g_env->GetStringLength(js);
    jchar buf[512]; char out[512];
    if (len > 512) return luaL_error(L, "too long");
    g_env->GetStringRegion(js, 0, len, buf);
    for (jsize i = 0; i < len; i++) out[i] = (char) buf[i];
    lua_pushlstring(L, out, len);
    g_env->DeleteLocalRef(js);
    return 1;
}
// byte[] out -> GetByteArrayRegion -> lua_pushlstring
// term.blit-like: three strings in as three byte[] copies
static int lean_blit3(lua_State *L) {
    jbyteArray a[3];
    for (int i = 0; i < 3; i++) {
        size_t len; const char *s = lua_tolstring(L, i + 1, &len);
        a[i] = g_env->NewByteArray((jsize) len);
        g_env->SetByteArrayRegion(a[i], 0, (jsize) len, (const jbyte *) s);
    }
    g_env->CallStaticVoidMethod(bench_class, lean_blit3_id, a[0], a[1], a[2]);
    for (int i = 0; i < 3; i++) g_env->DeleteLocalRef(a[i]);
    return 0;
}
static int lean_bytesout(lua_State *L) {
    jbyteArray a = (jbyteArray) g_env->CallStaticObjectMethod(bench_class, lean_bytesout_id);
    jsize len = g_env->GetArrayLength(a);
    char out[512];
    if (len > 512) return luaL_error(L, "too long");
    g_env->GetByteArrayRegion(a, 0, len, (jbyte *) out);
    lua_pushlstring(L, out, len);
    g_env->DeleteLocalRef(a);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Pure C functions (no Java) + FFI export
// ---------------------------------------------------------------------------------------------
static int c_nop(lua_State *L) { return 0; }
static int c_num(lua_State *L) { lua_pushnumber(L, lua_tonumber(L, 1) + 1); return 1; }
static volatile size_t c_sink;
static int c_strin(lua_State *L) { size_t len; lua_tolstring(L, 1, &len); c_sink += len; return 0; }
static char STR100[101];
static int c_strout(lua_State *L) { lua_pushlstring(L, STR100, 100); return 1; }
extern "C" __declspec(dllexport) double hb_ffi_num(double x) { return x + 1; }

// ---------------------------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------------------------
static double qpc_freq;
static int hb_now(lua_State *L) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    lua_pushnumber(L, (double) t.QuadPart / qpc_freq);
    return 1;
}
static int hb_mem(lua_State *L) {   // process private bytes in MB
    PROCESS_MEMORY_COUNTERS_EX pmc; pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &pmc, sizeof pmc);
    lua_pushnumber(L, (double) pmc.PrivateUsage / (1024.0 * 1024.0));
    return 1;
}
static int hb_out(lua_State *L) { printf("%s\n", luaL_checkstring(L, 1)); fflush(stdout); return 0; }

static const luaL_Reg hb_funcs[] = {
    {"now", hb_now}, {"mem", hb_mem}, {"out", hb_out},
    {"c_nop", c_nop}, {"c_num", c_num}, {"c_strin", c_strin}, {"c_strout", c_strout},
    {"boxed_leaky_num", boxed_leaky_num}, {"boxed_num", boxed_num}, {"boxed_echo", boxed_echo},
    {"boxed_strout", boxed_strout}, {"boxed_strin", boxed_strin}, {"boxed_nop", boxed_nop},
    {"boxed_bytes256", boxed_bytes256},
    {"port_num", port_num}, {"port_strout", port_strout}, {"port_strin", port_strin}, {"port_nop", port_nop},
    {"lean_nop", lean_nop}, {"lean_num", lean_num}, {"lean_str", lean_str}, {"lean_bytes", lean_bytes},
    {"lean_buf", lean_buf}, {"lean_strout", lean_strout}, {"lean_bytesout", lean_bytesout},
    {"lean_blit3", lean_blit3}, {"boxed_blit3", boxed_blit3}, {"port_blit3", port_blit3},
    {NULL, NULL}
};

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    jvm = vm;
    JNIEnv *env;
    if (vm->GetEnv((void **) &env, JV) != JNI_OK) return JV;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpc_freq = (double) f.QuadPart;
    memset(STR100, 'x', 100);
    bench_class = gref(env, "JniBench");
    object_class = gref(env, "java/lang/Object");
    number_class = gref(env, "java/lang/Number");
    double_class = gref(env, "java/lang/Double");
    boolean_class = gref(env, "java/lang/Boolean");
    string_class = gref(env, "java/lang/String");
    bytearray_class = gref(env, "[B");
    decode_string_id = env->GetStaticMethodID(bench_class, "decodeString", "([B)Ljava/lang/String;");
    doublevalue_id = env->GetMethodID(number_class, "doubleValue", "()D");
    double_valueof_id = env->GetStaticMethodID(double_class, "valueOf", "(D)Ljava/lang/Double;");
    boolean_valueof_id = env->GetStaticMethodID(boolean_class, "valueOf", "(Z)Ljava/lang/Boolean;");
    booleanvalue_id = env->GetMethodID(boolean_class, "booleanValue", "()Z");
    call_method_id = env->GetMethodID(bench_class, "callMethod", "(Ljava/lang/Object;I[Ljava/lang/Object;)[Ljava/lang/Object;");
    call_port_id = env->GetMethodID(bench_class, "callPort", "(Ljava/lang/Object;I[Ljava/lang/Object;)[Ljava/lang/Object;");
    yield_requested_id = env->GetFieldID(bench_class, "yieldRequested", "Z");
    lean_nop_id = env->GetStaticMethodID(bench_class, "leanNop", "()V");
    lean_num_id = env->GetStaticMethodID(bench_class, "leanNum", "(D)D");
    lean_str_id = env->GetStaticMethodID(bench_class, "leanStr", "(Ljava/lang/String;)V");
    lean_bytes_id = env->GetStaticMethodID(bench_class, "leanBytes", "([B)V");
    lean_buf_id = env->GetStaticMethodID(bench_class, "leanBuf", "(Ljava/nio/ByteBuffer;)V");
    lean_strout_id = env->GetStaticMethodID(bench_class, "leanStrOut", "()Ljava/lang/String;");
    lean_bytesout_id = env->GetStaticMethodID(bench_class, "leanBytesOut", "()[B");
    lean_blit3_id = env->GetStaticMethodID(bench_class, "leanBlit3", "([B[B[B)V");
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    return JV;
}

// static native String run(JniBench machine, String script, boolean jitOn)
extern "C" JNIEXPORT jstring JNICALL Java_JniBench_run(JNIEnv *env, jclass, jobject machine, jstring script, jboolean jitOn) {
    g_env = env;
    g_machine = machine;
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    luaL_register(L, "hb", hb_funcs);
    lua_pop(L, 1);
    lua_pushstring(L, jitOn ? "jit" : "joff");
    lua_setglobal(L, "MODE");
    if (!jitOn) {
        luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    }
    const char *src = env->GetStringUTFChars(script, 0);
    int err = luaL_loadbuffer(L, src, strlen(src), "@hostcall.lua");
    env->ReleaseStringUTFChars(script, src);
    if (!err) err = lua_pcall(L, 0, 0, 0);
    jstring ret;
    if (err) ret = env->NewStringUTF(lua_tostring(L, -1));
    else ret = env->NewStringUTF("ok");
    lua_close(L);
    return ret;
}
