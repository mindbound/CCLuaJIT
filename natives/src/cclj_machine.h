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
**   PROTECTED      a lua_cpcall body, or a function that runs only inside one: any Lua call in it may raise.
**   LUA_CFUNCTION  api_call (called by Lua), or a function that only api_call calls: may raise in phases 1 and 3
**                  only, never in phase 2 (the host call).
**   NO_LUA         jni_machine.c: makes no Lua call at all.
** raise_text carries both PROTECTED and LUA_CFUNCTION, because both kinds of function call it.
**
** Thread rules:
** - A machine may be used from any thread, but by one thread at a time, and only while it is alive (until
**   cm_free returns CM_ST_OK). NativeMachine's Java monitor enforces both.
** - The C busy flag (an atomic compare-and-swap in cm_load, cm_resume and cm_free) is a second line of defence
**   against a second call while the machine is alive. That call returns CM_ST_BUSY and writes nothing into the
**   machine: the owner's Lua state, allocations and message stay as they are. The message belongs to the owner,
**   which may be writing it on another thread, so a caller that gets CM_ST_BUSY must not call cm_message.
** - The flag gives no protection against a cm_free on another thread, or a call after cm_free: the caller must
**   keep the machine alive. magic is a test tripwire, read without synchronization, not a defence.
** - The library keeps no mutable static state: different machines may run on different threads at once.
** - A cm_host_fn runs on the thread that called cm_resume, in phase 2 of api_call. It must not call Lua. It may
**   call cm_resume, cm_load or cm_free on the same machine: they return CM_ST_BUSY and change nothing.
** - cm_message reads the text of the last cm_load or cm_resume that did not return CM_ST_BUSY. It is valid until
**   the next cm_* call on the machine.
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

#define CCLJ_ABI 2                 /* NativeMachine.ABI must match; checked once after System.load */

/* Status codes of cm_load, cm_resume and cm_free. */
#define CM_ST_OK        0          /* cm_load: bios compiled */
#define CM_ST_YIELD     1          /* -> MachineResult.OK; machine kept (S2: message = filter) */
#define CM_ST_RETURNED  2          /* -> GENERIC_ERROR; dead */
#define CM_ST_ERROR     3          /* uncaught error, OOM, load error -> error(msg) / MachineException; dead */
#define CM_ST_INTERNAL  4          /* misuse: dead, bad handle, event in S1 -> internal error; dead */
#define CM_ST_BUSY      5          /* another call owns the machine: nothing done or written, no message */
/* S6a adds CM_ST_ABORTED 6 -> TIMEOUT */

/* Return codes of a cm_host_fn. Anything but CM_HOST_OK makes api_call raise a fixed level-0 error. */
#define CM_HOST_OK       0
#define CM_HOST_E_JNI    1         /* a JNI call failed (e.g. OutOfMemoryError; exception cleared), or the
                                      request was over CM_WIRE_MAX (no JNI call then; machine.c never builds one) */
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
** | CM_ST_INTERNAL | CM_ST_BUSY.
*/
int  cm_load(cm_machine *m, const unsigned char *spec, size_t spec_len,
             const unsigned char *bios, size_t bios_len);

/* UNPROTECTED (the work runs under lua_cpcall). Resumes the bios coroutine. fn(host, ...) serves its host calls.
** S1: event must be NULL. A non-NULL event gives CM_ST_INTERNAL with the message "events are not implemented yet
** (S2)", and the machine is not resumed; nResume relies on that and passes any event on.
** CM_ST_YIELD | CM_ST_RETURNED | CM_ST_ERROR | CM_ST_INTERNAL | CM_ST_BUSY.
*/
int  cm_resume(cm_machine *m, cm_host_fn fn, void *host,
               const unsigned char *event, size_t event_len);

/* UNPROTECTED. The text of the last cm_load or cm_resume that did not return CM_ST_BUSY: an error, an INTERNAL
** reason, or empty (S2: the filter). Valid until the next cm_* call. *tb_off is the offset of "\nstack traceback:"
** in the text (msg_tb), or SIZE_MAX when the text has no traceback. m == NULL gives an empty text (*len 0,
** *tb_off SIZE_MAX); nMessage relies on that for a zero handle.
*/
const unsigned char *cm_message(const cm_machine *m, size_t *len, size_t *tb_off);

/* UNPROTECTED. Frees the machine: CM_ST_OK; CM_ST_OK for m == NULL too (nothing to free); CM_ST_BUSY if another
** call owns it (then nothing is freed); CM_ST_INTERNAL if its magic is not CM_MAGIC (a test tripwire).
*/
int  cm_free(cm_machine *m);

#ifdef CCLJ_TEST
/* Test builds only (machine-test.sh); release builds have no cm_test_ symbol.
** cm_test_fail_after(n): the next n allocations of machine.c itself (cm_new's calloc and wire buffer growth)
**   succeed and every later one fails; n < 0 turns failures off. Returns the number of allocation attempts,
**   made or refused, since the previous call. Process-wide.
** cm_test_state(m): the machine's main lua_State, for inspection between calls.
** cm_test_used(m): the bytes counted by the allocator cap.
** cm_test_limit(m, limit): sets the cap to limit bytes (between calls, or from a host function).
** cm_test_cotop(m, ...): lua_gettop of the bios coroutine just before and just after append_traceback in the
**   last ERROR (both -1 before the first).
** cm_test_cotop_resume(m): lua_gettop of the bios coroutine just before the last lua_resume (-1 before the first).
** cm_test_on_leave(m, fn, ud): cm_load and cm_resume call fn(ud) just before they release the busy flag, after
**   the message is written (only when they took the flag). fn NULL turns it off. Between calls only.
*/
long  cm_test_fail_after(long n);
void *cm_test_state(cm_machine *m);
size_t cm_test_used(const cm_machine *m);
void  cm_test_limit(cm_machine *m, size_t limit);
void  cm_test_cotop(const cm_machine *m, int *before, int *after);
int   cm_test_cotop_resume(const cm_machine *m);
void  cm_test_on_leave(cm_machine *m, void (*fn)(void *), void *ud);
#endif

#endif
