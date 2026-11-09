/*
** cclj_machine.h: the internal C API of one CCLuaJIT machine (docs/port-1.21.1/M1.md section 3.5).
**
** machine.c implements it with Lua and never includes jni.h; jni_machine.c calls it from JNI and never includes a
** Lua header. So a Lua error cannot cross a JVM frame: the only path from Lua into Java is a cm_host_fn, which
** runs in phase 2 of api_call, where no Lua call happens. This header includes only <stddef.h> and <stdint.h>.
**
** Run-context tags. Every C function in machine.c and jni_machine.c carries one in a comment:
**   UNPROTECTED    makes only calls that cannot raise a Lua error (M1.md section 3.8). Pure functions that make
**                  no Lua call at all (the wire checkers, buffer helpers) are UNPROTECTED too.
**   PROTECTED      a lua_cpcall body: any Lua call in it may raise.
**   LUA_CFUNCTION  called by Lua: may raise in phases 1 and 3 only, never in phase 2 (the host call).
**   NO_LUA         jni_machine.c: makes no Lua call at all.
**
** Thread rules:
** - A machine may be used from any thread, but by one thread at a time. NativeMachine's Java monitor enforces
**   that; the C busy flag (an atomic compare-and-swap in cm_load, cm_resume and cm_free) is the second line of
**   defence: a call that finds the machine busy returns CM_ST_INTERNAL and leaves the Lua state and the
**   allocations alone. cm_load and cm_resume then set the message to "machine is busy" (when the owner runs on
**   another thread, which only a broken Java lock allows, that write races with the owner's message; every
**   write keeps the length within CM_MSG_MAX). cm_free writes nothing.
** - The library keeps no mutable static state: different machines may run on different threads at once.
** - A cm_host_fn runs on the thread that called cm_resume, in phase 2 of api_call. It must not call Lua. It may
**   call cm_resume, cm_load or cm_free on the same machine: they return CM_ST_INTERNAL (busy) and leave the Lua
**   state alone; cm_load and cm_resume set the message to "machine is busy", which the owner's call replaces
**   before it returns.
** - cm_message reads the text of the last call. It is valid until the next cm_* call on the machine.
**
** Wire format v1 (M1.md section 3.6). Integers are little-endian, numbers are IEEE-754 binary64:
**   value   := 00 NIL | 01 FALSE | 02 TRUE | 03 NUMBER f64 | 04 STRING u32 len, bytes
**              (tags 05..0F are reserved for S3 tables, back-references and objects)
**   request := u32 n, value*n                                    C -> host, one per host call
**   reply   := 00 u32 n, value*n                                 VALUES  host -> C
**            | 01 i32 level, value (NIL or STRING)               ERROR   host -> C
**              (kind 02 is reserved for the S5 CALLBACK)
**   spec    := u32 napi, api*                                    cm_load
**   api     := u32 nnames, str*nnames, u8 hasModule, [str module], u32 nmethods, method*
**   method  := str name, u8 flags                                bit 0 = MAY_YIELD; the other bits are 0
**   str     := u32 len, bytes
** Host functions are numbered in spec order, across all APIs, from 0. Decoders check every length against the
** remaining bytes in size_t arithmetic, reject unknown tags and kinds, and refuse bytes left over.
*/
#ifndef CCLJ_MACHINE_H
#define CCLJ_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#define CCLJ_ABI 1                 /* NativeMachine.ABI must match; checked once after System.load */

/* Status codes of cm_load, cm_resume and cm_free. */
#define CM_ST_OK        0          /* cm_load: bios compiled */
#define CM_ST_YIELD     1          /* -> MachineResult.OK; machine kept (S2: message = filter) */
#define CM_ST_RETURNED  2          /* -> GENERIC_ERROR; dead */
#define CM_ST_ERROR     3          /* uncaught error, OOM, load error -> error(msg) / MachineException; dead */
#define CM_ST_INTERNAL  4          /* misuse: busy, dead, bad handle, event in S1 -> internal error; dead */
/* S6a adds CM_ST_ABORTED 5 -> TIMEOUT */

/* Return codes of a cm_host_fn. Anything but CM_HOST_OK makes api_call raise a fixed level-0 error. */
#define CM_HOST_OK       0
#define CM_HOST_E_JNI    1         /* a JNI call failed (e.g. OutOfMemoryError); exception cleared */
#define CM_HOST_E_JAVA   2         /* the upcall threw; ExceptionDescribe (prints and clears) */
#define CM_HOST_E_NULL   3         /* the upcall returned null */
#define CM_HOST_E_TOOBIG 4         /* reply > CM_WIRE_MAX */
#define CM_HOST_E_NOMEM  5         /* reply buffer could not grow */

/* Limits. */
#define CM_WIRE_MAX (16u << 20)    /* max request/reply/spec/bios bytes (S3 revisits) */
#define CM_MSG_MAX  (64u << 10)    /* error/filter text is cut to this (DESIGN.md:219) */
#define CM_WIRE_MAX_FN   65536u    /* max host functions in a spec (all APIs together) */
#define CM_WIRE_MAX_NAME 256u      /* max bytes of an API, module or method name in a spec */

/* Wire tags (see the format above). */
#define CM_WIRE_NIL      0x00
#define CM_WIRE_FALSE    0x01
#define CM_WIRE_TRUE     0x02
#define CM_WIRE_NUMBER   0x03
#define CM_WIRE_STRING   0x04
#define CM_WIRE_VALUES   0x00      /* reply kind */
#define CM_WIRE_ERROR    0x01      /* reply kind */
#define CM_WIRE_MAY_YIELD 0x01     /* method flag */

typedef struct cm_machine cm_machine;
typedef struct cm_buf { unsigned char *data; size_t len, cap; } cm_buf;   /* malloc; owned by the machine */

/* NO_LUA. Called only from api_call phase 2. Writes the reply into *rep. *req holds the request; rep->len is 0 on
** entry. The host grows rep->data with realloc (the machine frees it with free), to at most CM_WIRE_MAX bytes.
** Returns CM_HOST_OK or a CM_HOST_E_* code.
*/
typedef int (*cm_host_fn)(void *host, uint32_t fn, const cm_buf *req, cm_buf *rep);

/* UNPROTECTED. A new machine whose Lua allocations are capped at mem_limit bytes. id_key keys the "<type>: %08x"
** hash of error values that are not strings, numbers, booleans or nil. NULL = out of memory.
*/
cm_machine *cm_new(size_t mem_limit, uint64_t id_key);

/* UNPROTECTED (the work runs under lua_cpcall). Opens the curated libraries, builds the API tables from spec and
** compiles bios as "@bios.lua" (text only) into the bios coroutine, without running it. Only once per machine.
** CM_ST_OK | CM_ST_ERROR (message: the load or memory error, or "CCLuaJIT: corrupt API table description")
** | CM_ST_INTERNAL.
*/
int  cm_load(cm_machine *m, const unsigned char *spec, size_t spec_len,
             const unsigned char *bios, size_t bios_len);

/* UNPROTECTED (the work runs under lua_cpcall). Resumes the bios coroutine. fn(host, ...) serves its host calls.
** S1: event must be NULL. CM_ST_YIELD | CM_ST_RETURNED | CM_ST_ERROR | CM_ST_INTERNAL.
*/
int  cm_resume(cm_machine *m, cm_host_fn fn, void *host,
               const unsigned char *event, size_t event_len);

/* UNPROTECTED. The text of the last cm_load or cm_resume: an error, an INTERNAL reason, or empty (S2: the filter).
** Valid until the next cm_* call. *tb_off is the offset of "\nstack traceback:" in the text (msg_tb), or SIZE_MAX
** when the text has no traceback.
*/
const unsigned char *cm_message(const cm_machine *m, size_t *len, size_t *tb_off);

/* UNPROTECTED. Frees the machine: CM_ST_OK, or CM_ST_INTERNAL if it is busy (then nothing is freed). */
int  cm_free(cm_machine *m);

#ifdef CCLJ_TEST
/* Test builds only (machine-test.sh); release builds have no cm_test_ symbol.
** cm_test_fail_after(n): the next n allocations of machine.c itself (cm_new's calloc and wire buffer growth)
**   succeed and every later one fails; n < 0 turns failures off. Returns the number of such allocations made
**   since the previous call. Process-wide.
** cm_test_state(m): the machine's main lua_State, for inspection between calls.
** cm_test_used(m): the bytes counted by the allocator cap.
** cm_test_limit(m, limit): sets the cap to limit bytes (between calls, or from a host function).
** cm_test_cotop(m, ...): lua_gettop of the bios coroutine just before and just after append_traceback in the
**   last ERROR (both -1 before the first).
** cm_test_cotop_resume(m): lua_gettop of the bios coroutine just before the last lua_resume (-1 before the first).
*/
long  cm_test_fail_after(long n);
void *cm_test_state(cm_machine *m);
size_t cm_test_used(const cm_machine *m);
void  cm_test_limit(cm_machine *m, size_t limit);
void  cm_test_cotop(const cm_machine *m, int *before, int *after);
int   cm_test_cotop_resume(const cm_machine *m);
#endif

#endif
