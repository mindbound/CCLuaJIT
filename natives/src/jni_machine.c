/*
** jni_machine.c: the native methods of io.github.mindbound.cclj.natives.NativeMachine and the host function that
** calls back into Java (docs/port-1.21.1/M1.md section 3.9). The machine itself is in machine.c (cclj_machine.h).
**
** Rules for every function in this file:
** - Never include a Lua header. Every function here is NO_LUA: it makes no Lua call, so no Lua error can be
**   raised here or unwind through a JVM frame. Lua runs only inside cm_load and cm_resume, under their own
**   lua_cpcall, and calls Java only through jni_host_call, in phase 2 of api_call.
** - No JNI_OnLoad, no class lookup by name, no global references and no mutable static data. The method ID of
**   NativeMachine.upcall is looked up once per nResume, from the class of the object it was called on. JNIEnv and
**   the NativeMachine reference live in a JniHost on nResume's stack, and only for that call (J6).
** - J1: every JNI call that can throw is followed by ExceptionCheck or a NULL check before the next JNI call.
**   J2: jni_host_call pushes one local frame and pops it through one exit. J3: only the JNI functions the lint
**   allows. J4: no exception is pending when a native method returns. J5: every jsize conversion is bounded by
**   CM_WIRE_MAX (or CM_MSG_MAX + 4).
** - The jlong handle is the cm_machine pointer. Java passes it only while it owns the machine (NativeMachine's
**   lock and its running flag); cm_* calls check the busy flag again and return CM_ST_BUSY, which these functions
**   pass on as it is, without reading the message (cclj_machine.h, thread rules).
*/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>

#include "cclj_machine.h"

/* What jni_host_call needs: one per nResume, on its stack (J6). */
typedef struct JniHost {
  JNIEnv *env;
  jobject self;        /* the NativeMachine: nResume's own local reference, valid until nResume returns */
  jmethodID upcall;    /* NativeMachine.upcall(int, byte[]): byte[] */
} JniHost;

/* NO_LUA. A jlong handle as the machine pointer. */
static cm_machine *machine_of_handle(jlong h)
{
  return (cm_machine *)(intptr_t)h;
}

/* NO_LUA (pure). Makes room for n bytes in the reply buffer, which is empty (rep->len is 0 on entry). Plain
** realloc: machine.c frees the buffer with free. At most CM_WIRE_MAX bytes. 0 = it cannot grow.
*/
static int rep_reserve(cm_buf *b, size_t n)
{
  size_t cap;
  unsigned char *p;
  if (n > CM_WIRE_MAX) return 0;
  if (n <= b->cap && b->data != NULL) return 1;
  cap = b->cap >= 256 ? b->cap : 256;
  while (cap < n) cap = cap > CM_WIRE_MAX / 2 ? CM_WIRE_MAX : cap * 2;
  p = (unsigned char *)realloc(b->data, cap);
  if (p == NULL) return 0;
  b->data = p;
  b->cap = cap;
  return 1;
}

/* NO_LUA. The cm_host_fn of nResume: calls NativeMachine.upcall(fn, request) and copies the byte[] it returns into
** *rep. One local frame per call, popped at the single exit (J2), so a resume with any number of host calls keeps
** no local reference. Returns CM_HOST_OK or a CM_HOST_E_* code; no exception is pending on return (J4).
*/
static int jni_host_call(void *hp, uint32_t fn, const cm_buf *req, cm_buf *rep)
{
  JniHost *host = (JniHost *)hp;
  JNIEnv *env = host->env;
  jbyteArray jreq, jrep;
  jsize n;
  int rc;

  if (req->len > CM_WIRE_MAX) return CM_HOST_E_JNI;   /* J5; machine.c never builds a larger request */
  if ((*env)->PushLocalFrame(env, 4) != 0) {
    (*env)->ExceptionClear(env);
    return CM_HOST_E_JNI;
  }
  jreq = (*env)->NewByteArray(env, (jsize)req->len);
  if (jreq == NULL) { rc = CM_HOST_E_JNI; goto out; }   /* OutOfMemoryError pending */
  if (req->len > 0) {
    (*env)->SetByteArrayRegion(env, jreq, 0, (jsize)req->len, (const jbyte *)req->data);
    if ((*env)->ExceptionCheck(env)) { rc = CM_HOST_E_JNI; goto out; }
  }
  jrep = (jbyteArray)(*env)->CallObjectMethod(env, host->self, host->upcall, (jint)fn, jreq);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);   /* prints the exception and its stack trace to stderr, and clears it */
    rc = CM_HOST_E_JAVA;
    goto out;
  }
  if (jrep == NULL) { rc = CM_HOST_E_NULL; goto out; }
  n = (*env)->GetArrayLength(env, jrep);
  if (n < 0 || (size_t)n > CM_WIRE_MAX) { rc = CM_HOST_E_TOOBIG; goto out; }
  if (!rep_reserve(rep, (size_t)n)) { rc = CM_HOST_E_NOMEM; goto out; }
  if (n > 0) {
    (*env)->GetByteArrayRegion(env, jrep, 0, n, (jbyte *)rep->data);
    if ((*env)->ExceptionCheck(env)) { rc = CM_HOST_E_JNI; goto out; }
  }
  rep->len = (size_t)n;
  rc = CM_HOST_OK;
out:
  /* E_JAVA: ExceptionDescribe has cleared it; clearing again costs nothing and keeps J4 even if printing threw. */
  if (rc == CM_HOST_E_JNI || rc == CM_HOST_E_JAVA) (*env)->ExceptionClear(env);
  (*env)->PopLocalFrame(env, NULL);
  return rc;
}

/* NO_LUA. Copies a non-null Java byte array into a new malloc block (*out, *len; at least one byte is allocated,
** so *out is never NULL on success). 0 = longer than CM_WIRE_MAX, out of memory, or a JNI failure; no exception is
** pending then and nothing is allocated.
*/
static int copy_array(JNIEnv *env, jbyteArray a, unsigned char **out, size_t *len)
{
  jsize n = (*env)->GetArrayLength(env, a);
  unsigned char *p;
  *out = NULL;
  *len = 0;
  if (n < 0 || (size_t)n > CM_WIRE_MAX) return 0;
  p = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
  if (p == NULL) return 0;
  if (n > 0) {
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)p);
    if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      free(p);
      return 0;
    }
  }
  *out = p;
  *len = (size_t)n;
  return 1;
}

/* NO_LUA. NativeMachine.nAbi(): the native ABI version, which NativeLibrary compares with NativeMachine.ABI once
** after the library is loaded.
*/
JNIEXPORT jint JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nAbi(JNIEnv *env, jclass cls)
{
  (void)env; (void)cls;
  return CCLJ_ABI;
}

/* NO_LUA. NativeMachine.nNew(memLimit, idKey): a new machine whose Lua heap is capped at memLimit bytes, or 0
** (memLimit <= 0, or out of memory). No JNI call.
*/
JNIEXPORT jlong JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nNew(JNIEnv *env, jclass cls,
                                                                                jlong memLimit, jlong idKey)
{
  size_t limit;
  (void)env; (void)cls;
  if (memLimit <= 0) return 0;
  limit = (uint64_t)memLimit < (uint64_t)SIZE_MAX ? (size_t)memLimit : SIZE_MAX;
  return (jlong)(intptr_t)cm_new(limit, (uint64_t)idKey);
}

/* NO_LUA. NativeMachine.nLoad(h, spec, bios): copies both arrays and calls cm_load, and returns its status
** (CM_ST_BUSY included). CM_ST_INTERNAL for a zero handle, a null array, an array longer than CM_WIRE_MAX, or a
** failed copy (the message is then left as it was).
*/
JNIEXPORT jint JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nLoad(JNIEnv *env, jclass cls, jlong h,
                                                                                jbyteArray spec, jbyteArray bios)
{
  unsigned char *s = NULL, *b = NULL;
  size_t slen = 0, blen = 0;
  int st = CM_ST_INTERNAL;
  (void)cls;
  if (h == 0 || spec == NULL || bios == NULL) return CM_ST_INTERNAL;
  if (copy_array(env, spec, &s, &slen) && copy_array(env, bios, &b, &blen))
    st = cm_load(machine_of_handle(h), s, slen, b, blen);
  free(s);
  free(b);
  return st;
}

/* NO_LUA. NativeMachine.nResume(h, event): resumes the bios coroutine. Its host calls go to this.upcall through
** jni_host_call. A non-null event is copied and passed on: in S1, cm_resume refuses it with CM_ST_INTERNAL and the
** message "events are not implemented yet (S2)". Returns cm_resume's status (CM_ST_BUSY included), or
** CM_ST_INTERNAL with the message left as it was for a zero handle, or when the event cannot be copied.
*/
JNIEXPORT jint JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nResume(JNIEnv *env, jobject self,
                                                                                  jlong h, jbyteArray event)
{
  JniHost host;
  jclass cls;
  jmethodID upcall;
  unsigned char *ev = NULL;
  size_t evlen = 0;
  int st;
  if (h == 0) return CM_ST_INTERNAL;
  if (event != NULL && !copy_array(env, event, &ev, &evlen)) return CM_ST_INTERNAL;
  cls = (*env)->GetObjectClass(env, self);
  if (cls == NULL) {
    (*env)->ExceptionClear(env);
    free(ev);
    return CM_ST_INTERNAL;
  }
  upcall = (*env)->GetMethodID(env, cls, "upcall", "(I[B)[B");
  if (upcall == NULL) {   /* NoSuchMethodError pending */
    (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    free(ev);
    return CM_ST_INTERNAL;
  }
  (*env)->DeleteLocalRef(env, cls);
  host.env = env;
  host.self = self;
  host.upcall = upcall;
  st = cm_resume(machine_of_handle(h), jni_host_call, &host, ev, evlen);   /* ev is NULL for no event */
  free(ev);
  if ((*env)->ExceptionCheck(env)) {   /* never return with one pending (J4); jni_host_call leaves none */
    (*env)->ExceptionClear(env);
    st = CM_ST_INTERNAL;
  }
  return st;
}

/* NO_LUA. NativeMachine.nMessage(h): the text of the last cm_load or cm_resume that did not return CM_ST_BUSY, as a
** byte[] of a 4-byte little-endian traceback offset (0xFFFFFFFF for none) and then the message bytes. NULL, with no
** exception pending, if the array cannot be made; Java then shows "(no message)".
*/
JNIEXPORT jbyteArray JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nMessage(JNIEnv *env, jclass cls,
                                                                                         jlong h)
{
  const unsigned char *s;
  size_t len = 0, tb = SIZE_MAX;
  uint32_t off;
  unsigned char head[4];
  jbyteArray a;
  (void)cls;
  s = cm_message(machine_of_handle(h), &len, &tb);
  if (len > CM_MSG_MAX) len = CM_MSG_MAX;   /* J5; cm_message never gives more */
  off = (tb == SIZE_MAX || tb > len) ? UINT32_C(0xFFFFFFFF) : (uint32_t)tb;
  head[0] = (unsigned char)off;
  head[1] = (unsigned char)(off >> 8);
  head[2] = (unsigned char)(off >> 16);
  head[3] = (unsigned char)(off >> 24);
  a = (*env)->NewByteArray(env, (jsize)(4 + len));
  if (a == NULL) {   /* OutOfMemoryError pending */
    (*env)->ExceptionClear(env);
    return NULL;
  }
  (*env)->SetByteArrayRegion(env, a, 0, 4, (const jbyte *)head);
  if ((*env)->ExceptionCheck(env)) goto fail;
  if (len > 0) {
    (*env)->SetByteArrayRegion(env, a, 4, (jsize)len, (const jbyte *)s);
    if ((*env)->ExceptionCheck(env)) goto fail;
  }
  return a;
fail:
  (*env)->ExceptionClear(env);
  (*env)->DeleteLocalRef(env, a);
  return NULL;
}

/* NO_LUA. NativeMachine.nFree(h): cm_free. CM_ST_OK (also for a zero handle); CM_ST_BUSY when the machine is busy,
** or CM_ST_INTERNAL for a wrong magic (a test tripwire): then it is not freed and Java logs a leak. No JNI call.
*/
JNIEXPORT jint JNICALL Java_io_github_mindbound_cclj_natives_NativeMachine_nFree(JNIEnv *env, jclass cls, jlong h)
{
  (void)env; (void)cls;
  return cm_free(machine_of_handle(h));
}
