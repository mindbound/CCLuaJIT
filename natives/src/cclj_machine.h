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
** raise_text and push_value carry both PROTECTED and LUA_CFUNCTION, because both kinds of function call them.
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
** - cm_message reads the message of the last cm_load or cm_resume that did not return CM_ST_BUSY: a text, or the
**   filter record after CM_ST_YIELD. It is valid until the next cm_* call on the machine.
**
** Wire format v1 (M1.md section 3.6). Integers are little-endian, numbers are IEEE-754 binary64:
**   value   := 00 NIL | 01 FALSE | 02 TRUE | 03 NUMBER f64 | 04 STRING u32 len, bytes
**              (tags 05..0F are reserved for S3 tables, back-references and objects)
**   request := u32 n, value*n                                    C -> host, one per host call
**   reply   := 00 u32 n, value*n                                 VALUES  host -> C
**            | 01 i32 level, value (NIL or STRING)               ERROR   host -> C
**              (kind 02 is reserved for the S5 CALLBACK)
**   event   := u32 n, value*n                                    host -> C, cm_resume (S2). 1 <= n; the first
**              value is a STRING (the name); n <= CM_EVENT_MAX after a CM_ST_YIELD, n <= CM_EVENT_MAX - 1 on the
**              first resume; at most CM_WIRE_MAX bytes in all
**   filter  := (no bytes)                                       the message after CM_ST_YIELD (S2): no filter
**            | 04 u32 len, bytes                                STRING: the filter, whole (len <= CM_FILTER_MAX)
**            | 01                                               FALSE: a longer filter; only "terminate" passes
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

#define CCLJ_ABI 3                 /* NativeMachine.ABI must match; checked once after System.load. 3 (S2):
                                      cm_resume takes events; the message after CM_ST_YIELD is the filter record */

/* Status codes of cm_load, cm_resume and cm_free. */
#define CM_ST_OK        0          /* cm_load: bios compiled */
#define CM_ST_YIELD     1          /* -> MachineResult.OK; machine kept; message = the filter record (S2) */
#define CM_ST_RETURNED  2          /* -> GENERIC_ERROR; dead */
#define CM_ST_ERROR     3          /* uncaught error, OOM, load error -> error(msg) / MachineException; dead */
#define CM_ST_INTERNAL  4          /* misuse: dead, bad handle, malformed or over-limit event -> internal error.
                                      An event refused before any Lua call leaves the machine as it was (I10);
                                      Java frees it anyway */
#define CM_ST_BUSY      5          /* another call owns the machine: nothing done or written, no message */
/* No other status. 6 stays reserved for S6a's CM_ST_ABORTED -> TIMEOUT. */

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
#define CM_MSG_MAX  (64u << 10)    /* error text is cut to this (DESIGN.md section 4.2); a filter is never cut */
#define CM_FILTER_MAX (CM_MSG_MAX - 5u)   /* the longest filter sent whole: 65,531 bytes (S2, D26) */
#define CM_EVENT_MAX  7999u       /* values of an event, the name included: LUAI_MAXCSTACK - 1, because co holds
                                      slot 1 of L (S2, F23); one fewer on the first resume */
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
** event NULL (event_len must be 0): resume with no values. Otherwise an event record of event_len bytes, read only
** during this call; its values become the results of the bios's coroutine.yield, or the main chunk's ... on the
** first resume. A malformed record gives CM_ST_INTERNAL "corrupt event"; a record of more than CM_EVENT_MAX
** values (CM_EVENT_MAX - 1 on the first resume) gives CM_ST_INTERNAL "event has too many values". Both come
** before any Lua call and leave the machine as it was (I10). After CM_ST_YIELD the message is the filter record.
** CM_ST_YIELD | CM_ST_RETURNED | CM_ST_ERROR | CM_ST_INTERNAL | CM_ST_BUSY.
*/
int  cm_resume(cm_machine *m, cm_host_fn fn, void *host,
               const unsigned char *event, size_t event_len);

/* UNPROTECTED. The message of the last cm_load or cm_resume that did not return CM_ST_BUSY: an error, an INTERNAL
** reason, empty, or after CM_ST_YIELD the filter record (empty = no filter). Valid until the next cm_* call.
** *tb_off is the offset of "\nstack traceback:" in the text (msg_tb), or SIZE_MAX when the text has no traceback
** (always after CM_ST_YIELD). m == NULL gives an empty text (*len 0, *tb_off SIZE_MAX); nMessage relies on that
** for a zero handle.
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
** cm_test_cotop_resume(m): lua_gettop of the bios coroutine when the last resume started, before its event was
**   pushed (-1 before the first).
** cm_test_on_leave(m, fn, ud): cm_load and cm_resume call fn(ud) just before they release the busy flag, after
**   the message is written (only when they took the flag). fn NULL turns it off. Between calls only.
** cm_test_grow_fail(m, on): on = one-shot. The next event push clears the flag and sets the cap to the bytes in use
**   just before lua_checkstack(co, n + 1). The lowered cap stays: the machine is dead then, or the test resets it
**   with cm_test_limit. Between calls only.
** cm_test_grow(m, ...): in the last event push, the return of lua_checkstack(co, n + 1) and lua_status(co) just
**   before and just after it (all -1 before the first).
*/
long  cm_test_fail_after(long n);
void *cm_test_state(cm_machine *m);
size_t cm_test_used(const cm_machine *m);
void  cm_test_limit(cm_machine *m, size_t limit);
void  cm_test_cotop(const cm_machine *m, int *before, int *after);
int   cm_test_cotop_resume(const cm_machine *m);
void  cm_test_on_leave(cm_machine *m, void (*fn)(void *), void *ud);
void  cm_test_grow_fail(cm_machine *m, int on);
void  cm_test_grow(const cm_machine *m, int *ret, int *status_before, int *status_after);
#endif

#endif
