/*
** machine.c: one CCLuaJIT machine, a capped LuaJIT state that runs the bios in a coroutine
** (docs/port-1.21.1/M1.md sections 3.5 to 3.8). The API is in cclj_machine.h.
**
** Rules for every function in this file:
** - Never include jni.h. Java is reached only through the cm_host_fn that jni_machine.c passes to cm_resume, and
**   only in phase 2 of api_call, where no Lua call happens.
** - Each function carries its run-context tag (cclj_machine.h): UNPROTECTED functions make only calls that
**   cannot raise (M1.md section 3.8); PROTECTED functions are lua_cpcall bodies or run only inside one;
**   LUA_CFUNCTION functions are api_call or run only when api_call calls them. raise_text and push_value carry
**   both of the last two tags, because both kinds of function call them.
** - The ud of every lua_cpcall is the heap cm_machine (F3). No light userdata is ever pushed: Lua sees no C
**   pointer, and C code finds the machine through the allocator's ud (machine_of).
** - lua_checkstack(to, n + 1) comes before every lua_xmove(from, to, n) (F1).
** - The bios coroutine "co" is touched outside lua_resume only through the calls of the "rule for co" (M1.md
**   section 3.7): lua_status, lua_gettop, a shrinking lua_settop, lua_type, lua_tolstring on a LUA_TSTRING only,
**   lua_tonumber on a LUA_TNUMBER only (S2), lua_checkstack (a failed growth resets co's status, F19), lua_xmove
**   after the check above, lua_resume while the machine is not dead, lua_getstack, and lua_getinfo with "Sln"
**   only.
** - No mutable static state (release builds), no exit, and no output except cm_panic's one line. Debug and test
**   builds add CM_CHECK, which prints one line and aborts.
** - Every copy is bounded; wire decoders use size_t arithmetic and check every length.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cclj_machine.h"

/* The wire format is little-endian. The codec below assembles integers byte by byte, but the Java side and the
** S3 plans assume a little-endian host, so refuse anything else at build time.
*/
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CCLuaJIT supports little-endian hosts only");

/* The header cannot include luaconf.h, so CM_EVENT_MAX is a literal (docs/port-1.21.1/S2.md section 4.2). co holds
** slot 1 of L while an event is pushed, so L has room for LUAI_MAXCSTACK - 1 values (F23). A filter record is the
** tag, the 4-byte length and the bytes, and fits the message buffer whole.
*/
_Static_assert(CM_EVENT_MAX == LUAI_MAXCSTACK - 1, "CM_EVENT_MAX must be LUAI_MAXCSTACK - 1 (S2.md F23)");
_Static_assert(CM_FILTER_MAX + 5 == CM_MSG_MAX, "a filter record of CM_FILTER_MAX bytes must fill msg exactly");

#define CM_MAGIC        0x564c4d43u   /* "CMLV": a live machine */
#define CM_MAGIC_FREED  0x44414544u   /* "DEAD": set just before the struct is freed (a test tripwire) */

/* Machine states: the state of the bios coroutine (F4). */
#define CM_NEW        0   /* not resumed yet (or not loaded yet) */
#define CM_SUSPENDED  1   /* yielded */
#define CM_DEAD       2   /* returned, raised, or failed to load: never resumed again */

/* Traceback elision, as LuaJIT's own traceback does it: 12 leading frames, "...", 10 trailing frames. */
#define TB_LEVELS1  12
#define TB_LEVELS2  10
#define TB_NAME_MAX 100   /* bytes of a function name in a frame line */

/* Stack-balance checks. CCLJ_DEBUG builds (build-natives.sh CCLJ_DEBUG=1) and CCLJ_TEST builds (machine-test.sh)
** check that the Lua stacks hold what the code below expects; a failed check is a bug in this file, so it prints
** one line and aborts. Release builds compile the checks out.
*/
#if defined(CCLJ_DEBUG) || defined(CCLJ_TEST)
/* UNPROTECTED. */
static void cm_check_failed(int line, const char *what)
{
  fprintf(stderr, "CCLuaJIT check failed: machine.c:%d: %s\n", line, what);
  abort();
}
#define CM_CHECK(c) ((c) ? (void)0 : cm_check_failed(__LINE__, #c))
#else
#define CM_CHECK(c) ((void)sizeof(c))   /* not evaluated; keeps the variables it names "used" */
#endif

struct cm_machine {
  unsigned char pad0[64];    /* calloc only guarantees 16-byte alignment, so intr_req has 64 bytes of padding */
  uint32_t intr_req;         /* on each side to own its cache line. S6a: written by other threads. Unused in S1 */
  unsigned char pad1[60];
  uint32_t magic;            /* CM_MAGIC live, CM_MAGIC_FREED after free: a test tripwire, not a defence */
  int busy;                  /* __atomic CAS 0->1 (ACQUIRE) in cm_load/cm_resume/cm_free; store 0 (RELEASE) on exit */
  int state;                 /* CM_NEW, CM_SUSPENDED, CM_DEAD: the bios coroutine (F4) */
  lua_State *L;              /* main thread: runs only our cpcall bodies, never Lua code */
  int co_ref;                /* registry ref of the bios coroutine; re-fetched and type-checked per resume */
  uint32_t nfn;              /* number of host functions (bounds check) */
  lua_Alloc orig_alloc; void *orig_ud;   /* LuaJIT's allocator; restored before lua_close */
  size_t used, limit;        /* bytes through the wrapper */
  cm_host_fn host_fn; void *host;        /* non-NULL only inside cm_resume */
  const unsigned char *in_spec, *in_bios;   /* cm_load only (cpcall ud is always m, F3) */
  size_t in_spec_len, in_bios_len;
  cm_buf req_buf, rep_buf;   /* wire buffers: outside the Lua cap, at most CM_WIRE_MAX each */
  const unsigned char *in_event; size_t in_event_len;   /* cm_resume only: the checked record (cpcall ud is m, F3) */
  uint32_t in_event_n;       /* its number of values; 0 = no event */
  int result;                /* body -> caller */
  lua_Number err_num;        /* a number for render_error or the filter -> their number bodies */
  uint64_t id_key;           /* SecureRandom; keys the "<type>: %08x" hash (patch 10 reuses it in M2) */
  size_t msg_len, msg_tb;    /* msg_tb: offset of the traceback in msg, SIZE_MAX if none (D7) */
  unsigned char msg[CM_MSG_MAX];
#ifdef CCLJ_TEST
  int test_cotop_before, test_cotop_after;   /* lua_gettop(co) around append_traceback (cm_test_cotop) */
  int test_cotop_resume;                     /* lua_gettop(co) when the resume starts, before the event is pushed */
  int test_grow_fail;                        /* cm_test_grow_fail: one-shot */
  int test_grow_ret, test_grow_before, test_grow_after;   /* cm_test_grow */
  void (*test_leave_fn)(void *);             /* cm_test_on_leave */
  void *test_leave_ud;
#endif
};

/* -- Test hooks (CCLJ_TEST builds only) ------------------------------------ */

#ifdef CCLJ_TEST
/* Writable statics, in test builds only: machine-test.sh compiles with CCLJ_TEST, release builds never do. */
static long test_budget = -1;   /* allocations of this file that may still succeed; < 0: no limit */
static long test_allocs = 0;    /* allocation attempts since the last cm_test_fail_after */

/* UNPROTECTED (pure). */
long cm_test_fail_after(long n)
{
  long made = __atomic_exchange_n(&test_allocs, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&test_budget, n < 0 ? -1 : n, __ATOMIC_SEQ_CST);
  return made;
}

/* UNPROTECTED (pure). Called before each allocation of this file: 0 = make it fail. */
static int test_alloc_ok(void)
{
  long b;
  __atomic_add_fetch(&test_allocs, 1, __ATOMIC_SEQ_CST);
  b = __atomic_load_n(&test_budget, __ATOMIC_SEQ_CST);
  while (b >= 0) {
    if (b == 0) return 0;
    if (__atomic_compare_exchange_n(&test_budget, &b, b - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
  }
  return 1;
}

/* UNPROTECTED. */
void *cm_test_state(cm_machine *m) { return m->L; }

/* UNPROTECTED. */
size_t cm_test_used(const cm_machine *m) { return m->used; }

/* UNPROTECTED. Only between calls, or from a host function (the allocator reads limit on the same thread). */
void cm_test_limit(cm_machine *m, size_t limit) { m->limit = limit; }

/* UNPROTECTED. */
void cm_test_cotop(const cm_machine *m, int *before, int *after)
{
  *before = m->test_cotop_before;
  *after = m->test_cotop_after;
}

/* UNPROTECTED. */
int cm_test_cotop_resume(const cm_machine *m) { return m->test_cotop_resume; }

/* UNPROTECTED. Between calls only. */
void cm_test_on_leave(cm_machine *m, void (*fn)(void *), void *ud)
{
  m->test_leave_fn = fn;
  m->test_leave_ud = ud;
}

/* UNPROTECTED. Between calls only. */
void cm_test_grow_fail(cm_machine *m, int on) { m->test_grow_fail = on != 0; }

/* UNPROTECTED. */
void cm_test_grow(const cm_machine *m, int *ret, int *status_before, int *status_after)
{
  *ret = m->test_grow_ret;
  *status_before = m->test_grow_before;
  *status_after = m->test_grow_after;
}
#else
#define test_alloc_ok() 1
#endif

/* -- Message --------------------------------------------------------------- */

/* UNPROTECTED (pure). Sets the message to n bytes of s, cut to CM_MSG_MAX. A message set here has no traceback. */
static void msg_set(cm_machine *m, const char *s, size_t n)
{
  if (n > CM_MSG_MAX) n = CM_MSG_MAX;
  if (n > 0) memcpy(m->msg, s, n);
  m->msg_len = n;
  m->msg_tb = SIZE_MAX;
}

/* UNPROTECTED (pure). */
static void msg_setz(cm_machine *m, const char *s)
{
  msg_set(m, s, strlen(s));
}

/* UNPROTECTED (pure). Appends n bytes of s if all of them fit; 0 if they do not (nothing is appended then). */
static int msg_append(cm_machine *m, const char *s, size_t n)
{
  if (n > CM_MSG_MAX - m->msg_len) return 0;
  memcpy(m->msg + m->msg_len, s, n);
  m->msg_len += n;
  return 1;
}

/* UNPROTECTED. The error message of a failed lua_cpcall, at the top of L: a string is copied (cut to
** CM_MSG_MAX); anything else can only be the result of a failed allocation.
*/
static void msg_from_top(cm_machine *m, lua_State *L)
{
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t n;
    const char *s = lua_tolstring(L, -1, &n);
    msg_set(m, s, n);
  } else {
    msg_setz(m, "not enough memory");
  }
}

/* UNPROTECTED (pure). Sets the message to the filter record of n bytes of s (S2.md section 4.1): STRING with the
** whole text, or FALSE when it is longer than CM_FILTER_MAX (D26). A filter is never cut.
*/
static void msg_filter(cm_machine *m, const char *s, size_t n)
{
  m->msg_tb = SIZE_MAX;
  if (n > CM_FILTER_MAX) {
    m->msg[0] = CM_WIRE_FALSE;
    m->msg_len = 1;
    return;
  }
  m->msg[0] = CM_WIRE_STRING;
  m->msg[1] = (unsigned char)n;
  m->msg[2] = (unsigned char)(n >> 8);
  m->msg[3] = (unsigned char)(n >> 16);
  m->msg[4] = (unsigned char)(n >> 24);
  if (n > 0) memcpy(m->msg + 5, s, n);
  m->msg_len = 5 + n;
}

/* -- Allocator and machine lookup ------------------------------------------ */

/* UNPROTECTED (allocator, VM thread). LuaJIT's allocator behind a cap of m->limit bytes. Only growth is ever
** refused; frees and shrinks always go through. The "used > limit" test stops "limit - used" from wrapping.
*/
static void *cm_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  cm_machine *m = (cm_machine *)ud;
  size_t old = ptr ? osize : 0;
  void *p;
  if (nsize > old && (m->used > m->limit || nsize - old > m->limit - m->used)) return NULL;
  p = m->orig_alloc(m->orig_ud, ptr, osize, nsize);
  if (p != NULL || nsize == 0) m->used = m->used - old + nsize;
  return p;
}

/* UNPROTECTED. The machine that owns L (any thread of it), or NULL. */
static cm_machine *machine_of(lua_State *L)
{
  void *ud;
  return lua_getallocf(L, &ud) == cm_alloc ? (cm_machine *)ud : NULL;
}

/* UNPROTECTED. LuaJIT calls this for an error outside any protected call (a bug in this file), then exits the
** process. Only Linux gets here; on Windows such an error ends the process without it. One line on stderr.
*/
static int cm_panic(lua_State *L)
{
  char line[201];
  size_t n = 0, len = 0, i;
  const char *s = "?";
  if (lua_type(L, -1) == LUA_TSTRING) s = lua_tolstring(L, -1, &len);
  else len = 1;
  for (i = 0; i < len && n < sizeof(line) - 1; i++) {
    unsigned char c = (unsigned char)s[i];
    line[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
  }
  line[n] = '\0';
  fprintf(stderr, "CCLuaJIT bug: Lua error outside a protected call: %s\n", line);
  return 0;
}

/* UNPROTECTED (pure). Mixes 64 bits into 32: the fmix64 finalizer of MurmurHash3 (public domain), folded. */
static uint32_t mix32(uint64_t x)
{
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

/* -- Busy flag --------------------------------------------------------------- */

/* UNPROTECTED. Takes the busy flag of a live machine: CM_ST_OK, CM_ST_INTERNAL for NULL or a wrong magic, or
** CM_ST_BUSY when another call owns it. Writes nothing into the machine unless it returns CM_ST_OK: a refused call
** may run while the owner writes the message on another thread (cclj_machine.h, thread rules).
*/
static int machine_enter(cm_machine *m)
{
  int expected = 0;
  if (m == NULL || m->magic != CM_MAGIC) return CM_ST_INTERNAL;
  if (!__atomic_compare_exchange_n(&m->busy, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return CM_ST_BUSY;
  return CM_ST_OK;
}

/* UNPROTECTED. Releases the busy flag that machine_enter took. */
static void machine_leave(cm_machine *m)
{
#ifdef CCLJ_TEST
  if (m->test_leave_fn != NULL) m->test_leave_fn(m->test_leave_ud);
#endif
  __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}

/* -- Wire codec ------------------------------------------------------------- */

/* A bounded reader: p is the next byte, left the number of bytes from p on (p[0] included). */
typedef struct wire_rd {
  const unsigned char *p;
  size_t left;
} wire_rd;

/* UNPROTECTED (pure). */
static int rd_u8(wire_rd *r, unsigned *v)
{
  if (r->left < 1) return 0;
  *v = r->p[0];
  r->p++; r->left--;
  return 1;
}

/* UNPROTECTED (pure). */
static int rd_u32(wire_rd *r, uint32_t *v)
{
  if (r->left < 4) return 0;
  *v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
  r->p += 4; r->left -= 4;
  return 1;
}

/* UNPROTECTED (pure). Takes n bytes; *at points at them. */
static int rd_take(wire_rd *r, size_t n, const unsigned char **at)
{
  if (n > r->left) return 0;
  *at = r->p;
  r->p += n; r->left -= n;
  return 1;
}

/* UNPROTECTED (pure). A str: u32 len, then len bytes, at most max bytes. */
static int rd_str(wire_rd *r, size_t max, const unsigned char **s, size_t *len)
{
  uint32_t n;
  if (!rd_u32(r, &n) || n > max) return 0;
  *len = n;
  return rd_take(r, n, s);
}

/* One decoded value. */
typedef struct wire_val {
  unsigned tag;              /* CM_WIRE_NIL .. CM_WIRE_STRING */
  double num;                /* CM_WIRE_NUMBER */
  const unsigned char *s;    /* CM_WIRE_STRING: the bytes (inside the buffer) */
  size_t len;
} wire_val;

/* UNPROTECTED (pure). Reads one value; 0 on an unknown tag or a length past the end. */
static int rd_value(wire_rd *r, wire_val *v)
{
  const unsigned char *b;
  if (!rd_u8(r, &v->tag)) return 0;
  switch (v->tag) {
  case CM_WIRE_NIL: case CM_WIRE_FALSE: case CM_WIRE_TRUE:
    return 1;
  case CM_WIRE_NUMBER: {
    uint64_t u = 0;
    int i;
    if (!rd_take(r, 8, &b)) return 0;
    for (i = 7; i >= 0; i--) u = (u << 8) | b[i];
    memcpy(&v->num, &u, sizeof(v->num));
    return 1;
  }
  case CM_WIRE_STRING:
    return rd_str(r, CM_WIRE_MAX, &v->s, &v->len);
  default:
    return 0;
  }
}

/* A checked reply. */
typedef struct wire_reply {
  unsigned kind;             /* CM_WIRE_VALUES or CM_WIRE_ERROR */
  uint32_t n;                /* VALUES: number of values (at most the number of bytes after the count) */
  int32_t level;             /* ERROR: the level */
  wire_rd rest;              /* VALUES: the values; ERROR: the message value */
} wire_reply;

/* UNPROTECTED (pure; a Tier 0 fuzz target). Checks a whole reply: a known kind, every length inside the
** buffer, known tags, an ERROR message that is NIL or STRING, and no bytes left over. 1 = valid.
*/
static int wire_check_reply(const unsigned char *d, size_t len, wire_reply *out)
{
  wire_rd r;
  wire_val v;
  uint32_t i, u;
  r.p = d; r.left = d ? len : 0;
  if (!rd_u8(&r, &out->kind)) return 0;
  if (out->kind == CM_WIRE_VALUES) {
    if (!rd_u32(&r, &out->n) || out->n > r.left) return 0;   /* every value takes at least one byte */
    out->level = 0;
    out->rest = r;
    for (i = 0; i < out->n; i++)
      if (!rd_value(&r, &v)) return 0;
  } else if (out->kind == CM_WIRE_ERROR) {
    if (!rd_u32(&r, &u)) return 0;
    out->level = u <= 0x7fffffffu ? (int32_t)u : -(int32_t)(~u) - 1;   /* two's complement, without overflow */
    out->n = 0;
    out->rest = r;
    if (!rd_value(&r, &v) || (v.tag != CM_WIRE_NIL && v.tag != CM_WIRE_STRING)) return 0;
  } else {
    return 0;
  }
  return r.left == 0;
}

/* UNPROTECTED (pure; a Tier 0 fuzz target). Checks a whole event record (S2): a count of at least 1 and at most
** the number of bytes after it, a STRING first value (the name), known tags, every length inside the buffer, and
** no bytes left over. 1 = valid, and *n is the number of values. The bound on n depends on the machine's state,
** so cm_resume checks it.
*/
static int wire_check_event(const unsigned char *d, size_t len, uint32_t *n)
{
  wire_rd r;
  wire_val v;
  uint32_t i, cnt;
  r.p = d; r.left = d ? len : 0;
  if (!rd_u32(&r, &cnt) || cnt == 0 || cnt > r.left) return 0;   /* every value takes at least one byte */
  if (!rd_value(&r, &v) || v.tag != CM_WIRE_STRING) return 0;
  for (i = 1; i < cnt; i++)
    if (!rd_value(&r, &v)) return 0;
  if (r.left != 0) return 0;
  *n = cnt;
  return 1;
}

/* UNPROTECTED (pure; a Tier 0 fuzz target). Checks a whole spec: every length inside the buffer, names of at
** most CM_WIRE_MAX_NAME bytes, hasModule 0 or 1, method flags with only MAY_YIELD, at most CM_WIRE_MAX_FN
** methods in all, and no bytes left over. 1 = valid, and *nfn is the number of methods.
*/
static int wire_check_spec(const unsigned char *d, size_t len, uint32_t *nfn)
{
  wire_rd r;
  const unsigned char *s;
  size_t slen;
  uint32_t napi, nnames, nmethods, a, k, total = 0;
  unsigned has, flags;
  r.p = d; r.left = d ? len : 0;
  if (!rd_u32(&r, &napi)) return 0;
  for (a = 0; a < napi; a++) {     /* each API takes at least 9 bytes, so a bad napi fails fast */
    if (!rd_u32(&r, &nnames)) return 0;
    for (k = 0; k < nnames; k++)
      if (!rd_str(&r, CM_WIRE_MAX_NAME, &s, &slen)) return 0;
    if (!rd_u8(&r, &has) || has > 1) return 0;
    if (has && !rd_str(&r, CM_WIRE_MAX_NAME, &s, &slen)) return 0;
    if (!rd_u32(&r, &nmethods) || nmethods > CM_WIRE_MAX_FN - total) return 0;
    total += nmethods;
    for (k = 0; k < nmethods; k++) {
      if (!rd_str(&r, CM_WIRE_MAX_NAME, &s, &slen)) return 0;
      if (!rd_u8(&r, &flags) || (flags & ~(unsigned)CM_WIRE_MAY_YIELD) != 0) return 0;
    }
  }
  if (r.left != 0) return 0;
  *nfn = total;
  return 1;
}

/* UNPROTECTED (pure). Makes room for extra more bytes in b: at most CM_WIRE_MAX in all. 0 = it cannot grow. */
static int buf_reserve(cm_buf *b, size_t extra)
{
  size_t need, cap;
  unsigned char *p;
  if (extra > CM_WIRE_MAX || b->len > CM_WIRE_MAX - extra) return 0;
  need = b->len + extra;
  if (need <= b->cap) return 1;
  cap = b->cap >= 256 ? b->cap : 256;
  while (cap < need) cap = cap > CM_WIRE_MAX / 2 ? CM_WIRE_MAX : cap * 2;
  if (!test_alloc_ok()) return 0;
  p = (unsigned char *)realloc(b->data, cap);
  if (p == NULL) return 0;
  b->data = p;
  b->cap = cap;
  return 1;
}

/* UNPROTECTED (pure). The put_* helpers write into room that buf_reserve made. */
static void put_u8(cm_buf *b, unsigned v)
{
  b->data[b->len++] = (unsigned char)v;
}

/* UNPROTECTED (pure). */
static void put_u32(cm_buf *b, uint32_t v)
{
  b->data[b->len++] = (unsigned char)v;
  b->data[b->len++] = (unsigned char)(v >> 8);
  b->data[b->len++] = (unsigned char)(v >> 16);
  b->data[b->len++] = (unsigned char)(v >> 24);
}

/* UNPROTECTED (pure). */
static void put_f64(cm_buf *b, double d)
{
  uint64_t u;
  int i;
  memcpy(&u, &d, sizeof(u));
  for (i = 0; i < 8; i++) b->data[b->len++] = (unsigned char)(u >> (8 * i));
}

/* -- Host calls --------------------------------------------------------------- */

/* UNPROTECTED (pure). The fixed level-0 error for a failed host call. */
static const char *host_error_text(int rc)
{
  switch (rc) {
  case CM_HOST_E_JNI: return "CCLuaJIT: JNI failure in host call";
  case CM_HOST_E_JAVA: return "CCLuaJIT: Java exception in host call";
  case CM_HOST_E_NULL: return "CCLuaJIT: host call returned null";
  case CM_HOST_E_TOOBIG: return "CCLuaJIT: host reply too large";
  case CM_HOST_E_NOMEM: return "CCLuaJIT: not enough memory for host reply";
  default: return "CCLuaJIT: host call failed";
  }
}

/* PROTECTED or LUA_CFUNCTION (phases 1 and 3): both kinds of function call it. Raises s as it is, without a
** position. Never returns.
*/
static int raise_text(lua_State *L, const char *s)
{
  lua_pushstring(L, s);
  return lua_error(L);
}

/* PROTECTED or LUA_CFUNCTION (phase 3): resume_body pushes event values with it, api_call reply values. Pushes one
** value that wire_check_reply or wire_check_event accepted. May raise (out of memory).
*/
static void push_value(lua_State *L, const wire_val *v)
{
  switch (v->tag) {
  case CM_WIRE_FALSE: lua_pushboolean(L, 0); break;
  case CM_WIRE_TRUE: lua_pushboolean(L, 1); break;
  case CM_WIRE_NUMBER: lua_pushnumber(L, v->num); break;   /* lua_pushnumber canonicalizes NaNs */
  case CM_WIRE_STRING: lua_pushlstring(L, (const char *)v->s, v->len); break;
  default: lua_pushnil(L); break;
  }
}

/* LUA_CFUNCTION. Every API method is this C closure; upvalue 1 is its function index (a number).
** Phase 1 (may raise): encode the arguments into req_buf. Phase 2 (no Lua call): the host call. Phase 3 (may
** raise): check the reply and push its values, or raise its error. A raise can leave only the machine's own
** buffers half-written: nothing here holds a malloc block, a lock or a JNI frame across a Lua call.
*/
static int api_call(lua_State *L)
{
  cm_machine *m = machine_of(L);
  cm_buf *req;
  lua_Number d;
  uint32_t fn, k;
  int i, n, rc;
  wire_reply rp;
  wire_val v;

  /* Phase 1. */
  if (m == NULL || m->host_fn == NULL) return luaL_error(L, "CCLuaJIT: host call outside a machine resume");
  if (lua_type(L, lua_upvalueindex(1)) != LUA_TNUMBER) return luaL_error(L, "CCLuaJIT: bad host function index");
  d = lua_tonumber(L, lua_upvalueindex(1));
  if (!(d >= 0 && d < (lua_Number)m->nfn)) return luaL_error(L, "CCLuaJIT: bad host function index");
  fn = (uint32_t)d;
  if ((lua_Number)fn != d) return luaL_error(L, "CCLuaJIT: bad host function index");
  req = &m->req_buf;
  req->len = 0;
  n = lua_gettop(L);
  if (!buf_reserve(req, 4)) return raise_text(L, "not enough memory");
  put_u32(req, (uint32_t)n);
  for (i = 1; i <= n; i++) {
    switch (lua_type(L, i)) {
    case LUA_TBOOLEAN:
      if (!buf_reserve(req, 1)) return raise_text(L, "not enough memory");
      put_u8(req, lua_toboolean(L, i) ? CM_WIRE_TRUE : CM_WIRE_FALSE);
      break;
    case LUA_TNUMBER:
      if (!buf_reserve(req, 9)) return raise_text(L, "not enough memory");
      put_u8(req, CM_WIRE_NUMBER);
      put_f64(req, (double)lua_tonumber(L, i));
      break;
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, i, &len);   /* a string: no conversion, no allocation */
      if (len > CM_WIRE_MAX || !buf_reserve(req, 5 + len)) return raise_text(L, "not enough memory");
      put_u8(req, CM_WIRE_STRING);
      put_u32(req, (uint32_t)len);
      if (len > 0) memcpy(req->data + req->len, s, len);
      req->len += len;
      break;
    }
    case LUA_TTABLE:
      return luaL_error(L, "CCLuaJIT: tables are not supported yet");
    default:   /* nil; function, thread and userdata become nil, as in Cobalt */
      if (!buf_reserve(req, 1)) return raise_text(L, "not enough memory");
      put_u8(req, CM_WIRE_NIL);
      break;
    }
  }

  /* Phase 2: no Lua call until the host returns. */
  m->rep_buf.len = 0;
  rc = m->host_fn(m->host, fn, req, &m->rep_buf);

  /* Phase 3. No Lua code runs while the reply is read: nothing reachable has a __gc metamethod (I5). */
  if (rc != CM_HOST_OK) return raise_text(L, host_error_text(rc));
  if (m->rep_buf.len > CM_WIRE_MAX || m->rep_buf.len > m->rep_buf.cap ||
      !wire_check_reply(m->rep_buf.data, m->rep_buf.len, &rp))
    return raise_text(L, "CCLuaJIT: corrupt host reply");
  if (rp.kind == CM_WIRE_VALUES) {
    if (rp.n > (uint32_t)LUAI_MAXCSTACK || !lua_checkstack(L, (int)rp.n))
      return luaL_error(L, "too many results to return");
    for (k = 0; k < rp.n; k++) {
      if (!rd_value(&rp.rest, &v)) return raise_text(L, "CCLuaJIT: corrupt host reply");
      push_value(L, &v);
    }
    CM_CHECK(lua_gettop(L) == n + (int)rp.n);
    return (int)rp.n;
  }
  if (!rd_value(&rp.rest, &v)) return raise_text(L, "CCLuaJIT: corrupt host reply");
  if (v.tag == CM_WIRE_STRING && rp.level > 0) {
    luaL_where(L, rp.level);   /* level 1 is the Lua caller of this closure */
    lua_pushlstring(L, (const char *)v.s, v.len);
    lua_concat(L, 2);
  } else {
    push_value(L, &v);   /* a string as it is, or nil */
  }
  return lua_error(L);
}

/* -- Load ------------------------------------------------------------------- */

/* PROTECTED. Opens one library the way the stand-alone interpreter does, with its name as the argument. */
static void open_lib(lua_State *L, lua_CFunction f, const char *name)
{
  lua_pushcfunction(L, f);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

/* PROTECTED. jit.security(opt) for the jit table at index jit (an absolute index). */
static lua_Integer jit_security(lua_State *L, int jit, const char *opt)
{
  lua_Integer v;
  int top = lua_gettop(L);
  lua_getfield(L, jit, "security");
  lua_pushstring(L, opt);
  lua_call(L, 1, 1);
  v = lua_tointeger(L, -1);   /* 0 for anything but a number: fails safe */
  lua_pop(L, 1);
  CM_CHECK(lua_gettop(L) == top);
  return v;
}

/* PROTECTED. Raises unless the value at absolute index idx is a table. */
static void need_table(lua_State *L, int idx)
{
  if (lua_type(L, idx) != LUA_TTABLE) raise_text(L, "CCLuaJIT: library table missing");
}

/* PROTECTED. t[name] = nil, raw, for the table at absolute index t. */
static void remove_field(lua_State *L, int t, const char *name)
{
  lua_pushstring(L, name);
  lua_pushnil(L);
  lua_rawset(L, t);
}

/* PROTECTED. Builds one table of api_call closures per API of the spec (already checked by wire_check_spec), and
** puts it into _G under each of its names and into _LOADED under its module name, if it has one.
*/
static void build_apis(cm_machine *m, lua_State *L)
{
  wire_rd r, names;
  const unsigned char *s, *mod = NULL;
  size_t slen, modlen = 0;
  uint32_t napi, nnames, nmethods, a, k, idx = 0, total = 0;
  unsigned has, flags;
  int g, loaded, t;

  if (!wire_check_spec(m->in_spec, m->in_spec_len, &total)) raise_text(L, "CCLuaJIT: corrupt API table description");
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  g = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  loaded = lua_gettop(L);
  need_table(L, loaded);
  r.p = m->in_spec; r.left = m->in_spec_len;
  /* The reads below cannot fail after wire_check_spec; they are checked anyway. */
  if (!rd_u32(&r, &napi)) goto corrupt;
  for (a = 0; a < napi; a++) {
    if (!rd_u32(&r, &nnames)) goto corrupt;
    names = r;
    for (k = 0; k < nnames; k++)
      if (!rd_str(&r, CM_WIRE_MAX_NAME, &s, &slen)) goto corrupt;
    if (!rd_u8(&r, &has)) goto corrupt;
    if (has && !rd_str(&r, CM_WIRE_MAX_NAME, &mod, &modlen)) goto corrupt;
    if (!rd_u32(&r, &nmethods) || nmethods > CM_WIRE_MAX_FN) goto corrupt;
    lua_createtable(L, 0, (int)nmethods);
    t = lua_gettop(L);
    for (k = 0; k < nmethods; k++) {
      if (!rd_str(&r, CM_WIRE_MAX_NAME, &s, &slen) || !rd_u8(&r, &flags)) goto corrupt;
      lua_pushlstring(L, (const char *)s, slen);
      lua_pushinteger(L, (lua_Integer)idx++);
      lua_pushcclosure(L, api_call, 1);
      lua_rawset(L, t);
    }
    for (k = 0; k < nnames; k++) {
      if (!rd_str(&names, CM_WIRE_MAX_NAME, &s, &slen)) goto corrupt;
      lua_pushlstring(L, (const char *)s, slen);
      lua_pushvalue(L, t);
      lua_rawset(L, g);
    }
    if (has) {
      lua_pushlstring(L, (const char *)mod, modlen);
      lua_pushvalue(L, t);
      lua_rawset(L, loaded);
    }
    CM_CHECK(lua_gettop(L) == t && t == loaded + 1);
    lua_settop(L, t - 1);
  }
  if (idx != total) goto corrupt;
  m->nfn = idx;
  lua_settop(L, g - 1);
  return;
corrupt:
  raise_text(L, "CCLuaJIT: corrupt API table description");
}

/* PROTECTED. The body of cm_load (M1.md section 3.7, load_body steps 1-7). */
static int load_body(lua_State *L)
{
  cm_machine *m = machine_of(L);
  lua_State *co;
  int jit, g, t;

  lua_settop(L, 0);
  if (m == NULL) return raise_text(L, "CCLuaJIT: machine missing");

  /* 1. The curated libraries, one at a time. luaopen_base also opens coroutine; luaopen_jit turns the JIT on.
  **    io, os, package, debug and ffi are never opened.
  */
  open_lib(L, luaopen_base, "");
  open_lib(L, luaopen_table, LUA_TABLIBNAME);
  open_lib(L, luaopen_string, LUA_STRLIBNAME);
  open_lib(L, luaopen_math, LUA_MATHLIBNAME);
  open_lib(L, luaopen_bit, LUA_BITLIBNAME);
  open_lib(L, luaopen_jit, LUA_JITLIBNAME);

  /* 2. LuaJIT's security defaults must be on (DESIGN.md section 2). */
  lua_getfield(L, LUA_GLOBALSINDEX, "jit");
  jit = lua_gettop(L);
  need_table(L, jit);
  if (jit_security(L, jit, "prng") == 0 || jit_security(L, jit, "strhash") == 0 || jit_security(L, jit, "mcode") == 0)
    return raise_text(L, "CCLuaJIT: LuaJIT security defaults are off");

  /* 3. At most 256 KiB of machine code per state (DESIGN.md section 3.6, F11). */
  lua_getfield(L, jit, "opt");
  need_table(L, lua_gettop(L));
  lua_getfield(L, -1, "start");
  lua_pushstring(L, "maxmcode=256");
  lua_call(L, 1, 0);
  lua_settop(L, 0);

  /* 4. Removals (F12), raw, on explicitly pushed tables. _LOADED.bit stays (decision 6). */
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  g = lua_gettop(L);
  remove_field(L, g, "jit");
  remove_field(L, g, "bit");
  remove_field(L, g, "dofile");
  remove_field(L, g, "loadfile");
  remove_field(L, g, "collectgarbage");
  remove_field(L, g, "gcinfo");
  remove_field(L, g, "newproxy");
  remove_field(L, g, "print");
  lua_pushstring(L, LUA_STRLIBNAME);
  lua_rawget(L, g);
  t = lua_gettop(L);
  need_table(L, t);
  remove_field(L, t, "dump");
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  t = lua_gettop(L);
  need_table(L, t);
  remove_field(L, t, "jit");
  remove_field(L, t, "jit.opt");
  lua_pushstring(L, "_PRELOAD");   /* jit.util, string.buffer, table.new and table.clear were preloaded */
  lua_newtable(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_settop(L, 0);

  /* 5. The API tables. */
  build_apis(m, L);

  /* 6. The bios: text only (patch 03 refuses binary chunks in any case). The message reads "bios.lua:N: ...". */
  if (luaL_loadbufferx(L, m->in_bios ? (const char *)m->in_bios : "", m->in_bios_len, "@bios.lua", "t") != 0)
    return lua_error(L);

  /* 7. The bios coroutine, not run yet. The registry keeps it; resume_body fetches it again each time. */
  co = lua_newthread(L);
  lua_pushvalue(L, -2);
  if (!lua_checkstack(co, 2)) return raise_text(L, "CCLuaJIT: cannot grow the bios coroutine stack");
  lua_xmove(L, co, 1);
  m->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_settop(L, 0);
  return 0;
}

/* UNPROTECTED. */
int cm_load(cm_machine *m, const unsigned char *spec, size_t spec_len, const unsigned char *bios, size_t bios_len)
{
  int st, res;
  if ((res = machine_enter(m)) != CM_ST_OK) return res;
  if (m->state == CM_DEAD) {
    msg_setz(m, "machine is dead");
    res = CM_ST_INTERNAL;
  } else if (m->state != CM_NEW || m->co_ref != LUA_NOREF) {
    msg_setz(m, "bios is already loaded");
    res = CM_ST_INTERNAL;
  } else if ((spec == NULL && spec_len != 0) || (bios == NULL && bios_len != 0) ||
             spec_len > CM_WIRE_MAX || bios_len > CM_WIRE_MAX) {
    msg_setz(m, "bad load arguments");
    res = CM_ST_INTERNAL;
  } else {
    m->in_spec = spec; m->in_spec_len = spec_len;
    m->in_bios = bios; m->in_bios_len = bios_len;
    st = lua_cpcall(m->L, load_body, m);
    CM_CHECK(lua_gettop(m->L) == (st != 0));
    m->in_spec = NULL; m->in_spec_len = 0;
    m->in_bios = NULL; m->in_bios_len = 0;
    if (st != 0) {
      msg_from_top(m, m->L);
      m->state = CM_DEAD;   /* a half-built state is never loaded or resumed again */
      res = CM_ST_ERROR;
    } else {
      msg_set(m, "", 0);
      res = CM_ST_OK;
    }
    lua_settop(m->L, 0);
  }
  machine_leave(m);
  return res;
}

/* -- Resume ----------------------------------------------------------------- */

/* PROTECTED. The body of the nested lua_cpcall in render_error: writes m->err_num into msg as Lua's tostring
** does. The conversion allocates a string, so it may raise "not enough memory".
*/
static int number_body(lua_State *L)
{
  cm_machine *m = machine_of(L);
  size_t n;
  const char *s;
  if (m == NULL) return raise_text(L, "CCLuaJIT: machine missing");
  lua_pushnumber(L, m->err_num);
  s = lua_tolstring(L, -1, &n);   /* converts the copy pushed here, never a value of the caller */
  msg_set(m, s, n);
  return 0;
}

/* PROTECTED (called by resume_body; raw, never runs a metamethod, and raises nothing itself). Writes the error
** value at idx into msg (M1.md section 3.7). A number is converted in a nested lua_cpcall, so a conversion that
** fails under an exhausted cap does not take the traceback with it (D7): msg then holds the message of that
** failure, "not enough memory", the text that a failed allocation raises everywhere else.
*/
static void render_error(cm_machine *m, lua_State *L, int idx)
{
  char buf[64];
  size_t n;
  const char *s;
  int top;
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  switch (lua_type(L, idx)) {
  case LUA_TSTRING:
    s = lua_tolstring(L, idx, &n);
    msg_set(m, s, n);
    break;
  case LUA_TNUMBER:
    m->err_num = lua_tonumber(L, idx);
    top = lua_gettop(L);
    if (lua_cpcall(L, number_body, m) != 0) msg_from_top(m, L);
    lua_settop(L, top);
    break;
  case LUA_TNIL:
    msg_setz(m, "nil");
    break;
  case LUA_TBOOLEAN:
    msg_setz(m, lua_toboolean(L, idx) ? "true" : "false");
    break;
  default:   /* "<type>: %08x" of a keyed hash: Cobalt's shape without an address */
    snprintf(buf, sizeof(buf), "%s: %08x", lua_typename(L, lua_type(L, idx)),
             (unsigned)mix32((uint64_t)(uintptr_t)lua_topointer(L, idx) ^ m->id_key));
    msg_setz(m, buf);
    break;
  }
}

/* PROTECTED. The body of the nested lua_cpcall in yield_filter: writes the filter record of m->err_num, the text
** of Lua's tostring under patch 05 (S2.md D25, F26). The conversion allocates a string, so it may raise "not
** enough memory".
*/
static int filter_number_body(lua_State *L)
{
  cm_machine *m = machine_of(L);
  size_t n;
  const char *s;
  if (m == NULL) return raise_text(L, "CCLuaJIT: machine missing");
  lua_pushnumber(L, m->err_num);
  s = lua_tolstring(L, -1, &n);   /* converts the copy pushed here, never a value of co (F27) */
  msg_filter(m, s, n);
  return 0;
}

/* PROTECTED (called by resume_body after a yield; raises nothing itself). Writes the filter record of the first
** yielded value into msg (S2.md section 4.3): a string as it is, a number as tostring prints it, anything else no
** filter. co is read only through the rule for co. A number is converted in a nested lua_cpcall on L; if that
** fails (only "not enough memory" under an exhausted cap), the machine ends with that error (D25).
*/
static void yield_filter(cm_machine *m, lua_State *L, lua_State *co)
{
  size_t n;
  const char *s;
  int top;
  if (lua_gettop(co) < 1) {
    msg_set(m, "", 0);
    return;
  }
  switch (lua_type(co, 1)) {
  case LUA_TSTRING:
    s = lua_tolstring(co, 1, &n);   /* a string: no conversion, no allocation */
    msg_filter(m, s, n);
    break;
  case LUA_TNUMBER:
    m->err_num = lua_tonumber(co, 1);
    top = lua_gettop(L);
    if (lua_cpcall(L, filter_number_body, m) != 0) {
      msg_from_top(m, L);
      m->state = CM_DEAD;
      m->result = CM_ST_ERROR;
    }
    lua_settop(L, top);
    break;
  default:   /* nil, boolean, table, function, thread, userdata: no filter */
    msg_set(m, "", 0);
    break;
  }
}

/* UNPROTECTED (pure). The length of s, reading at most max bytes. */
static size_t cstr_len(const char *s, size_t max)
{
  size_t n = 0;
  if (s == NULL) return 0;
  while (n < max && s[n] != '\0') n++;
  return n;
}

/* UNPROTECTED (pure). Formats one traceback line, "\n\t<frame>", from an ar that lua_getinfo(co, "Sln") filled.
** Cobalt's wording (F21). No address, "[builtin#N]", "[builtin:...]" or C function pointer is ever printed.
*/
static int frame_text(char *out, size_t cap, const lua_Debug *ar)
{
  char what[200];
  size_t srclen = cstr_len(ar->short_src, LUA_IDSIZE - 1);
  const char *kind = ar->what ? ar->what : "";
  const char *nw = ar->namewhat ? ar->namewhat : "";
  /* LuaJIT's bytecode builtins (table.remove, string.len, ...) are Lua functions named "[builtin:...]". */
  int cfunc = strcmp(kind, "C") == 0 || (srclen >= 9 && memcmp(ar->short_src, "[builtin:", 9) == 0);
  int named = ar->name != NULL && nw[0] != '\0' && !(strcmp(nw, "local") == 0 && ar->name[0] == '(');
  if (named) {
    int nlen = (int)cstr_len(ar->name, TB_NAME_MAX);
    if (strcmp(nw, "global") == 0) snprintf(what, sizeof(what), "function '%.*s'", nlen, ar->name);
    else snprintf(what, sizeof(what), "%.20s '%.*s'", nw, nlen, ar->name);
  } else if (!cfunc && strcmp(kind, "main") == 0) {
    snprintf(what, sizeof(what), "main chunk");
  } else if (!cfunc && strcmp(kind, "Lua") == 0) {
    snprintf(what, sizeof(what), "function <%.*s:%d>", (int)srclen, ar->short_src, ar->linedefined);
  } else {
    snprintf(what, sizeof(what), "?");
  }
  if (cfunc) return snprintf(out, cap, "\n\t[C]: in %s", what);
  if (ar->currentline > 0)
    return snprintf(out, cap, "\n\t%.*s:%d: in %s", (int)srclen, ar->short_src, ar->currentline, what);
  return snprintf(out, cap, "\n\t%.*s: in %s", (int)srclen, ar->short_src, what);
}

/* UNPROTECTED (allocates nothing and cannot raise; D7). Appends "\nstack traceback:" and one "\n\t<frame>" per
** frame of co to msg, after the rendered error value, and sets msg_tb. Lines that do not fit whole are left out;
** the rendered value is never cut for them. co is read only through lua_getstack and lua_getinfo(co, "Sln") (F20).
** The lua_Debug rule: lua_getinfo only gets an ar that lua_getstack filled for that level just before and that
** returned 1; the frame count goes into a separate cnt (lua_getstack leaves a count in i_ci when it returns 0).
*/
static void append_traceback(cm_machine *m, lua_State *co)
{
  static const char header[] = "\nstack traceback:";
  static const char dots[] = "\n\t...";
  char line[512];
  lua_Debug cnt, ar;
  int level, nframes = 0, elide = 0, len;
  size_t at = m->msg_len;

  m->msg_tb = SIZE_MAX;
  if (!msg_append(m, header, sizeof(header) - 1)) return;
  m->msg_tb = at;
  /* Elision, decided before the frame loop: with 24 frames or more, print levels 0-11, "...", then the last 10.
  ** lua_getstack(co, -10) walks every frame once and leaves the count in i_ci. Level -1 is never used: it
  ** matches the dummy frame after a stack overflow.
  */
  if (lua_getstack(co, TB_LEVELS1 + TB_LEVELS2 + 1, &cnt) && !lua_getstack(co, -TB_LEVELS2, &cnt) &&
      cnt.i_ci > TB_LEVELS1 + TB_LEVELS2 + 1) {
    elide = 1;
    nframes = cnt.i_ci;
  }
  for (level = 0;; level++) {
    if (elide && level == TB_LEVELS1) {
      if (!msg_append(m, dots, sizeof(dots) - 1)) return;
      level = nframes - TB_LEVELS2;
    }
    if (!lua_getstack(co, level, &ar)) return;
    if (!lua_getinfo(co, "Sln", &ar)) return;
    len = frame_text(line, sizeof(line), &ar);
    if (len < 0 || (size_t)len >= sizeof(line) || !msg_append(m, line, (size_t)len)) return;
  }
}

/* PROTECTED. The body of cm_resume (M1.md section 3.7, resume_body). */
static int resume_body(lua_State *L)
{
  cm_machine *m = machine_of(L);
  lua_State *co;
  wire_rd r;
  wire_val v;
  int st, base, n, k;

  lua_settop(L, 0);
  if (m == NULL) return raise_text(L, "CCLuaJIT: machine missing");
  lua_rawgeti(L, LUA_REGISTRYINDEX, m->co_ref);
  co = lua_tothread(L, -1);
  if (co == NULL) return raise_text(L, "bios coroutine missing");
  /* co stays at index 1 of L until this body returns. co_ref alone does not keep it alive here: once Lua can
  ** reach the registry (S4: debug.getregistry), the bios can remove co_ref, and an allocation on L before the
  ** last use of co (the number conversions in render_error and yield_filter) can run a GC cycle, which marks L's
  ** stack but not a coroutine that nothing refers to. During the event push the registry still refers to co: no
  ** Lua code runs between the lua_rawgeti above and lua_resume, because nothing has a __gc metamethod (I5).
  */
  /* F4: a coroutine is resumed only in the state the machine expects; anything else is a bug. */
  if (lua_status(co) != (m->state == CM_SUSPENDED ? LUA_YIELD : 0)) {
    m->state = CM_DEAD;
    return raise_text(L, "CCLuaJIT: bios coroutine in an unexpected state");
  }
#ifdef CCLJ_TEST
  m->test_cotop_resume = lua_gettop(co);   /* when the resume starts, before the event */
#endif
  base = m->state == CM_NEW ? 1 : 0;   /* the bios function waits on co until the first resume */
  CM_CHECK(lua_gettop(L) == 1 && lua_gettop(co) == base);
  n = 0;
  if (m->in_event != NULL) {   /* 5. The event (S2): its values are pushed onto L above co, then moved to co. */
    n = (int)m->in_event_n;
    if (n < 1 || n + 1 > LUAI_MAXCSTACK - lua_gettop(co)) {   /* cm_resume checked both: never reached */
      msg_setz(m, "event has too many values");
      m->result = CM_ST_INTERNAL;   /* co and state untouched (I10) */
      return 0;
    }
    if (!lua_checkstack(L, n)) return raise_text(L, "not enough memory");   /* size cannot fail: 1 + n <= 8000 */
    r.p = m->in_event + 4; r.left = m->in_event_len - 4;   /* past the checked count */
    for (k = 0; k < n; k++) {
      if (!rd_value(&r, &v)) return raise_text(L, "CCLuaJIT: corrupt event");   /* never: checked */
      push_value(L, &v);   /* lua_pushlstring may raise "not enough memory" */
    }
    CM_CHECK(lua_gettop(L) == 1 + n);
    /* Nothing may allocate on any thread from here to the lua_xmove: a GC step can shrink co's stack again
    ** (S2.md section 4.7), and lua_xmove would then grow co and raise on co (F1).
    */
#ifdef CCLJ_TEST
    if (m->test_grow_fail) { m->test_grow_fail = 0; m->limit = m->used; }   /* one-shot */
    m->test_grow_before = lua_status(co);
#endif
    k = lua_checkstack(co, n + 1);   /* F1; its size test cannot fail here (checked above) */
#ifdef CCLJ_TEST
    m->test_grow_ret = k;
    m->test_grow_after = lua_status(co);
#endif
    if (!k) {   /* F19: co's status was reset, so it is never resumed again. Never raise here: */
      m->state = CM_DEAD;   /* cm_resume's msg_from_top would overwrite msg */
      msg_setz(m, "CCLuaJIT: cannot grow the bios coroutine stack");
      m->result = CM_ST_ERROR;
      return 0;
    }
    lua_xmove(L, co, n);
  }
  CM_CHECK(lua_gettop(L) == 1 && lua_gettop(co) == base + n);
  st = lua_resume(co, n);   /* 6 */
  if (st == LUA_YIELD) {   /* 7 */
    m->state = CM_SUSPENDED;
    m->result = CM_ST_YIELD;
    yield_filter(m, L, co);   /* the filter record; D25 may set ERROR and DEAD */
    lua_settop(co, 0);   /* the next event finds co empty (F23) */
  } else if (st == 0) {
    m->state = CM_DEAD;
    msg_set(m, "", 0);
    m->result = CM_ST_RETURNED;
  } else {
    m->state = CM_DEAD;
    if (lua_gettop(co) < 1 || !lua_checkstack(L, 2)) {   /* cannot happen: the error value is on co */
      msg_setz(m, "CCLuaJIT: error value missing");
    } else {
      lua_xmove(co, L, 1);
      render_error(m, L, -1);
      CM_CHECK(lua_gettop(L) == 2);   /* co and the error value: render_error is balanced */
#ifdef CCLJ_TEST
      m->test_cotop_before = lua_gettop(co);
#endif
      append_traceback(m, co);
#ifdef CCLJ_TEST
      m->test_cotop_after = lua_gettop(co);
#endif
    }
    m->result = CM_ST_ERROR;
  }
  return 0;
}

/* UNPROTECTED. */
int cm_resume(cm_machine *m, cm_host_fn fn, void *host, const unsigned char *event, size_t event_len)
{
  int st, res;
  uint32_t n = 0;
  if ((res = machine_enter(m)) != CM_ST_OK) return res;   /* INTERNAL or BUSY, nothing written */
  if (m->state == CM_DEAD) {
    msg_setz(m, "machine is dead");
    res = CM_ST_INTERNAL;
  } else if (event == NULL ? event_len != 0
                           : event_len > CM_WIRE_MAX || !wire_check_event(event, event_len, &n)) {
    msg_setz(m, "corrupt event");   /* I10: before any Lua call; the machine stays as it was */
    res = CM_ST_INTERNAL;
  } else if (n > CM_EVENT_MAX - (m->state == CM_NEW ? 1u : 0u)) {   /* F23: the bios function is on co */
    msg_setz(m, "event has too many values");
    res = CM_ST_INTERNAL;
  } else {
    m->in_event = event; m->in_event_len = event_len; m->in_event_n = n;
    m->host_fn = fn;
    m->host = host;
    m->result = CM_ST_INTERNAL;   /* every return 0 of resume_body sets it (I11) */
    st = lua_cpcall(m->L, resume_body, m);
    CM_CHECK(lua_gettop(m->L) == (st != 0));
    if (st != 0) {   /* a raise: "not enough memory" and the like */
      msg_from_top(m, m->L);
      m->state = CM_DEAD;
      m->result = CM_ST_ERROR;
    }
    lua_settop(m->L, 0);
    m->host_fn = NULL;
    m->host = NULL;
    m->in_event = NULL; m->in_event_len = 0; m->in_event_n = 0;   /* I12: no pointer into the caller's buffer */
    res = m->result;
  }
  machine_leave(m);   /* every path after a successful enter */
  return res;
}

/* -- Lifecycle -------------------------------------------------------------- */

/* UNPROTECTED. None of the calls here can raise. */
cm_machine *cm_new(size_t mem_limit, uint64_t id_key)
{
  cm_machine *m;
  lua_State *L;
  if (!test_alloc_ok()) return NULL;
  m = (cm_machine *)calloc(1, sizeof(cm_machine));
  if (m == NULL) return NULL;
  L = luaL_newstate();
  if (L == NULL) {
    free(m);
    return NULL;
  }
  lua_atpanic(L, cm_panic);
  m->orig_alloc = lua_getallocf(L, &m->orig_ud);
  m->used = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024 + (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
  m->limit = mem_limit;
  m->L = L;
  m->id_key = id_key;
  lua_setallocf(L, cm_alloc, m);
  m->co_ref = LUA_NOREF;
  m->state = CM_NEW;
  m->msg_len = 0;
  m->msg_tb = SIZE_MAX;
#ifdef CCLJ_TEST
  m->test_cotop_before = -1;
  m->test_cotop_after = -1;
  m->test_cotop_resume = -1;
  m->test_grow_ret = -1;
  m->test_grow_before = -1;
  m->test_grow_after = -1;
#endif
  m->magic = CM_MAGIC;
  return m;
}

/* UNPROTECTED. */
const unsigned char *cm_message(const cm_machine *m, size_t *len, size_t *tb_off)
{
  static const unsigned char none[1] = {0};
  if (m == NULL || m->magic != CM_MAGIC) {
    if (len) *len = 0;
    if (tb_off) *tb_off = SIZE_MAX;
    return none;
  }
  if (len) *len = m->msg_len;
  if (tb_off) *tb_off = m->msg_tb;
  return m->msg;
}

/* UNPROTECTED. lua_close runs no Lua code in M1: no newproxy, no userdata with __gc, no FFI. The busy flag is
** taken and never released: nothing may use the machine after this.
*/
int cm_free(cm_machine *m)
{
  int st;
  if (m == NULL) return CM_ST_OK;
  if ((st = machine_enter(m)) != CM_ST_OK) return st;   /* busy or wrong magic: not freed; Java logs a leak */
  lua_setallocf(m->L, m->orig_alloc, m->orig_ud);   /* so that lua_close destroys LuaJIT's arena at once */
  lua_close(m->L);
  free(m->req_buf.data);
  free(m->rep_buf.data);
  *(volatile uint32_t *)&m->magic = CM_MAGIC_FREED;
  free(m);
  return CM_ST_OK;
}
