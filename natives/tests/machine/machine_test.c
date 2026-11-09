/*
** machine_test.c: Tier 0 of S1, the native machine core (natives/src/machine.c) without a JVM
** (docs/port-1.21.1/M1.md section 3.15, items 1-10, and the "try to break" list of section 3.17).
**
** natives/tests/machine-test.sh builds it together with machine.c compiled with -DCCLJ_TEST, against an assert and
** API-check build of the patched LuaJIT, and runs it. The host functions talk to machine.c through wire format v1
** with their own small codec below, written from the format description, so machine.c's encoder and decoders are
** also checked against an independent implementation.
**
** usage: machine_test [--smoke]
**   --smoke  one machine end to end only: machine-test.sh uses it to see whether a sanitized build runs at all.
** Prints one line per check ("ok   <name>" or "FAIL <name>: <detail>") and a summary. Exit 0: every check passed;
** 1: a check failed.
*/
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#include "lua.h"
#include "lauxlib.h"
/* Internal: only to read the JIT flags and maxmcode of a loaded machine (test 1). The xcflags of the LuaJIT
** tree are on the command line, so these structures have the library's layout.
*/
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

#include "cclj_machine.h"

#define KEY   UINT64_C(0x9e3779b97f4a7c15)
#define LIMIT ((size_t)64 << 20)   /* the production cap */
#define MEM_GROWTH_MAX ((size_t)16 << 20)   /* test 10: process memory growth allowed over 1,000 machines */

/* The G1 bios (M1.md section 3.1) and its machine error (section 3.15 item 5). */
static const char G1_BIOS[] = "term.write(\"hi\")\nlocal x = 1\nerror(\"boom\")";
static const char G1_MSG[] =
  "bios.lua:3: boom\nstack traceback:\n\t[C]: in function 'error'\n\tbios.lua:3: in main chunk";

/* -- Report ------------------------------------------------------------------ */

static int n_checks, n_failed;   /* main thread only */

/* Printable form of n bytes (\n, \t, \\, \xNN), cut with "..." to fit cap. */
static const char *esc(const void *p, size_t n, char *out, size_t cap)
{
  static const char hex[] = "0123456789abcdef";
  const unsigned char *s = (const unsigned char *)p;
  size_t i, k = 0;
  for (i = 0; i < n && k + 8 < cap; i++) {
    unsigned c = s[i];
    if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
    else if (c == '\t') { out[k++] = '\\'; out[k++] = 't'; }
    else if (c == '\\') { out[k++] = '\\'; out[k++] = '\\'; }
    else if (c >= 0x20 && c < 0x7f) out[k++] = (char)c;
    else { out[k++] = '\\'; out[k++] = 'x'; out[k++] = hex[c >> 4]; out[k++] = hex[c & 15]; }
  }
  if (i < n) { out[k++] = '.'; out[k++] = '.'; out[k++] = '.'; }
  out[k] = '\0';
  return out;
}

static void check(int pass, const char *name, const char *fmt, ...)
{
  char d[1600] = "";
  if (fmt != NULL) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof(d), fmt, ap);
    va_end(ap);
  }
  n_checks++;
  if (!pass) n_failed++;
  printf("%s %s%s%s\n", pass ? "ok  " : "FAIL", name, d[0] ? ": " : "", d);
  fflush(stdout);
}

static void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);
  if (p == NULL) { fputs("machine_test: out of memory\n", stderr); exit(3); }
  return p;
}

/* -- Threads and time ---------------------------------------------------------- */

typedef struct thr_box { void (*fn)(void *); void *arg; } thr_box;

#ifdef _WIN32
typedef HANDLE thr_t;
static DWORD WINAPI thr_main(LPVOID p) { thr_box *b = (thr_box *)p; b->fn(b->arg); return 0; }
static int thr_start(thr_t *t, thr_box *b) { *t = CreateThread(NULL, 0, thr_main, b, 0, NULL); return *t != NULL; }
static void thr_join(thr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
/* The process's private bytes (committed memory), or -1. */
static double mem_bytes(void)
{
  PROCESS_MEMORY_COUNTERS_EX pmc;
  memset(&pmc, 0, sizeof(pmc));
  if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) return -1;
  return (double)pmc.PrivateUsage;
}
#else
typedef pthread_t thr_t;
static void *thr_main(void *p) { thr_box *b = (thr_box *)p; b->fn(b->arg); return NULL; }
static int thr_start(thr_t *t, thr_box *b) { return pthread_create(t, NULL, thr_main, b) == 0; }
static void thr_join(thr_t t) { pthread_join(t, NULL); }
static void sleep_ms(int ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
/* The process's resident bytes, or -1. */
static double mem_bytes(void)
{
  FILE *f = fopen("/proc/self/statm", "r");
  unsigned long size, resident;
  int n;
  if (f == NULL) return -1;
  n = fscanf(f, "%lu %lu", &size, &resident);
  fclose(f);
  return n == 2 ? (double)resident * (double)sysconf(_SC_PAGESIZE) : -1;
}
#endif

/* Runs fn(arg) on a new thread and waits for it. */
static int on_thread(void (*fn)(void *), void *arg)
{
  thr_box b;
  thr_t t;
  b.fn = fn; b.arg = arg;
  if (!thr_start(&t, &b)) return 0;
  thr_join(t);
  return 1;
}

/* -- Random numbers (xorshift64*, fixed seeds: every run is the same) --------- */

static uint64_t rnd(uint64_t *s)
{
  uint64_t x = *s;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  *s = x;
  return x * UINT64_C(0x2545f4914f6cdd1d);
}

/* -- Byte buffers (cm_buf, grown with realloc as the machine expects) ---------- */

static void cb_need(cm_buf *b, size_t k)
{
  if (b->len + k > b->cap) {
    size_t c = b->cap ? b->cap : 64;
    while (c < b->len + k) c *= 2;
    b->data = (unsigned char *)realloc(b->data, c);
    if (b->data == NULL) { fputs("machine_test: out of memory\n", stderr); exit(3); }
    b->cap = c;
  }
}
static void cb_u8(cm_buf *b, unsigned v) { cb_need(b, 1); b->data[b->len++] = (unsigned char)v; }
static void cb_u32(cm_buf *b, uint32_t v)
{
  int i;
  cb_need(b, 4);
  for (i = 0; i < 4; i++) b->data[b->len++] = (unsigned char)(v >> (8 * i));
}
static void cb_f64(cm_buf *b, double d)
{
  uint64_t u;
  int i;
  memcpy(&u, &d, 8);
  cb_need(b, 8);
  for (i = 0; i < 8; i++) b->data[b->len++] = (unsigned char)(u >> (8 * i));
}
static void cb_mem(cm_buf *b, const void *p, size_t n)
{
  cb_need(b, n);
  if (n > 0) memcpy(b->data + b->len, p, n);
  b->len += n;
}
static void cb_str(cm_buf *b, const void *p, size_t n) { cb_u32(b, (uint32_t)n); cb_mem(b, p, n); }
static void cb_strz(cm_buf *b, const char *s) { cb_str(b, s, strlen(s)); }
static void cb_cat(cm_buf *b, const char *s) { cb_mem(b, s, strlen(s)); }
static void cb_free(cm_buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* -- The test's own wire codec --------------------------------------------------- */

typedef struct tval {
  unsigned tag;                /* 0 nil, 1 false, 2 true, 3 number, 4 string */
  double num;
  const unsigned char *s;      /* points into the buffer that was read */
  size_t len;
} tval;

static uint32_t get_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t as_i32(uint32_t u)
{
  int32_t v;
  memcpy(&v, &u, 4);
  return v;
}

/* Reads one value; 0 if it is malformed. */
static int tv_read(const unsigned char **p, size_t *left, tval *v)
{
  uint32_t n;
  if (*left < 1) return 0;
  v->tag = **p;
  (*p)++; (*left)--;
  switch (v->tag) {
  case 0: case 1: case 2:
    return 1;
  case 3: {
    uint64_t u = 0;
    int i;
    if (*left < 8) return 0;
    for (i = 7; i >= 0; i--) u = (u << 8) | (*p)[i];
    memcpy(&v->num, &u, 8);
    *p += 8; *left -= 8;
    return 1;
  }
  case 4:
    if (*left < 4) return 0;
    n = get_u32(*p);
    *p += 4; *left -= 4;
    if (n > *left) return 0;
    v->s = *p; v->len = n;
    *p += n; *left -= n;
    return 1;
  default:
    return 0;
  }
}

static void tv_write(cm_buf *b, const tval *v)
{
  cb_u8(b, v->tag);
  if (v->tag == 3) cb_f64(b, v->num);
  else if (v->tag == 4) cb_str(b, v->s, v->len);
}

/* Equal as Lua sees them after a round trip: NaN equals NaN (Lua canonicalizes NaNs), -0 differs from 0. */
static int tv_equal(const tval *a, const tval *b)
{
  if (a->tag != b->tag) return 0;
  if (a->tag == 3) {
    uint64_t x, y;
    if (a->num != a->num) return b->num != b->num;
    memcpy(&x, &a->num, 8); memcpy(&y, &b->num, 8);
    return x == y;
  }
  if (a->tag == 4) return a->len == b->len && (a->len == 0 || memcmp(a->s, b->s, a->len) == 0);
  return 1;
}

static int tv_is_str(const tval *v, const char *s)
{
  size_t n = strlen(s);
  return v->tag == 4 && v->len == n && memcmp(v->s, s, n) == 0;
}

static tval tv_num(double d) { tval v; memset(&v, 0, sizeof(v)); v.tag = 3; v.num = d; return v; }
static tval tv_str(const void *s, size_t n)
{
  tval v;
  memset(&v, 0, sizeof(v));
  v.tag = 4; v.s = (const unsigned char *)s; v.len = n;
  return v;
}
static tval tv_bool(int b) { tval v; memset(&v, 0, sizeof(v)); v.tag = b ? 2 : 1; return v; }

static void reply_values(cm_buf *rep, const tval *v, int n)
{
  int i;
  cb_u8(rep, 0);
  cb_u32(rep, (uint32_t)n);
  for (i = 0; i < n; i++) tv_write(rep, &v[i]);
}
static void reply_none(cm_buf *rep) { reply_values(rep, NULL, 0); }
static void reply_one(cm_buf *rep, tval v) { reply_values(rep, &v, 1); }
/* An ERROR record; msg NULL is a nil message. */
static void reply_error(cm_buf *rep, int32_t level, const void *msg, size_t len)
{
  uint32_t u;
  memcpy(&u, &level, 4);
  cb_u8(rep, 1);
  cb_u32(rep, u);
  if (msg != NULL) { cb_u8(rep, 4); cb_str(rep, msg, len); }
  else cb_u8(rep, 0);
}

/* Reference reply decoder: the oracle of the reply fuzz. kind -1: invalid; 0: VALUES; 1: ERROR. */
#define REF_MAXV 4096
typedef struct refreply {
  int kind;
  uint32_t n;
  tval v[REF_MAXV];
  int32_t level;
  tval msg;
} refreply;

static void ref_reply(const unsigned char *d, size_t len, refreply *r)
{
  const unsigned char *p = d;
  size_t left = len;
  unsigned kind;
  uint32_t i;
  r->kind = -1;
  if (left < 1) return;
  kind = *p++; left--;
  if (kind == 0) {
    if (left < 4) return;
    r->n = get_u32(p); p += 4; left -= 4;
    if (r->n > left || r->n > REF_MAXV) return;   /* REF_MAXV is above anything the generators make */
    for (i = 0; i < r->n; i++)
      if (!tv_read(&p, &left, &r->v[i])) return;
    if (left != 0) return;
    r->kind = 0;
  } else if (kind == 1) {
    if (left < 4) return;
    r->level = as_i32(get_u32(p)); p += 4; left -= 4;
    if (!tv_read(&p, &left, &r->msg) || (r->msg.tag != 0 && r->msg.tag != 4) || left != 0) return;
    r->kind = 1;
  }
}

/* Reference spec checker: the oracle of the spec fuzz. 1 = valid; *nfn methods; up to maxnames API names. */
typedef struct refname { const unsigned char *s; size_t len; } refname;

static int ref_str(const unsigned char **p, size_t *left, const unsigned char **s, size_t *len)
{
  uint32_t n;
  if (*left < 4) return 0;
  n = get_u32(*p);
  *p += 4; *left -= 4;
  if (n > 256 || n > *left) return 0;
  *s = *p; *len = n;
  *p += n; *left -= n;
  return 1;
}

static int ref_spec(const unsigned char *d, size_t len, uint32_t *nfn, refname *names, int maxnames, int *nnames)
{
  const unsigned char *p = d, *s;
  size_t left = len, sl;
  uint32_t napi, a, k, nn, nm;
  uint64_t total = 0;
  *nnames = 0;
  if (left < 4) return 0;
  napi = get_u32(p); p += 4; left -= 4;
  for (a = 0; a < napi; a++) {
    if (left < 4) return 0;
    nn = get_u32(p); p += 4; left -= 4;
    for (k = 0; k < nn; k++) {
      if (!ref_str(&p, &left, &s, &sl)) return 0;
      if (*nnames < maxnames) { names[*nnames].s = s; names[*nnames].len = sl; (*nnames)++; }
    }
    if (left < 1 || *p > 1) return 0;
    if (*p++ == 1) { left--; if (!ref_str(&p, &left, &s, &sl)) return 0; }
    else left--;
    if (left < 4) return 0;
    nm = get_u32(p); p += 4; left -= 4;
    total += nm;
    if (total > 65536) return 0;
    for (k = 0; k < nm; k++) {
      if (!ref_str(&p, &left, &s, &sl)) return 0;
      if (left < 1 || (*p & ~1u) != 0) return 0;
      p++; left--;
    }
  }
  if (left != 0) return 0;
  *nfn = (uint32_t)total;
  return 1;
}

/* -- Host ---------------------------------------------------------------------- */

typedef struct fuzz fuzz;
typedef struct host host;
typedef int (*hfn)(host *h, cm_buf *rep);

struct host {
  cm_machine *m;
  tval *args;                       /* the decoded request, valid during the call */
  int nargs, args_cap;
  uint32_t fn;
  long calls, bad_req;              /* bad_req: malformed requests, or a reply buffer not empty on entry */
  unsigned char req[64];            /* the last request, if it was this short */
  size_t req_len;
  size_t req_max;
  char log[512];                    /* term.write output */
  size_t loglen;
  int marks;
  const unsigned char *raw;         /* t.raw() replies with these bytes (or from fz) */
  size_t rawlen;
  fuzz *fz;
  int re_resume, re_load, re_free;  /* t.reenter() */
  unsigned char re_msg[64];
  size_t re_msglen;
  int gate, blocked;                /* __atomic: t.block() sets blocked, then waits for gate */
};

static void host_init(host *h) { memset(h, 0, sizeof(*h)); }
static void host_done(host *h) { free(h->args); h->args = NULL; h->args_cap = 0; }

static hfn g_fns[32];
static int g_nfn;
static cm_buf g_spec;

static int host_call(void *hp, uint32_t fn, const cm_buf *req, cm_buf *rep)
{
  host *h = (host *)hp;
  const unsigned char *p = req->data;
  size_t left = req->len;
  uint32_t n, i;
  h->calls++;
  if (rep->len != 0) h->bad_req++;
  if (req->len > h->req_max) h->req_max = req->len;
  h->req_len = req->len;
  if (req->len <= sizeof(h->req)) memcpy(h->req, req->data, req->len);
  if (left < 4) { h->bad_req++; return CM_HOST_E_JAVA; }
  n = get_u32(p); p += 4; left -= 4;
  if (n > left) { h->bad_req++; return CM_HOST_E_JAVA; }
  if ((int)n > h->args_cap) {
    free(h->args);
    h->args_cap = (int)n + 16;
    h->args = (tval *)xmalloc(sizeof(tval) * (size_t)h->args_cap);
  }
  for (i = 0; i < n; i++)
    if (!tv_read(&p, &left, &h->args[i])) { h->bad_req++; return CM_HOST_E_JAVA; }
  if (left != 0) { h->bad_req++; return CM_HOST_E_JAVA; }
  h->nargs = (int)n;
  h->fn = fn;
  if (fn >= (uint32_t)g_nfn) { h->bad_req++; return CM_HOST_E_JAVA; }
  return g_fns[fn](h, rep);
}

static double arg_num(host *h, int i) { return i < h->nargs && h->args[i].tag == 3 ? h->args[i].num : 0; }

static int fuzz_serve(fuzz *fz, cm_buf *rep);
static int fuzz_check(fuzz *fz, host *h, cm_buf *rep);

/* t.echo(...): returns its arguments. */
static int h_echo(host *h, cm_buf *rep) { reply_values(rep, h->args, h->nargs); return CM_HOST_OK; }

/* t.err(level, msg): an ERROR record; a missing or non-string msg is nil. */
static int h_err(host *h, cm_buf *rep)
{
  int32_t level = (int32_t)arg_num(h, 0);
  if (h->nargs > 1 && h->args[1].tag == 4) reply_error(rep, level, h->args[1].s, h->args[1].len);
  else reply_error(rep, level, NULL, 0);
  return CM_HOST_OK;
}

/* t.raw(): replies with h->raw, or with the next fuzz case. */
static int h_raw(host *h, cm_buf *rep)
{
  if (h->fz != NULL) return fuzz_serve(h->fz, rep);
  cb_mem(rep, h->raw, h->rawlen);
  return CM_HOST_OK;
}

/* t.check(...): the fuzz oracle. */
static int h_check(host *h, cm_buf *rep) { return h->fz ? fuzz_check(h->fz, h, rep) : CM_HOST_E_JAVA; }

/* t.code(c): returns c as the host status, without a reply. */
static int h_code(host *h, cm_buf *rep) { (void)rep; return (int)arg_num(h, 0); }

/* t.reenter(): calls cm_resume, cm_load and cm_free on its own machine from inside the host call. */
static int h_reenter(host *h, cm_buf *rep)
{
  size_t len;
  const unsigned char *msg;
  tval v[4];
  h->re_resume = cm_resume(h->m, host_call, h, NULL, 0);
  h->re_load = cm_load(h->m, g_spec.data, g_spec.len, (const unsigned char *)"", 0);
  h->re_free = cm_free(h->m);
  /* White box: the header does not let a caller read the message after CM_ST_BUSY. Here it is race-free (the
  ** owner's own thread, and nothing writes the message during a host call), and it shows that the three refused
  ** calls wrote nothing.
  */
  msg = cm_message(h->m, &len, NULL);
  if (len > sizeof(h->re_msg)) len = sizeof(h->re_msg);
  memcpy(h->re_msg, msg, len);
  h->re_msglen = len;
  v[0] = tv_num(h->re_resume);
  v[1] = tv_str(h->re_msg, h->re_msglen);
  v[2] = tv_num(h->re_load);
  v[3] = tv_num(h->re_free);
  reply_values(rep, v, 4);
  return CM_HOST_OK;
}

static int h_mark(host *h, cm_buf *rep) { h->marks++; reply_none(rep); return CM_HOST_OK; }
static int h_nop(host *h, cm_buf *rep) { (void)h; reply_none(rep); return CM_HOST_OK; }
static int h_f(host *h, cm_buf *rep) { (void)h; reply_error(rep, 1, "f failed", 8); return CM_HOST_OK; }

/* t.block(): waits (up to 20 s) until another thread opens the gate. */
static int h_block(host *h, cm_buf *rep)
{
  double t0 = now_s();
  __atomic_store_n(&h->blocked, 1, __ATOMIC_SEQ_CST);
  while (!__atomic_load_n(&h->gate, __ATOMIC_SEQ_CST) && now_s() - t0 < 20) sleep_ms(1);
  reply_none(rep);
  return CM_HOST_OK;
}

/* t.argc(...): the wire tags of its arguments as a string, e.g. "0 2 4". */
static int h_argc(host *h, cm_buf *rep)
{
  char s[256];
  size_t k = 0;
  int i;
  for (i = 0; i < h->nargs && k + 3 < sizeof(s); i++) {
    if (i) s[k++] = ' ';
    s[k++] = (char)('0' + h->args[i].tag);
  }
  reply_one(rep, tv_str(s, k));
  return CM_HOST_OK;
}

/* LuaJIT's own count of its heap: what the cap must count too (M1.md section 3.7, cm_new step 5). */
static double gc_bytes(cm_machine *m)
{
  lua_State *L = (lua_State *)cm_test_state(m);
  return (double)lua_gc(L, LUA_GCCOUNT, 0) * 1024.0 + (double)lua_gc(L, LUA_GCCOUNTB, 0);
}

/* t.used(): the cap's count minus LuaJIT's own count, in the middle of a run (0 when the accounting is right). */
static int h_used(host *h, cm_buf *rep)
{
  reply_one(rep, tv_num((double)cm_test_used(h->m) - gc_bytes(h->m)));
  return CM_HOST_OK;
}

/* t.capnow(): lowers the cap to the bytes in use, so the next Lua allocation fails. */
static int h_capnow(host *h, cm_buf *rep)
{
  cm_test_limit(h->m, cm_test_used(h->m));
  reply_none(rep);
  return CM_HOST_OK;
}

/* term.write(s): appends s to the log. */
static int h_write(host *h, cm_buf *rep)
{
  if (h->nargs > 0 && h->args[0].tag == 4) {
    size_t n = h->args[0].len;
    if (n > sizeof(h->log) - 1 - h->loglen) n = sizeof(h->log) - 1 - h->loglen;
    memcpy(h->log + h->loglen, h->args[0].s, n);
    h->loglen += n;
    h->log[h->loglen] = '\0';
  }
  reply_none(rep);
  return CM_HOST_OK;
}

/* alias1.which(): its own function index. */
static int h_which(host *h, cm_buf *rep) { reply_one(rep, tv_num(h->fn)); return CM_HOST_OK; }

/* The default spec: 4 APIs, 16 host functions. */
typedef struct mdef { const char *name; unsigned flags; hfn fn; } mdef;

static const mdef T_METHODS[] = {
  {"echo", 0, h_echo}, {"err", 0, h_err}, {"raw", 0, h_raw}, {"check", 0, h_check}, {"code", 0, h_code},
  {"reenter", 0, h_reenter}, {"mark", 0, h_mark}, {"nop", 0, h_nop}, {"f", 0, h_f}, {"block", 0, h_block},
  {"argc", 0, h_argc}, {"used", 0, h_used}, {"capnow", 0, h_capnow}
};
static const mdef TERM_METHODS[] = {{"write", 0, h_write}};
static const mdef ALIAS_METHODS[] = {{"which", CM_WIRE_MAY_YIELD, h_which}, {"which2", 0, h_which}};
#define N_T     ((int)(sizeof(T_METHODS) / sizeof(T_METHODS[0])))
#define FN_WRITE N_T
#define FN_WHICH (N_T + 1)

static void spec_api(cm_buf *b, int nnames, const char *n1, const char *n2, const char *module,
                     const mdef *ms, int nm)
{
  int i;
  cb_u32(b, (uint32_t)nnames);
  cb_strz(b, n1);
  if (nnames > 1) cb_strz(b, n2);
  if (module) { cb_u8(b, 1); cb_strz(b, module); } else cb_u8(b, 0);
  cb_u32(b, (uint32_t)nm);
  for (i = 0; i < nm; i++) {
    cb_strz(b, ms[i].name);
    cb_u8(b, ms[i].flags);
    g_fns[g_nfn++] = ms[i].fn;
  }
}

static void build_spec(void)
{
  g_nfn = 0;
  cb_u32(&g_spec, 4);
  spec_api(&g_spec, 1, "t", NULL, "t", T_METHODS, N_T);
  spec_api(&g_spec, 1, "term", NULL, NULL, TERM_METHODS, 1);
  spec_api(&g_spec, 2, "alias1", "alias2", "aliasmod", ALIAS_METHODS, 2);
  spec_api(&g_spec, 1, "empty", NULL, "emptymod", NULL, 0);
}

/* -- Running a machine ----------------------------------------------------------- */

typedef struct outcome {
  int load_st, st, free_st;   /* -1: not called */
  unsigned char *msg;         /* copy of cm_message after the last call, NUL-terminated */
  size_t len, tb;
  int cotop_b, cotop_a;
} outcome;

static long g_cotop_errors, g_cotop_runs;   /* lua_gettop(co) changed by append_traceback */
static long g_tb_leaks;                     /* tracebacks with "0x" or "[builtin" in them */
static long g_tb_badline;                   /* tracebacks with a line number of 0 or below (":-", ":0: in") */

static void out_init(outcome *o) { memset(o, 0, sizeof(*o)); o->load_st = o->st = o->free_st = -1; o->tb = SIZE_MAX; }
static void out_free(outcome *o) { free(o->msg); o->msg = NULL; }

static void out_msg(outcome *o, const cm_machine *m)
{
  const unsigned char *p = cm_message(m, &o->len, &o->tb);
  free(o->msg);
  o->msg = (unsigned char *)xmalloc(o->len + 1);
  memcpy(o->msg, p, o->len);
  o->msg[o->len] = '\0';
}

static void note_cotop(outcome *o, const cm_machine *m)
{
  cm_test_cotop(m, &o->cotop_b, &o->cotop_a);
  if (o->st == CM_ST_ERROR && o->tb != SIZE_MAX) {   /* also called on the churn threads */
    size_t i;
    __atomic_add_fetch(&g_cotop_runs, 1, __ATOMIC_SEQ_CST);
    if (o->cotop_b != o->cotop_a) __atomic_add_fetch(&g_cotop_errors, 1, __ATOMIC_SEQ_CST);
    for (i = o->tb; i + 2 <= o->len; i++) {
      if ((o->msg[i] == '0' && o->msg[i + 1] == 'x') ||
          (i + 8 <= o->len && memcmp(o->msg + i, "[builtin", 8) == 0)) {
        __atomic_add_fetch(&g_tb_leaks, 1, __ATOMIC_SEQ_CST);
        break;
      }
    }
    /* A Lua frame without a line leaves "<line>:" out (M1.md section 3.7), so no frame shows line 0 or -1. */
    for (i = o->tb; i + 2 <= o->len; i++) {
      if ((o->msg[i] == ':' && o->msg[i + 1] == '-') ||
          (i + 6 <= o->len && memcmp(o->msg + i, ":0: in", 6) == 0)) {
        __atomic_add_fetch(&g_tb_badline, 1, __ATOMIC_SEQ_CST);
        break;
      }
    }
  }
}

/* Cap accounting: the cap counts exactly the bytes that LuaJIT counts (M1.md section 3.7: cm_new starts from
** LUA_GCCOUNT and LUA_GCCOUNTB; cm_alloc counts every growth, shrink and free). Checked between calls.
*/
static long g_acct_checks, g_acct_bad;   /* __atomic */
static int g_acct_first_set;
static char g_acct_first[200];

static void acct_note(cm_machine *m, const char *when)
{
  double used = (double)cm_test_used(m), gc = gc_bytes(m);
  int zero = 0;
  __atomic_add_fetch(&g_acct_checks, 1, __ATOMIC_SEQ_CST);
  if (used == gc) return;
  __atomic_add_fetch(&g_acct_bad, 1, __ATOMIC_SEQ_CST);
  if (__atomic_compare_exchange_n(&g_acct_first_set, &zero, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    snprintf(g_acct_first, sizeof(g_acct_first), "%s: the cap counts %.0f bytes, LuaJIT %.0f", when, used, gc);
}

/* cm_load with the default spec and a NUL-terminated bios. */
static int load_z(cm_machine *m, const char *bios)
{
  return cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)bios, strlen(bios));
}

/* New machine, load the default spec and bios, resume once, free. */
static void run_len(host *h, size_t limit, const char *bios, size_t blen, outcome *o)
{
  cm_machine *m = cm_new(limit, KEY);
  out_init(o);
  if (m == NULL) return;
  h->m = m;
  o->load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)bios, blen);
  out_msg(o, m);
  acct_note(m, "after cm_load");
  if (o->load_st == CM_ST_OK) {
    o->st = cm_resume(m, host_call, h, NULL, 0);
    out_msg(o, m);
    note_cotop(o, m);
    acct_note(m, "after cm_resume");
  }
  o->free_st = cm_free(m);
  h->m = NULL;
}
static void run(host *h, const char *bios, outcome *o) { run_len(h, LIMIT, bios, strlen(bios), o); }

/* The rendered value: the message before the traceback. */
static size_t head_len(const outcome *o) { return o->tb == SIZE_MAX ? o->len : o->tb; }
static int head_is(const outcome *o, const char *want)
{
  size_t n = strlen(want);
  return head_len(o) == n && memcmp(o->msg, want, n) == 0;
}
static int msg_is(const outcome *o, const char *want)
{
  size_t n = strlen(want);
  return o->len == n && memcmp(o->msg, want, n) == 0;
}
static int msg_has(const outcome *o, const char *part)
{
  size_t n = strlen(part), i;
  for (i = 0; i + n <= o->len; i++)
    if (memcmp(o->msg + i, part, n) == 0) return 1;
  return 0;
}
static int tb_has(const outcome *o, const char *part)
{
  size_t n = strlen(part), i;
  if (o->tb == SIZE_MAX) return 0;
  for (i = o->tb; i + n <= o->len; i++)
    if (memcmp(o->msg + i, part, n) == 0) return 1;
  return 0;
}
static const char *show(const outcome *o, char *buf, size_t cap) { return esc(o->msg, o->len, buf, cap); }

/* One check: the bios ran to status st (load OK, freed OK) and its rendered value is head. */
static void expect(const char *name, host *h, const char *bios, int st, const char *head)
{
  outcome o;
  char b[1200];
  run(h, bios, &o);
  check(o.load_st == CM_ST_OK && o.st == st && o.free_st == CM_ST_OK && head_is(&o, head), name,
        "load %d, resume %d (want %d), free %d, message \"%s\"", o.load_st, o.st, st, o.free_st,
        show(&o, b, sizeof(b)));
  out_free(&o);
}

/* Number of "\n\t" frame lines in the traceback, and whether line 13 is "...". */
static int tb_lines(const outcome *o, int *dots_at)
{
  size_t i;
  int n = 0;
  *dots_at = 0;
  if (o->tb == SIZE_MAX) return -1;
  for (i = o->tb; i + 1 < o->len; i++) {
    if (o->msg[i] == '\n' && o->msg[i + 1] == '\t') {
      n++;
      if (i + 5 <= o->len && memcmp(o->msg + i, "\n\t...", 5) == 0 && (i + 5 == o->len || o->msg[i + 5] == '\n'))
        *dots_at = n;
    }
  }
  return n;
}

/* -- 1. Allow-lists -------------------------------------------------------------- */

typedef struct keys {
  char g[4096], loaded[2048], reg[1024];
  int nonstring_g, nonstring_loaded, preload_type, preload_n;
  int jit_on, maxmcode;
  char loaded_bad[256];   /* the _LOADED entries that are not the expected table */
} keys;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The sorted string keys of the table at absolute index t, joined by spaces. Non-string keys are counted. */
static void join_keys(lua_State *L, int t, char *out, size_t cap, int *nonstring)
{
  char *names[1024];
  int n = 0, i;
  size_t k = 0;
  lua_pushnil(L);
  while (lua_next(L, t)) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      size_t len;
      const char *s = lua_tolstring(L, -2, &len);
      if (n < 1024) {
        names[n] = (char *)xmalloc(len + 1);
        memcpy(names[n], s, len + 1);
        n++;
      }
    } else if (nonstring) {
      (*nonstring)++;
    }
    lua_pop(L, 1);
  }
  qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
  for (i = 0; i < n; i++) {
    size_t l = strlen(names[i]);
    if (k + l + 2 < cap) {
      if (i) out[k++] = ' ';
      memcpy(out + k, names[i], l);
      k += l;
    }
    free(names[i]);
  }
  out[k] = '\0';
}

static int keys_body(lua_State *L)
{
  keys *k = (keys *)lua_touserdata(L, 1);
  lua_settop(L, 0);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  join_keys(L, 1, k->g, sizeof(k->g), &k->nonstring_g);
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  join_keys(L, 2, k->loaded, sizeof(k->loaded), &k->nonstring_loaded);
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  join_keys(L, 3, k->reg, sizeof(k->reg), NULL);
  lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
  k->preload_type = lua_type(L, 4);
  if (k->preload_type == LUA_TTABLE) {
    lua_pushnil(L);
    while (lua_next(L, 4)) { k->preload_n++; lua_pop(L, 1); }
  }
  /* Each _LOADED entry is the very table that _G has (an API table under its module name, M1.md section 3.7
  ** load_body step 5), and _LOADED.bit, which _G no longer has, is still the bit library.
  */
  {
    static const char *const pairs[][2] = {
      {"t", "t"}, {"aliasmod", "alias1"}, {"emptymod", "empty"}, {"_G", "_G"}, {"string", "string"},
      {"table", "table"}, {"math", "math"}, {"coroutine", "coroutine"}
    };
    size_t i, at = 0;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
      lua_getfield(L, 2, pairs[i][0]);
      lua_getfield(L, 1, pairs[i][1]);
      if (lua_type(L, -1) != LUA_TTABLE || !lua_rawequal(L, -1, -2))
        at += (size_t)snprintf(k->loaded_bad + at, sizeof(k->loaded_bad) - at, "%s ", pairs[i][0]);
      lua_pop(L, 2);
      if (at >= sizeof(k->loaded_bad)) at = sizeof(k->loaded_bad) - 1;
    }
    lua_getfield(L, 2, "bit");
    if (lua_type(L, -1) == LUA_TTABLE) lua_getfield(L, -1, "band");
    if (lua_type(L, -1) != LUA_TFUNCTION) snprintf(k->loaded_bad + at, sizeof(k->loaded_bad) - at, "bit");
  }
  lua_settop(L, 0);
  k->jit_on = (L2J(L)->flags & JIT_F_ON) != 0;
  k->maxmcode = (int)L2J(L)->param[JIT_P_maxmcode];
  return 0;
}

static const char WANT_G[] =
  "_G _VERSION alias1 alias2 assert coroutine empty error getfenv getmetatable ipairs load loadstring math next "
  "pairs pcall rawequal rawget rawlen rawset select setfenv setmetatable string t table term tonumber tostring type "
  "unpack xpcall";
static const char WANT_LOADED[] = "_G aliasmod bit coroutine emptymod math string t table";
static const char WANT_REG[] = "_LOADED _PRELOAD";

static void test_allow_lists(void)
{
  host h;
  keys k;
  cm_machine *m;
  int st, cp;
  host_init(&h);
  memset(&k, 0, sizeof(k));
  m = cm_new(LIMIT, KEY);
  acct_note(m, "after cm_new");
  st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"", 0);
  acct_note(m, "after cm_load");
  cp = lua_cpcall((lua_State *)cm_test_state(m), keys_body, &k);
  check(st == CM_ST_OK && cp == 0, "1 allow-lists: state inspected", "load %d, inspection %d", st, cp);
  check(strcmp(k.g, WANT_G) == 0 && k.nonstring_g == 0, "1 allow-lists: keys of _G",
        "got \"%s\" (%d non-string keys)", k.g, k.nonstring_g);
  check(strcmp(k.loaded, WANT_LOADED) == 0 && k.nonstring_loaded == 0, "1 allow-lists: keys of _LOADED",
        "got \"%s\" (%d non-string keys)", k.loaded, k.nonstring_loaded);
  check(k.loaded_bad[0] == '\0', "1 allow-lists: each _LOADED entry is the table that _G has",
        "entries that differ: %s", k.loaded_bad[0] ? k.loaded_bad : "none");
  check(strcmp(k.reg, WANT_REG) == 0, "1 allow-lists: string keys of the registry", "got \"%s\"", k.reg);
  check(k.preload_type == LUA_TTABLE && k.preload_n == 0, "1 allow-lists: _PRELOAD is an empty table",
        "type %d, %d entries", k.preload_type, k.preload_n);
  check(k.jit_on && k.maxmcode == 256, "1 allow-lists: the JIT is on and maxmcode is 256 KiB",
        "JIT %s, maxmcode %d", k.jit_on ? "on" : "off", k.maxmcode);
  cm_free(m);

  /* The same from Lua: what the removals must have removed (M1.md section 3.15, Tier 2 scenario 17). */
  expect("1 allow-lists: removed globals and string.dump are absent", &h,
         "local names = {'io', 'os', 'package', 'require', 'dofile', 'loadfile', 'collectgarbage', 'gcinfo',\n"
         "  'newproxy', 'print', 'jit', 'debug', 'bit', 'module', 'ffi', 'bit32', 'utf8'}\n"
         "local present = {}\n"
         "for _, n in ipairs(names) do if _G[n] ~= nil then present[#present + 1] = n end end\n"
         "if string.dump ~= nil then present[#present + 1] = 'string.dump' end\n"
         "error('present:' .. table.concat(present, ' '), 0)",
         CM_ST_ERROR, "present:");
  expect("1 allow-lists: API tables, aliases and an empty API", &h,
         "error(tostring(alias1 == alias2) .. ' ' .. type(t.echo) .. ' ' .. type(term.write) .. ' ' ..\n"
         "  tostring(next(empty)) .. ' ' .. tostring(rawequal(_G, _G._G)), 0)",
         CM_ST_ERROR, "true function function nil true");
  host_done(&h);
}

/* -- 2. Load ---------------------------------------------------------------------- */

static int dump_writer(lua_State *L, const void *p, size_t n, void *ud)
{
  (void)L;
  cb_mem((cm_buf *)ud, p, n);
  return 0;
}

static void load_case(const char *name, const unsigned char *spec, size_t slen, const char *bios, size_t blen,
                      int want_st, const char *want_msg)
{
  outcome o;
  char b[600];
  cm_machine *m = cm_new(LIMIT, KEY);
  out_init(&o);
  o.load_st = cm_load(m, spec, slen, (const unsigned char *)bios, blen);
  out_msg(&o, m);
  o.free_st = cm_free(m);
  check(o.load_st == want_st && o.free_st == CM_ST_OK && (want_msg == NULL || msg_is(&o, want_msg)) &&
        (want_st == CM_ST_OK || o.tb == SIZE_MAX), name, "load %d (want %d), msg_tb %s, message \"%s\"", o.load_st,
        want_st, o.tb == SIZE_MAX ? "none" : "set", show(&o, b, sizeof(b)));
  out_free(&o);
}

static void test_load(void)
{
  cm_buf bc = {0}, big = {0};
  lua_State *L2;
  cm_machine *m;
  outcome o;
  host h;
  char b[600];
  int i, st;

  load_case("2 load: a bios loads", g_spec.data, g_spec.len, "return", 6, CM_ST_OK, "");
  load_case("2 load: syntax error", g_spec.data, g_spec.len, "x = = 1", 7, CM_ST_ERROR,
            "bios.lua:1: unexpected symbol near '='");
  load_case("2 load: a chunk starting with ESC is refused", g_spec.data, g_spec.len, "\033LJ\002\000", 5,
            CM_ST_ERROR, "attempt to load a binary chunk");
  /* Real LuaJIT bytecode, from a separate plain state. */
  L2 = luaL_newstate();
  if (L2 != NULL && luaL_loadstring(L2, "return 1") == 0) lua_dump(L2, dump_writer, &bc);
  if (L2 != NULL) lua_close(L2);
  load_case("2 load: real bytecode is refused", g_spec.data, g_spec.len, (const char *)bc.data, bc.len,
            CM_ST_ERROR, "attempt to load a binary chunk");
  cb_free(&bc);
  load_case("2 load: a 0-byte bios loads", g_spec.data, g_spec.len, "", 0, CM_ST_OK, "");
  host_init(&h);
  run_len(&h, LIMIT, NULL, 0, &o);
  check(o.load_st == CM_ST_OK && o.st == CM_ST_RETURNED && o.len == 0 && o.free_st == CM_ST_OK,
        "2 load: a 0-byte bios returns RETURNED", "load %d, resume %d, message \"%s\"", o.load_st, o.st,
        show(&o, b, sizeof(b)));
  out_free(&o);
  load_case("2 load: an empty spec is corrupt", NULL, 0, "", 0, CM_ST_ERROR, "CCLuaJIT: corrupt API table description");
  load_case("2 load: a truncated spec is corrupt", g_spec.data, g_spec.len - 1, "", 0, CM_ST_ERROR,
            "CCLuaJIT: corrupt API table description");
  /* 65,536 methods load; 65,537 and 70,000 do not. */
  for (i = 0; i < 3; i++) {
    uint32_t n = i == 0 ? 65536u : i == 1 ? 65537u : 70000u, k;
    char nm[16];
    big.len = 0;
    cb_u32(&big, 1); cb_u32(&big, 1); cb_strz(&big, "many"); cb_u8(&big, 0); cb_u32(&big, n);
    for (k = 0; k < n; k++) { snprintf(nm, sizeof(nm), "m%u", (unsigned)k); cb_strz(&big, nm); cb_u8(&big, 0); }
    if (i == 0) {
      load_case("2 load: a spec with 65,536 methods loads", big.data, big.len, "", 0, CM_ST_OK, "");
    } else {
      load_case(i == 1 ? "2 load: a spec with 65,537 methods is corrupt"
                       : "2 load: a spec with 70,000 methods is corrupt",
                big.data, big.len, "", 0, CM_ST_ERROR, "CCLuaJIT: corrupt API table description");
    }
  }
  cb_free(&big);
  {
    cm_buf s = {0};
    cb_u32(&s, 1); cb_u32(&s, 1); cb_strz(&s, "x"); cb_u8(&s, 0); cb_u32(&s, 1); cb_strz(&s, "m"); cb_u8(&s, 2);
    load_case("2 load: unknown method flag bits are corrupt", s.data, s.len, "", 0, CM_ST_ERROR,
              "CCLuaJIT: corrupt API table description");
    s.len = 0;
    cb_u32(&s, 1); cb_u32(&s, 1); cb_strz(&s, "x"); cb_u8(&s, 2); cb_u32(&s, 0);
    load_case("2 load: hasModule 2 is corrupt", s.data, s.len, "", 0, CM_ST_ERROR,
              "CCLuaJIT: corrupt API table description");
    s.len = 0;
    cb_u32(&s, 1); cb_u32(&s, 1);
    cb_u32(&s, 257); for (i = 0; i < 257; i++) cb_u8(&s, 'a');
    cb_u8(&s, 0); cb_u32(&s, 0);
    load_case("2 load: a 257-byte name is corrupt", s.data, s.len, "", 0, CM_ST_ERROR,
              "CCLuaJIT: corrupt API table description");
    s.len = 0;
    cb_u32(&s, 1); cb_u32(&s, 1);
    cb_u32(&s, 256); for (i = 0; i < 256; i++) cb_u8(&s, 'a');
    cb_u8(&s, 0); cb_u32(&s, 0);
    load_case("2 load: a 256-byte name loads", s.data, s.len, "", 0, CM_ST_OK, "");
    cb_u8(&s, 0);
    load_case("2 load: a trailing byte after the spec is corrupt", s.data, s.len, "", 0, CM_ST_ERROR,
              "CCLuaJIT: corrupt API table description");
    s.len = 0;
    cb_u32(&s, 0xffffffffu);
    load_case("2 load: napi 0xFFFFFFFF is corrupt", s.data, s.len, "", 0, CM_ST_ERROR,
              "CCLuaJIT: corrupt API table description");
    cb_free(&s);
  }

  /* Misuse: a second load, a load after a failed load, bad arguments, no machine. */
  m = cm_new(LIMIT, KEY);
  st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  out_init(&o);
  o.load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  out_msg(&o, m);
  check(st == CM_ST_OK && o.load_st == CM_ST_INTERNAL && msg_is(&o, "bios is already loaded") && o.tb == SIZE_MAX,
        "2 load: a second load is INTERNAL", "first %d, second %d \"%s\"", st, o.load_st, show(&o, b, sizeof(b)));
  check(cm_free(m) == CM_ST_OK, "2 load: freed after a second load", NULL);
  m = cm_new(LIMIT, KEY);
  st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"x = = 1", 7);
  o.load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  out_msg(&o, m);
  check(st == CM_ST_ERROR && o.load_st == CM_ST_INTERNAL && msg_is(&o, "machine is dead"),
        "2 load: a load after a failed load is INTERNAL", "first %d, second %d \"%s\"", st, o.load_st,
        show(&o, b, sizeof(b)));
  o.st = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(o.st == CM_ST_INTERNAL && msg_is(&o, "machine is dead") && o.tb == SIZE_MAX,
        "2 load: a resume after a failed load is INTERNAL", "resume %d \"%s\"", o.st, show(&o, b, sizeof(b)));
  cm_free(m);
  m = cm_new(LIMIT, KEY);
  o.load_st = cm_load(m, NULL, 5, (const unsigned char *)"", 0);
  out_msg(&o, m);
  st = cm_load(m, g_spec.data, g_spec.len, NULL, 3);
  check(o.load_st == CM_ST_INTERNAL && st == CM_ST_INTERNAL && msg_is(&o, "bad load arguments"),
        "2 load: NULL inputs with a length are INTERNAL", "%d, %d \"%s\"", o.load_st, st, show(&o, b, sizeof(b)));
  st = cm_load(m, g_spec.data, (size_t)CM_WIRE_MAX + 1, (const unsigned char *)"", 0);
  check(st == CM_ST_INTERNAL, "2 load: a spec above CM_WIRE_MAX is INTERNAL", "%d", st);
  st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  check(st == CM_ST_OK, "2 load: the machine still loads after refused calls", "%d", st);
  cm_free(m);
  out_free(&o);
  {
    size_t len = 99, tb = 0;
    const unsigned char *p = cm_message(NULL, &len, &tb);
    check(cm_load(NULL, g_spec.data, g_spec.len, (const unsigned char *)"", 0) == CM_ST_INTERNAL &&
          cm_resume(NULL, host_call, &h, NULL, 0) == CM_ST_INTERNAL && cm_free(NULL) == CM_ST_OK &&
          p != NULL && len == 0 && tb == SIZE_MAX, "2 load: a NULL machine", NULL);
  }
  host_done(&h);
}

/* -- 3. Spec fuzz ------------------------------------------------------------------- */

/* Mutates d[0..len) in place (1 to 3 operations); returns the new length (at most cap). */
static size_t mutate(uint64_t *rs, unsigned char *d, size_t len, size_t cap)
{
  static const unsigned char bytes[] = {0, 1, 2, 3, 4, 5, 15, 0x7f, 0x80, 0xff};
  static const uint32_t words[] = {0, 1, 2, 255, 256, 257, 65535, 65536, 65537, 0x7fffffffu, 0x80000000u,
                                   0xfffffffeu, 0xffffffffu};
  int ops = 1 + (int)(rnd(rs) % 3);
  while (ops-- > 0) {
    uint64_t r = rnd(rs);
    switch (r % 8) {
    case 0: if (len) d[(r >> 8) % len] ^= (unsigned char)(1u << ((r >> 40) % 8)); break;
    case 1: if (len) d[(r >> 8) % len] = (unsigned char)(r >> 48); break;
    case 2: if (len) d[(r >> 8) % len] = bytes[(r >> 40) % sizeof(bytes)]; break;
    case 3:
      if (len >= 4) {
        size_t at = (size_t)((r >> 8) % (len - 3));
        uint32_t w = (r >> 60) % 4 == 0 ? (uint32_t)(len - at) + (uint32_t)((r >> 40) % 5) - 2
                                         : words[(r >> 40) % (sizeof(words) / sizeof(words[0]))];
        d[at] = (unsigned char)w; d[at + 1] = (unsigned char)(w >> 8);
        d[at + 2] = (unsigned char)(w >> 16); d[at + 3] = (unsigned char)(w >> 24);
      }
      break;
    case 4: if (len) len = (size_t)((r >> 8) % len); break;
    case 5: {
      size_t k = 1 + (size_t)((r >> 8) % 8), i;
      if (len + k <= cap) for (i = 0; i < k; i++) d[len++] = (unsigned char)rnd(rs);
      break;
    }
    case 6:
      if (len) {
        size_t at = (size_t)((r >> 8) % len), k = 1 + (size_t)((r >> 40) % 8);
        if (k > len - at) k = len - at;
        memmove(d + at, d + at + k, len - at - k);
        len -= k;
      }
      break;
    default:
      if (len) {
        size_t at = (size_t)((r >> 8) % len), k = 1 + (size_t)((r >> 40) % 16);
        if (k > len - at) k = len - at;
        if (len + k <= cap) {
          memmove(d + at + k, d + at, len - at);
          memcpy(d + at, d + at + k, k);
          len += k;
        }
      }
      break;
    }
  }
  return len;
}

typedef struct names_ctx { const refname *names; int n, missing; } names_ctx;

/* Every API name of a loaded spec is a table in _G. */
static int names_body(lua_State *L)
{
  names_ctx *c = (names_ctx *)lua_touserdata(L, 1);
  int i;
  for (i = 0; i < c->n; i++) {
    lua_pushlstring(L, (const char *)c->names[i].s, c->names[i].len);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_type(L, -1) != LUA_TTABLE) c->missing++;
    lua_pop(L, 1);
  }
  return 0;
}

static void test_spec_fuzz(int cases)
{
  uint64_t rs = UINT64_C(0x5eed0001);
  size_t cap = g_spec.len + 512;
  unsigned char *buf = (unsigned char *)xmalloc(cap);
  refname *names = (refname *)xmalloc(sizeof(refname) * 1024);
  long loaded = 0, corrupt = 0, other = 0, disagree = 0, badnames = 0;
  char first[900] = "", b[600];
  double t0 = now_s();
  int i;
  for (i = 0; i < cases; i++) {
    size_t len, mlen;
    uint32_t nfn;
    int nn, valid, st;
    const unsigned char *msg;
    cm_machine *m;
    memcpy(buf, g_spec.data, g_spec.len);
    len = mutate(&rs, buf, g_spec.len, cap);
    valid = ref_spec(buf, len, &nfn, names, 1024, &nn);
    m = cm_new(LIMIT, KEY);
    st = cm_load(m, buf, len, (const unsigned char *)"return", 6);
    msg = cm_message(m, &mlen, NULL);
    if (st == CM_ST_OK) {
      names_ctx c;
      loaded++;
      if (!valid) disagree++;
      c.names = names; c.n = nn; c.missing = 0;
      if (lua_cpcall((lua_State *)cm_test_state(m), names_body, &c) != 0 || c.missing) badnames++;
    } else if (st == CM_ST_ERROR && mlen == 39 && memcmp(msg, "CCLuaJIT: corrupt API table description", 39) == 0) {
      corrupt++;
      if (valid) disagree++;
    } else {
      other++;
      if (!first[0]) snprintf(first, sizeof(first), "case %d: status %d \"%s\"", i, st, esc(msg, mlen, b, sizeof(b)));
    }
    if (cm_free(m) != CM_ST_OK) other++;
  }
  check(other == 0 && disagree == 0 && badnames == 0 && loaded > 0 && corrupt > 0, "3 spec fuzz",
        "%d seeded mutations: %ld loaded, %ld corrupt, %ld other, %ld disagree with the reference checker, "
        "%ld loaded without all API tables (%.1f s)%s%s", cases, loaded, corrupt, other, disagree, badnames,
        now_s() - t0, first[0] ? "; first other: " : "", first);
  free(buf);
  free(names);
}

/* -- 4. Resume ------------------------------------------------------------------------ */

static void test_resume(void)
{
  host h;
  cm_machine *m;
  outcome o;
  char b[600];
  int st1, st2, st3, top1, top2, top3;

  host_init(&h);
  m = cm_new(LIMIT, KEY);
  h.m = m;
  out_init(&o);
  o.load_st = load_z(m, "term.write('a')\ncoroutine.yield()\nterm.write('b')\ncoroutine.yield(1, 2, 3)\n"
                        "error('after', 0)");
  acct_note(m, "after cm_load");
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  acct_note(m, "after the first YIELD");
  top1 = cm_test_cotop_resume(m);
  check(o.load_st == CM_ST_OK && st1 == CM_ST_YIELD && o.len == 0 && o.tb == SIZE_MAX && strcmp(h.log, "a") == 0 &&
        top1 == 1, "4 resume: YIELD", "load %d, resume %d, log \"%s\", lua_gettop(co) before it %d (want 1: the bios)",
        o.load_st, st1, h.log, top1);
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  top2 = cm_test_cotop_resume(m);
  acct_note(m, "after the second YIELD");
  st3 = cm_resume(m, host_call, &h, NULL, 0);
  top3 = cm_test_cotop_resume(m);
  out_msg(&o, m);
  acct_note(m, "after ERROR");
  check(st2 == CM_ST_YIELD && st3 == CM_ST_ERROR && head_is(&o, "after") && strcmp(h.log, "ab") == 0,
        "4 resume: YIELD with values, then ERROR", "%d, %d \"%s\", log \"%s\"", st2, st3, show(&o, b, sizeof(b)),
        h.log);
  /* S2 relies on it for the size bound of an event (M1.md section 3.7 resume_body step 3, F19, D19). */
  check(top2 == 0 && top3 == 0, "4 resume: the bios coroutine's stack is empty after a YIELD",
        "lua_gettop(co) when the next resume starts: %d after yield(), %d after yield(1, 2, 3)", top2, top3);
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st1 == CM_ST_INTERNAL && msg_is(&o, "machine is dead") && o.tb == SIZE_MAX,
        "4 resume: resume after ERROR is INTERNAL", "%d \"%s\"", st1, show(&o, b, sizeof(b)));
  check(cm_free(m) == CM_ST_OK, "4 resume: freed after ERROR", NULL);

  m = cm_new(LIMIT, KEY);
  h.m = m;
  o.load_st = load_z(m, "term.write('x')");
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st1 == CM_ST_RETURNED && o.len == 0, "4 resume: RETURNED", "%d \"%s\"", st1, show(&o, b, sizeof(b)));
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st1 == CM_ST_INTERNAL && msg_is(&o, "machine is dead"), "4 resume: resume after RETURNED is INTERNAL",
        "%d \"%s\"", st1, show(&o, b, sizeof(b)));
  cm_free(m);

  m = cm_new(LIMIT, KEY);
  h.m = m;
  o.load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  st1 = cm_resume(m, host_call, &h, (const unsigned char *)"x", 1);
  out_msg(&o, m);
  check(st1 == CM_ST_INTERNAL && msg_is(&o, "events are not implemented yet (S2)") && o.tb == SIZE_MAX,
        "4 resume: a non-NULL event is INTERNAL", "%d \"%s\"", st1, show(&o, b, sizeof(b)));
  st1 = cm_resume(m, host_call, &h, (const unsigned char *)"", 0);
  out_msg(&o, m);
  check(st1 == CM_ST_INTERNAL && msg_is(&o, "events are not implemented yet (S2)"),
        "4 resume: a non-NULL empty event is INTERNAL", "%d \"%s\"", st1, show(&o, b, sizeof(b)));
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st1 == CM_ST_RETURNED, "4 resume: the machine was not touched by the refused event", "%d", st1);
  check(o.len == 0 && o.tb == SIZE_MAX, "4 resume: RETURNED after an INTERNAL message leaves the message empty",
        "\"%s\", msg_tb %ld", show(&o, b, sizeof(b)), o.tb == SIZE_MAX ? -1L : (long)o.tb);
  cm_free(m);

  /* S2 turns the message after a YIELD into the event filter: a text left over from an earlier call would be a
  ** wrong filter.
  */
  m = cm_new(LIMIT, KEY);
  h.m = m;
  o.load_st = load_z(m, "coroutine.yield()\ncoroutine.yield()\nerror('x', 0)");
  st1 = cm_resume(m, host_call, &h, (const unsigned char *)"x", 1);
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(o.load_st == CM_ST_OK && st1 == CM_ST_INTERNAL && st2 == CM_ST_YIELD && o.len == 0 && o.tb == SIZE_MAX,
        "4 resume: YIELD after an INTERNAL message leaves the message empty", "%d, %d, %d \"%s\"", o.load_st, st1,
        st2, show(&o, b, sizeof(b)));
  st1 = cm_resume(m, host_call, &h, (const unsigned char *)"x", 1);
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st1 == CM_ST_INTERNAL && st2 == CM_ST_YIELD && o.len == 0 && o.tb == SIZE_MAX,
        "4 resume: the same after a YIELD", "%d, %d \"%s\"", st1, st2, show(&o, b, sizeof(b)));
  cm_free(m);

  m = cm_new(LIMIT, KEY);
  h.m = m;
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  check(st1 == CM_ST_ERROR && msg_is(&o, "bios coroutine missing") && st2 == CM_ST_INTERNAL,
        "4 resume: resume without a load", "%d \"%s\", then %d", st1, show(&o, b, sizeof(b)), st2);
  cm_free(m);

  h.loglen = 0; h.log[0] = '\0';
  m = cm_new(LIMIT, KEY);
  h.m = m;
  o.load_st = load_z(m, "local co = coroutine.wrap(function() term.write('in') coroutine.yield() term.write('2') end)\n"
                        "co()\ncoroutine.yield()\nco()\nerror('x', 0)");
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(o.load_st == CM_ST_OK && st1 == CM_ST_YIELD && st2 == CM_ST_ERROR && head_is(&o, "x") &&
        strcmp(h.log, "in2") == 0,
        "4 resume: host calls and yields of a nested coroutine", "%d %d %d \"%s\" log \"%s\"", o.load_st, st1, st2,
        show(&o, b, sizeof(b)), h.log);
  cm_free(m);
  out_free(&o);
  host_done(&h);
}

/* -- 5. render_error and append_traceback -------------------------------------------- */

/* The value before the traceback matches "<type>: " and 8 lowercase hex digits. */
static int is_hashed(const outcome *o, const char *type)
{
  size_t tl = strlen(type), i, n = head_len(o);
  if (n != tl + 10 || memcmp(o->msg, type, tl) != 0 || o->msg[tl] != ':' || o->msg[tl + 1] != ' ') return 0;
  for (i = tl + 2; i < n; i++)
    if (!((o->msg[i] >= '0' && o->msg[i] <= '9') || (o->msg[i] >= 'a' && o->msg[i] <= 'f'))) return 0;
  return 1;
}

/* The keyed hash of render_error, written again here: MurmurHash3's fmix64 of (pointer XOR id_key), folded to 32
** bits by XOR of its halves. It pins machine.c's choice, so a hash without the key, or the plain address, fails.
*/
static uint32_t t_mix32(uint64_t x)
{
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

/* lua_topointer and the type of the global named c->name. */
typedef struct ptr_ctx { const char *name; const void *p; int type; } ptr_ctx;
static int ptr_body(lua_State *L)
{
  ptr_ctx *c = (ptr_ctx *)lua_touserdata(L, 1);
  lua_getfield(L, LUA_GLOBALSINDEX, c->name);
  c->type = lua_type(L, -1);
  c->p = lua_topointer(L, -1);
  return 0;
}

/* unref(): removes every thread from the registry. From S4 the bios can do this to the reference of its own
** coroutine through debug.getregistry (M1.md section 6); this C function stands in for it.
*/
static int t_unref(lua_State *L)
{
  lua_pushnil(L);
  while (lua_next(L, LUA_REGISTRYINDEX)) {
    if (lua_type(L, -1) == LUA_TTHREAD) {
      lua_pop(L, 1);
      lua_pushvalue(L, -1);
      lua_pushnil(L);
      lua_rawset(L, LUA_REGISTRYINDEX);
    } else {
      lua_pop(L, 1);
    }
  }
  return 0;
}
static int unref_setup_body(lua_State *L)
{
  lua_pushcfunction(L, t_unref);
  lua_setfield(L, LUA_GLOBALSINDEX, "unref");
  return 0;
}

/* *ud = the type of _G.w[1] (-1 if w is not a table). Makes no allocation that could run a GC step. */
static int weak_body(lua_State *L)
{
  int *type = (int *)lua_touserdata(L, 1);
  *type = -1;
  lua_getfield(L, LUA_GLOBALSINDEX, "w");
  if (lua_type(L, -1) == LUA_TTABLE) {
    lua_rawgeti(L, -1, 1);
    *type = lua_type(L, -1);
  }
  return 0;
}

static void tb_frames(const char *name, host *h, const char *bios, int want_lines, int want_dots)
{
  outcome o;
  char b[1200];
  int dots, n;
  run(h, bios, &o);
  n = tb_lines(&o, &dots);
  check(o.st == CM_ST_ERROR && n == want_lines && dots == want_dots && tb_has(&o, "\n\tbios.lua:") &&
        o.len >= 15 && memcmp(o.msg + o.len - 15, ": in main chunk", 15) == 0 &&
        !tb_has(&o, "0x") && !tb_has(&o, "[builtin"), name,
        "status %d, %d lines (want %d), \"...\" at %d (want %d): \"%s\"",
        o.st, n, want_lines, dots, want_dots, show(&o, b, sizeof(b)));
  out_free(&o);
}

static void test_errors(void)
{
  host h;
  outcome o;
  char b[1600];
  int i;

  host_init(&h);
  run(&h, G1_BIOS, &o);
  check(o.st == CM_ST_ERROR && msg_is(&o, G1_MSG) && o.tb == 16 && strcmp(h.log, "hi") == 0,
        "5 traceback: G1 bios, exact text and msg_tb 16", "status %d, msg_tb %ld, log \"%s\", \"%s\"", o.st,
        o.tb == SIZE_MAX ? -1L : (long)o.tb, h.log, show(&o, b, sizeof(b)));
  out_free(&o);

  /* render_error, one type at a time. */
  expect("5 render: nil", &h, "error(nil)", CM_ST_ERROR, "nil");
  expect("5 render: error() is nil", &h, "error()", CM_ST_ERROR, "nil");
  expect("5 render: true", &h, "error(true)", CM_ST_ERROR, "true");
  expect("5 render: false", &h, "error(false)", CM_ST_ERROR, "false");
  expect("5 render: a number at level 0", &h, "error(42, 0)", CM_ST_ERROR, "42");
  expect("5 render: a fraction", &h, "error(1.5, 0)", CM_ST_ERROR, "1.5");
  expect("5 render: error(42) (LuaJIT adds a position, F6)", &h, "error(42)", CM_ST_ERROR, "bios.lua:1: 42");
  run(&h, "error('a\\0b\\255', 0)", &o);
  check(o.st == CM_ST_ERROR && head_len(&o) == 4 && memcmp(o.msg, "a\0b\377", 4) == 0, "5 render: NUL and 0xFF bytes",
        "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "error({})", &o);
  check(o.st == CM_ST_ERROR && is_hashed(&o, "table"), "5 render: a table is \"table: %08x\"", "\"%s\"",
        show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "error(function() end)", &o);
  check(o.st == CM_ST_ERROR && is_hashed(&o, "function"), "5 render: a function", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "error(coroutine.create(function() end))", &o);
  check(o.st == CM_ST_ERROR && is_hashed(&o, "thread"), "5 render: a thread", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  h.marks = 0;
  run(&h, "error(setmetatable({}, {__tostring = function() t.mark() return 'x' end,\n"
          "  __index = function() t.mark() end, __concat = function() t.mark() end}))", &o);
  check(o.st == CM_ST_ERROR && is_hashed(&o, "table") && h.marks == 0, "5 render: __tostring is never run",
        "marks %d, \"%s\"", h.marks, show(&o, b, sizeof(b)));
  out_free(&o);
  {
    /* The text is exactly the keyed hash of the table's address: never the address itself, and different under
    ** another key (M1.md section 3.7 render_error, I7).
    */
    static const uint64_t hkeys[2] = {1, KEY};
    for (i = 0; i < 2; i++) {
      cm_machine *m = cm_new(LIMIT, hkeys[i]);
      ptr_ctx pc;
      char want[48], addr[48], other[48], name[96];
      int cp;
      out_init(&o);
      h.m = m;
      load_z(m, "x = {}\nerror(x)");
      o.st = cm_resume(m, host_call, &h, NULL, 0);
      out_msg(&o, m);
      pc.name = "x"; pc.p = NULL; pc.type = -1;
      cp = lua_cpcall((lua_State *)cm_test_state(m), ptr_body, &pc);
      snprintf(want, sizeof(want), "table: %08x", (unsigned)t_mix32((uint64_t)(uintptr_t)pc.p ^ hkeys[i]));
      snprintf(addr, sizeof(addr), "table: %08x", (unsigned)(uint32_t)(uintptr_t)pc.p);
      snprintf(other, sizeof(other), "table: %08x", (unsigned)t_mix32((uint64_t)(uintptr_t)pc.p ^ hkeys[1 - i]));
      snprintf(name, sizeof(name), "5 render: a table is the hash of its address under key %d", i + 1);
      check(o.st == CM_ST_ERROR && cp == 0 && pc.type == LUA_TTABLE && head_is(&o, want) && !head_is(&o, addr) &&
            strcmp(want, other) != 0, name, "\"%s\", want \"%s\" (address \"%s\", other key \"%s\")",
            show(&o, b, sizeof(b)), want, addr, other);
      cm_free(m);
      out_free(&o);
    }
  }

  /* Frame wording. */
  run(&h, "function g() error('deep2') end\nlocal function l() g() end\nl()", &o);
  check(o.st == CM_ST_ERROR && msg_is(&o, "bios.lua:1: deep2\nstack traceback:\n\t[C]: in function 'error'\n"
                                          "\tbios.lua:1: in function 'g'\n\tbios.lua:2: in local 'l'\n"
                                          "\tbios.lua:3: in main chunk"),
        "5 traceback: global and local function frames", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local t2 = {}\nfunction t2.m() error('m', 0) end\nlocal f = function() t2.m() end\nf()", &o);
  check(o.st == CM_ST_ERROR && head_is(&o, "m") && tb_has(&o, "\n\tbios.lua:2: in field 'm'") &&
        tb_has(&o, "\n\tbios.lua:3: in local 'f'") && tb_has(&o, "\n\tbios.lua:4: in main chunk"),
        "5 traceback: field frames", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local t2 = {}\nfunction t2.m() error('m', 0) end\nlocal f = function() t2.m() end\n"
          "local co = coroutine.wrap(function() f() end)\nco()", &o);
  /* LuaJIT's coroutine.wrap adds the position of its caller to a string error when it re-raises it. */
  check(o.st == CM_ST_ERROR &&
        msg_is(&o, "bios.lua:5: m\nstack traceback:\n\t[C]: in local 'co'\n\tbios.lua:5: in main chunk"),
        "5 traceback: an error re-raised by coroutine.wrap shows the outer frames", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local t2 = {}\nfunction t2:m() error('m', 0) end\nt2:m()", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\n\tbios.lua:2: in method 'm'"), "5 traceback: method frames",
        "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local up = function() error('u', 0) end\nlocal function f() up() end\nf()", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\n\tbios.lua:1: in upvalue 'up'") &&
        tb_has(&o, "\n\tbios.lua:2: in local 'f'"), "5 traceback: upvalue frames", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local t2 = {function() error('anon', 0) end}\nt2[1]()", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\n\tbios.lua:1: in function <bios.lua:1>"),
        "5 traceback: an unnamed Lua function", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "for _, v in pairs({1}) do\n  for k in (function() error('it', 0) end) do end\nend", &o);
  check(o.st == CM_ST_ERROR && head_is(&o, "it") && !tb_has(&o, "(for") && !tb_has(&o, "'('"),
        "5 traceback: a \"(for ...)\" local is no name", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local t = {t.f}; t[1]()", &o);
  check(o.st == CM_ST_ERROR &&
        msg_is(&o, "bios.lua:1: f failed\nstack traceback:\n\t[C]: in ?\n\tbios.lua:1: in main chunk"),
        "5 traceback: an unnamed C closure is \"[C]: in ?\"", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "term.write({})", &o);
  check(o.st == CM_ST_ERROR && msg_is(&o, "bios.lua:1: CCLuaJIT: tables are not supported yet\nstack traceback:\n"
                                          "\t[C]: in field 'write'\n\tbios.lua:1: in main chunk"),
        "5 traceback: a host function frame", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local rm = table.remove\nrm(nil)", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\n\t[C]: in local 'rm'\n\tbios.lua:2: in main chunk") &&
        !tb_has(&o, "[builtin") && !tb_has(&o, "0x"), "5 traceback: a builtin through a local is \"[C]: in local\"",
        "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "({table.remove})[1](nil)", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\nstack traceback:\n\t[C]: in ?\n\tbios.lua:1: in main chunk") &&
        !tb_has(&o, "[builtin") && !tb_has(&o, "0x"), "5 traceback: an unnamed builtin is \"[C]: in ?\"",
        "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  run(&h, "local len = string.len\nlen(nil)", &o);
  check(o.st == CM_ST_ERROR && tb_has(&o, "\n\t[C]: in local 'len'") && !tb_has(&o, "[builtin"),
        "5 traceback: string.len through a local", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  {
    /* A function name is cut to 100 bytes (a 300-byte global name); a chunk name to LuaJIT's 59. */
    cm_buf src = {0};
    char n300[301], want[120], n101[102];
    memset(n300, 'n', 300); n300[300] = '\0';
    memset(n101, 'n', 101); n101[101] = '\0';
    snprintf(want, sizeof(want), "in function '%.100s'", n300);
    cb_cat(&src, "local f = load('error(\"long\", 0)', '=' .. string.rep('n', 300))\n");
    cb_cat(&src, n300);
    cb_cat(&src, " = function() f() end\n");
    cb_cat(&src, n300);
    cb_cat(&src, "()");
    run_len(&h, LIMIT, (const char *)src.data, src.len, &o);
    check(o.st == CM_ST_ERROR && head_is(&o, "long") && tb_has(&o, want) && !msg_has(&o, n101),
          "5 traceback: names cut to 100 bytes", "\"%s\"", show(&o, b, sizeof(b)));
    cb_free(&src);
  }
  out_free(&o);

  /* Elision: 13, 22 and 23 frames print every frame; 24 and 303 frames print 12, "...", 10. The recursion has
  ** k + 1 frames of f, plus error and the main chunk.
  */
  tb_frames("5 traceback: 13 frames, all printed", &h,
            "local function f(n) if n == 0 then error('deep') end f(n - 1) end\nf(10)", 13, 0);
  tb_frames("5 traceback: 22 frames, all printed", &h,
            "local function f(n) if n == 0 then error('deep') end f(n - 1) end\nf(19)", 22, 0);
  tb_frames("5 traceback: 23 frames, all printed", &h,
            "local function f(n) if n == 0 then error('deep') end f(n - 1) end\nf(20)", 23, 0);
  tb_frames("5 traceback: 24 frames give 12, \"...\", 10", &h,
            "local function f(n) if n == 0 then error('deep') end f(n - 1) end\nf(21)", 23, 13);
  tb_frames("5 traceback: a 300-deep recursion gives 12, \"...\", 10", &h,
            "local function f(n) if n == 0 then error('deep') end f(n - 1) end\nf(300)", 23, 13);
  tb_frames("5 traceback: an error 30,000 calls deep", &h,
            "local n = 30000\nlocal function f() if n == 0 then error('deep') end n = n - 1 f() end\nf()", 23, 13);
  tb_frames("5 traceback: a stack overflow", &h, "local function f() f() end\nf()", 23, 13);
  run(&h, "local function f() f() end\nf()", &o);
  /* The top frame raised the overflow and has no line: "<line>:" is left out (M1.md section 3.7). */
  check(o.st == CM_ST_ERROR && msg_has(&o, "stack overflow") &&
        tb_has(&o, "\nstack traceback:\n\tbios.lua: in upvalue 'f'\n\tbios.lua:1: in upvalue 'f'\n"),
        "5 traceback: the overflow message, and a frame without a line", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  /* A frame of 100 or more slots: after the overflow LuaJIT leaves a dummy frame on top, which level -1 would
  ** match, so the frame count must come from level -10 (M1.md section 3.7).
  */
  {
    static const int nlocals[2] = {100, 190};
    for (i = 0; i < 2; i++) {
      cm_buf src = {0};
      char name[120], v[16];
      int k;
      cb_cat(&src, "local function f()\n  local a0");
      for (k = 1; k < nlocals[i]; k++) { snprintf(v, sizeof(v), ", a%d", k); cb_cat(&src, v); }
      cb_cat(&src, " = 1");
      for (k = 1; k < nlocals[i]; k++) cb_cat(&src, ", 1");
      cb_cat(&src, "\n  f()\nend\nf()");
      cb_u8(&src, 0);
      snprintf(name, sizeof(name), "5 traceback: a stack overflow with %d locals per frame gives 12, \"...\", 10",
               nlocals[i]);
      tb_frames(name, &h, (const char *)src.data, 23, 13);
      cb_free(&src);
    }
  }

  /* An error under an exhausted cap still gets its traceback. */
  {
    double used = 0;
    size_t lim = (size_t)4 << 20;
    cm_machine *m = cm_new(lim, KEY);
    out_init(&o);
    h.m = m;
    load_z(m, "local head\nlocal function fill()\n  for i = 1, 1e9 do head = {head} end\nend\nfill()");
    o.st = cm_resume(m, host_call, &h, NULL, 0);
    out_msg(&o, m);
    used = (double)cm_test_used(m);
    note_cotop(&o, m);
    check(o.st == CM_ST_ERROR && head_is(&o, "not enough memory") &&
          tb_has(&o, "\n\tbios.lua: in local 'fill'\n\tbios.lua:5: in main chunk") &&
          used > (double)lim - 4096, "5 traceback: an error under an exhausted cap",
          "status %d, used %.0f of %lu, \"%s\"", o.st, used, (unsigned long)lim, show(&o, b, sizeof(b)));
    cm_free(m);
    out_free(&o);
  }

  /* A number error value when no memory is left: its conversion to text fails, and the error still gets its
  ** traceback (D7); the text is then "not enough memory". A string needs no conversion and keeps its text.
  ** A full collection first leaves no garbage that a GC step could free; t.capnow() then fixes the cap at the
  ** bytes in use.
  */
  {
    static const char *const cap_bios[2] = {"t.capnow()\nerror(12345.5, 0)", "t.capnow()\nerror('plain', 0)"};
    static const char *const cap_want[2] = {
      "not enough memory\nstack traceback:\n\t[C]: in function 'error'\n\tbios.lua:2: in main chunk",
      "plain\nstack traceback:\n\t[C]: in function 'error'\n\tbios.lua:2: in main chunk"
    };
    for (i = 0; i < 2; i++) {
      cm_machine *m = cm_new(LIMIT, KEY);
      out_init(&o);
      h.m = m;
      o.load_st = load_z(m, cap_bios[i]);
      lua_gc((lua_State *)cm_test_state(m), LUA_GCCOLLECT, 0);
      o.st = cm_resume(m, host_call, &h, NULL, 0);
      out_msg(&o, m);
      note_cotop(&o, m);
      check(o.load_st == CM_ST_OK && o.st == CM_ST_ERROR && msg_is(&o, cap_want[i]) && o.tb == (i == 0 ? 17u : 5u),
            i == 0 ? "5 render: a number when no memory is left: \"not enough memory\" and the traceback"
                   : "5 render: a string when no memory is left: the string and the traceback",
            "load %d, status %d, \"%s\"", o.load_st, o.st, show(&o, b, sizeof(b)));
      cm_free(m);
      out_free(&o);
    }
  }

  /* The bios coroutine stays referenced from L while resume_body uses it: even when the registry no longer
  ** refers to it (S4: debug.getregistry), a GC cycle in render_error (the number conversion) must not free it.
  ** GC pause 0 and step multiplier 0 make every GC check run a whole cycle. w[1] is a weak reference to co.
  */
  {
    cm_machine *m = cm_new(LIMIT, KEY);
    lua_State *L;
    int cp, wtype = -2;
    out_init(&o);
    h.m = m;
    o.load_st = load_z(m, "w = setmetatable({(coroutine.running())}, {__mode = 'v'})\nunref()\nerror(12345.5, 0)");
    L = (lua_State *)cm_test_state(m);
    cp = lua_cpcall(L, unref_setup_body, NULL);
    lua_gc(L, LUA_GCSETPAUSE, 0);
    lua_gc(L, LUA_GCSETSTEPMUL, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    o.st = cm_resume(m, host_call, &h, NULL, 0);
    out_msg(&o, m);
    if (cp == 0) cp = lua_cpcall(L, weak_body, &wtype);
    check(o.load_st == CM_ST_OK && cp == 0 && o.st == CM_ST_ERROR && wtype == LUA_TTHREAD &&
          msg_is(&o, "12345.5\nstack traceback:\n\t[C]: in function 'error'\n\tbios.lua:3: in main chunk"),
          "5 traceback: the bios coroutine stays alive without its registry reference",
          "load %d, setup %d, status %d, w[1] is %s, \"%s\"", o.load_st, cp, o.st,
          wtype == LUA_TTHREAD ? "the coroutine" : wtype == LUA_TNIL ? "nil (collected)" : "?",
          show(&o, b, sizeof(b)));
    cm_free(m);
    out_free(&o);
  }

  /* A value that fills msg keeps all its bytes; the traceback is cut or left out, never the value. */
  {
    static const struct { size_t n; size_t want_len; int want_tb; } cases[] = {
      {65536, 65536, 0}, {70000, 65536, 0}, {65536 - 10, 65536 - 10, 0}, {65536 - 17, 65536, 1},
      {65536 - 30, 65536 - 13, 1}, {65536 - 60, 0, 1}
    };
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
      char src[96], name[120];
      size_t k, n = cases[i].n, hl;
      int allx;
      snprintf(src, sizeof(src), "error(string.rep('x', %lu), 0)", (unsigned long)n);
      run(&h, src, &o);
      hl = head_len(&o);
      allx = hl == (n < 65536 ? n : 65536);
      for (k = 0; allx && k < hl; k++) allx = o.msg[k] == 'x';
      snprintf(name, sizeof(name), "5 traceback: a %lu-byte value", (unsigned long)n);
      check(o.st == CM_ST_ERROR && allx && (cases[i].want_tb ? o.tb == n : o.tb == SIZE_MAX) &&
            (cases[i].want_len == 0 || o.len == cases[i].want_len) && o.len <= CM_MSG_MAX &&
            (cases[i].want_len != 0 || tb_has(&o, "\n\t[C]: in function 'error'")), name,
            "status %d, len %lu, msg_tb %ld", o.st, (unsigned long)o.len, o.tb == SIZE_MAX ? -1L : (long)o.tb);
      out_free(&o);
    }
  }
  run(&h, "error('a\\nstack traceback:\\n\\tfake', 0)", &o);
  check(o.st == CM_ST_ERROR && o.tb == 24 && head_is(&o, "a\nstack traceback:\n\tfake"),
        "5 traceback: msg_tb points at the real header", "msg_tb %ld \"%s\"", o.tb == SIZE_MAX ? -1L : (long)o.tb,
        show(&o, b, sizeof(b)));
  out_free(&o);
  check(g_cotop_runs > 20 && g_cotop_errors == 0, "5 traceback: lua_gettop(co) is the same before and after",
        "%ld of %ld runs changed it", g_cotop_errors, g_cotop_runs);
  check(g_tb_leaks == 0, "5 traceback: no traceback so far contains \"0x\" or \"[builtin\"", "%ld of %ld do",
        g_tb_leaks, g_cotop_runs);
  host_done(&h);
}

/* -- 6. Host round trips --------------------------------------------------------------- */

static void raw_case(const char *name, host *h, const void *raw, size_t rawlen, const char *bios, int st,
                     const char *head)
{
  h->raw = (const unsigned char *)raw;
  h->rawlen = rawlen;
  expect(name, h, bios, st, head);
  h->raw = NULL;
  h->rawlen = 0;
}

static void test_host(void)
{
  host h;
  outcome o;
  char b[600];
  cm_buf r = {0};
  int i;
  static const char *const texts[] = {
    "CCLuaJIT: corrupt host reply", "CCLuaJIT: JNI failure in host call", "CCLuaJIT: Java exception in host call",
    "CCLuaJIT: host call returned null", "CCLuaJIT: host reply too large", "CCLuaJIT: not enough memory for host reply"
  };

  host_init(&h);
  run(&h, "term.write(\"hi\")\nerror('w', 0)", &o);
  check(h.req_len == 11 && memcmp(h.req, "\001\000\000\000\004\002\000\000\000hi", 11) == 0,
        "6 host: request bytes of term.write(\"hi\")", "%s", esc(h.req, h.req_len < 64 ? h.req_len : 64, b, sizeof(b)));
  out_free(&o);
  run(&h, "local s = t.echo('\\0\\255a\\0')\nerror(s, 0)", &o);
  check(o.st == CM_ST_ERROR && head_len(&o) == 4 && memcmp(o.msg, "\0\377a\0", 4) == 0,
        "6 host: NUL and 0xFF round trip",
        "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  expect("6 host: every value type round trips", &h,
         "local a, b, c, d, e, f, g, h, i = t.echo(nil, true, false, 1.5, 'x', -0.0, 1/0, 0/0, 2^53)\n"
         "local ok = a == nil and b == true and c == false and d == 1.5 and e == 'x' and 1/f == -1/0 and g == 1/0\n"
         "  and h ~= h and i == 2^53\n"
         "error(tostring(ok) .. ' ' .. select('#', t.echo(nil, nil, nil)) .. ' ' .. select('#', t.echo()), 0)",
         CM_ST_ERROR, "true 3 0");
  expect("6 host: functions and threads are sent as nil", &h,
         "error(t.argc(function() end, coroutine.create(function() end), nil, 1, 's', true, false), 0)",
         CM_ST_ERROR, "0 0 0 3 4 2 1");
  expect("6 host: a table argument raises", &h, "t.argc(1, {})", CM_ST_ERROR,
         "bios.lua:1: CCLuaJIT: tables are not supported yet");
  run(&h, "local ok, e = pcall(t.argc, {})\nerror(tostring(ok) .. '|' .. e, 0)", &o);
  check(o.st == CM_ST_ERROR && msg_has(&o, "false|") && msg_has(&o, "CCLuaJIT: tables are not supported yet"),
        "6 host: a table argument under pcall", "\"%s\"", show(&o, b, sizeof(b)));
  out_free(&o);
  expect("6 host: level 0", &h, "t.err(0, 'lvl0')", CM_ST_ERROR, "lvl0");
  expect("6 host: level 1", &h, "t.err(1, 'lvl1')", CM_ST_ERROR, "bios.lua:1: lvl1");
  expect("6 host: level 2", &h, "local function f()\n  t.err(2, 'lvl2')\nend\nf()", CM_ST_ERROR, "bios.lua:4: lvl2");
  expect("6 host: level 3", &h, "local function g() t.err(3, 'lvl3') end\nlocal function f() g() end\nf()",
         CM_ST_ERROR, "bios.lua:3: lvl3");
  expect("6 host: level -1", &h, "t.err(-1, 'lvl-1')", CM_ST_ERROR, "lvl-1");
  expect("6 host: level 1000", &h, "t.err(1000, 'far')", CM_ST_ERROR, "far");
  expect("6 host: a nil message", &h, "t.err(1)", CM_ST_ERROR, "nil");
  expect("6 host: a nil message under pcall", &h,
         "local ok, e = pcall(t.err, 1)\nerror(tostring(ok) .. ' ' .. type(e), 0)",
         CM_ST_ERROR, "false nil");
  /* luaL_where(L, 1) under pcall: level 1 is pcall's frame, which has no position (F5, Tier 2 scenario 7). */
  expect("6 host: level 1 under pcall has no position (F5)", &h, "local ok, e = pcall(t.err, 1, 'p1')\nerror(e, 0)",
         CM_ST_ERROR, "p1");
  expect("6 host: level 2 under pcall is the caller of pcall", &h, "local ok, e = pcall(t.err, 2, 'p2')\nerror(e, 0)",
         CM_ST_ERROR, "bios.lua:1: p2");
  expect("6 host: a host error is catchable", &h,
         "local ok, e = pcall(t.err, 0, 'caught')\nerror(tostring(ok) .. ' ' .. e .. ' ' .. t.echo('alive'), 0)",
         CM_ST_ERROR, "false caught alive");
  for (i = 0; i <= 6; i++) {
    char src[64], name[96];
    int code = i == 6 ? 99 : i;
    snprintf(src, sizeof(src), "t.code(%d)", code);
    snprintf(name, sizeof(name), "6 host: host status %d", code);
    expect(name, &h, src, CM_ST_ERROR, i == 6 ? "CCLuaJIT: host call failed" : texts[i]);
  }
  expect("6 host: a host status error is level 0 under pcall", &h,
         "local ok, e = pcall(t.code, 2)\nerror(e, 0)", CM_ST_ERROR, "CCLuaJIT: Java exception in host call");
  expect("6 host: function indices across APIs and aliases", &h,
         "error(alias1.which() .. ' ' .. alias2.which() .. ' ' .. alias1.which2(), 0)", CM_ST_ERROR, "14 14 15");
  expect("6 host: large numbers round trip", &h,
         "local a, b, c, d = t.echo(2^53 + 2, -2^63, 1e308, 5e-324)\n"
         "error(tostring(a == 2^53 + 2 and b == -2^63 and c == 1e308 and d == 5e-324), 0)", CM_ST_ERROR, "true");
  expect("6 host: a 1 MiB string round trips", &h,
         "local s = string.rep('ab\\0', 349526)\nlocal r = t.echo(s, s)\nerror(tostring(r == s) .. ' ' .. #r, 0)",
         CM_ST_ERROR, "true 1048578");

  /* Raw replies: try to break the decoder. */
  raw_case("6 host: reply count 0xFFFFFFFF", &h, "\000\377\377\377\377", 5, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: a string length past the end", &h, "\000\001\000\000\000\004\010\000\000\000abc", 13, "t.raw()",
           CM_ST_ERROR, "CCLuaJIT: corrupt host reply");
  raw_case("6 host: a string length of 0xFFFFFFFF", &h, "\000\001\000\000\000\004\377\377\377\377abc", 13, "t.raw()",
           CM_ST_ERROR, "CCLuaJIT: corrupt host reply");
  raw_case("6 host: a trailing byte", &h, "\000\000\000\000\000\000", 6, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: an unknown value tag", &h, "\000\001\000\000\000\005", 6, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: a truncated number", &h, "\000\001\000\000\000\003\000\000", 8, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: reply kind 2 (S5 CALLBACK) is refused", &h, "\002\000\000\000\000", 5, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: an empty reply", &h, "", 0, "t.raw()", CM_ST_ERROR, "CCLuaJIT: corrupt host reply");
  raw_case("6 host: an ERROR with a number message", &h, "\001\000\000\000\000\003\000\000\000\000\000\000\000\000", 14,
           "t.raw()", CM_ST_ERROR, "CCLuaJIT: corrupt host reply");
  raw_case("6 host: an ERROR without a message", &h, "\001\001\000\000\000", 5, "t.raw()", CM_ST_ERROR,
           "CCLuaJIT: corrupt host reply");
  raw_case("6 host: an ERROR with level 1000", &h, "\001\350\003\000\000\004\002\000\000\000hi", 12, "t.raw()",
           CM_ST_ERROR, "hi");
  raw_case("6 host: an ERROR with level INT32_MIN", &h, "\001\000\000\000\200\004\002\000\000\000hi", 12, "t.raw()",
           CM_ST_ERROR, "hi");
  /* LUAI_MAXCSTACK (8,000) slots: a C function with no arguments can return 8,000 values, not 8,001. */
  for (i = 0; i < 3; i++) {
    uint32_t n = i == 0 ? 7000 : i == 1 ? 8000 : 8001, k;
    r.len = 0;
    cb_u8(&r, 0); cb_u32(&r, n);
    for (k = 0; k < n; k++) cb_u8(&r, 0);
    if (i == 0) raw_case("6 host: 7,000 results", &h, r.data, r.len, "error('n=' .. select('#', t.raw()), 0)",
                         CM_ST_ERROR, "n=7000");
    else if (i == 1) raw_case("6 host: 8,000 results", &h, r.data, r.len, "error('n=' .. select('#', t.raw()), 0)",
                              CM_ST_ERROR, "n=8000");
    else raw_case("6 host: 8,001 results are too many", &h, r.data, r.len, "t.raw()", CM_ST_ERROR,
                  "bios.lua:1: too many results to return");
  }
  r.len = 0;
  cb_u8(&r, 0); cb_u32(&r, 7990);
  for (i = 0; i < 7990; i++) cb_u8(&r, 0);
  raw_case("6 host: results that do not fit above the arguments", &h, r.data, r.len,
           "local ok, e = pcall(t.raw, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)\nerror(tostring(ok) .. ' ' .. e, 0)",
           CM_ST_ERROR, "false too many results to return");
  cb_free(&r);
  check(h.bad_req == 0, "6 host: every request was well-formed", "%ld bad of %ld", h.bad_req, h.calls);
  host_done(&h);
}

/* -- 7. Reply fuzz ---------------------------------------------------------------------- */

struct fuzz {
  uint64_t rs;
  long n_random, n_mutated, served;
  int done;
  unsigned char cur[2048];
  size_t curlen;
  refreply exp;
  long got_invalid, got_values, got_error, mismatches;
  char first[900];
};

static size_t gen_random(uint64_t *rs, unsigned char *out)
{
  size_t len = (size_t)(rnd(rs) % 40), i;
  for (i = 0; i < len; i++) out[i] = (unsigned char)rnd(rs);
  if (len > 0) {
    unsigned r = (unsigned)(rnd(rs) % 10);
    if (r < 4) out[0] = 0;
    else if (r < 8) out[0] = 1;
  }
  if (len >= 5 && rnd(rs) % 2) {
    uint32_t c = (uint32_t)(rnd(rs) % 6);
    out[1] = (unsigned char)c; out[2] = out[3] = out[4] = 0;
  }
  for (i = 5; i < len; i++)
    if (rnd(rs) % 3 == 0) out[i] = (unsigned char)(rnd(rs) % 6);
  return len;
}

static void gen_tval(uint64_t *rs, cm_buf *b)
{
  static const uint64_t specials[] = {
    0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff8000000000000),
    UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000),
    UINT64_C(0x7ff8000000000001), UINT64_C(0xfff0000000000001), UINT64_C(0x7fefffffffffffff), 1,
    UINT64_C(0x4340000000000000), UINT64_C(0xfff8000000000000)
  };
  unsigned tag = (unsigned)(rnd(rs) % 5), k;
  cb_u8(b, tag);
  if (tag == 3) {
    uint64_t u = rnd(rs) % 2 ? specials[rnd(rs) % (sizeof(specials) / sizeof(specials[0]))] : rnd(rs);
    double d;
    memcpy(&d, &u, 8);
    cb_f64(b, d);
  } else if (tag == 4) {
    unsigned n = (unsigned)(rnd(rs) % 21);
    cb_u32(b, n);
    for (k = 0; k < n; k++) cb_u8(b, (unsigned)(rnd(rs) % 4 == 0 ? (rnd(rs) % 2 ? 0 : 255) : rnd(rs)));
  }
}

static size_t gen_valid(uint64_t *rs, unsigned char *out, size_t cap)
{
  static const int32_t levels[] = {-5, -1, 0, 1, 2, 3, 4, 1000, INT32_MIN, INT32_MAX};
  cm_buf b = {0};
  size_t len;
  if (rnd(rs) % 10 < 7) {
    uint32_t n = (uint32_t)(rnd(rs) % 10 == 0 ? rnd(rs) % 64 : rnd(rs) % 8), i;
    cb_u8(&b, 0);
    cb_u32(&b, n);
    for (i = 0; i < n; i++) gen_tval(rs, &b);
  } else {
    int32_t level = rnd(rs) % 4 ? levels[rnd(rs) % (sizeof(levels) / sizeof(levels[0]))] : as_i32((uint32_t)rnd(rs));
    uint32_t u;
    memcpy(&u, &level, 4);
    cb_u8(&b, 1);
    cb_u32(&b, u);
    if (rnd(rs) % 4 == 0) {
      cb_u8(&b, 0);
    } else {
      unsigned n = (unsigned)(rnd(rs) % 21), k;
      cb_u8(&b, 4);
      cb_u32(&b, n);
      for (k = 0; k < n; k++) cb_u8(&b, (unsigned)rnd(rs));
    }
  }
  len = b.len < cap ? b.len : cap;
  memcpy(out, b.data, len);
  cb_free(&b);
  return len;
}

static int fuzz_serve(fuzz *fz, cm_buf *rep)
{
  size_t len;
  if (fz->served >= fz->n_random + fz->n_mutated) {
    fz->done = 1;
    reply_none(rep);
    return CM_HOST_OK;
  }
  if (fz->served < fz->n_random) {
    len = gen_random(&fz->rs, fz->cur);
  } else {
    len = gen_valid(&fz->rs, fz->cur, sizeof(fz->cur) - 64);   /* mutate adds at most 16 bytes */
    if (rnd(&fz->rs) % 10 != 0) len = mutate(&fz->rs, fz->cur, len, sizeof(fz->cur));
  }
  fz->served++;
  fz->curlen = len;
  ref_reply(fz->cur, len, &fz->exp);
  cb_mem(rep, fz->cur, len);
  return CM_HOST_OK;
}

/* The error value of an ERROR record with a string under the fuzz bios's "check(pcall(raw))" on line 2: level 2
** is the main chunk, so "bios.lua:2: " comes first; level 1 is pcall's frame and level 3 is past the bottom of the
** coroutine, which give no position; levels 0 and below never do.
*/
static int msg_matches(const tval *got, int32_t level, const tval *s)
{
  if (level != 2) return tv_equal(got, s);
  return got->tag == 4 && got->len == 12 + s->len && memcmp(got->s, "bios.lua:2: ", 12) == 0 &&
         (s->len == 0 || memcmp(got->s + 12, s->s, s->len) == 0);
}

static int fuzz_check(fuzz *fz, host *h, cm_buf *rep)
{
  const tval *a = h->args;
  int n = h->nargs, ok = 0;
  uint32_t i;
  if (fz->done) {
    reply_one(rep, tv_bool(1));
    return CM_HOST_OK;
  }
  switch (fz->exp.kind) {
  case -1:
    fz->got_invalid++;
    ok = n == 2 && a[0].tag == 1 && tv_is_str(&a[1], "CCLuaJIT: corrupt host reply");
    break;
  case 0:
    fz->got_values++;
    ok = n == 1 + (int)fz->exp.n && a[0].tag == 2;
    for (i = 0; ok && i < fz->exp.n; i++) ok = tv_equal(&a[1 + i], &fz->exp.v[i]);
    break;
  default:
    fz->got_error++;
    ok = n == 2 && a[0].tag == 1 &&
         (fz->exp.msg.tag == 0 ? a[1].tag == 0 : msg_matches(&a[1], fz->exp.level, &fz->exp.msg));
    break;
  }
  if (!ok) {
    fz->mismatches++;
    if (!fz->first[0]) {
      char b1[400], b2[400];
      snprintf(fz->first, sizeof(fz->first), "case %ld, reply \"%s\", expected kind %d, got %d values, second \"%s\"",
               fz->served, esc(fz->cur, fz->curlen, b1, sizeof(b1)), fz->exp.kind, n,
               n > 1 && a[1].tag == 4 ? esc(a[1].s, a[1].len, b2, sizeof(b2)) : "-");
    }
  }
  reply_one(rep, tv_bool(0));
  return CM_HOST_OK;
}

static void test_reply_fuzz(long n_random, long n_mutated)
{
  host h;
  outcome o;
  char b[600];
  double t0 = now_s();
  fuzz *fz = (fuzz *)xmalloc(sizeof(fuzz));
  memset(fz, 0, sizeof(*fz));
  fz->rs = UINT64_C(0x5eed0002);
  fz->n_random = n_random;
  fz->n_mutated = n_mutated;
  host_init(&h);
  h.fz = fz;
  run(&h, "local raw, check = t.raw, t.check\nrepeat until check(pcall(raw))\nerror('fuzz finished', 0)", &o);
  check(o.st == CM_ST_ERROR && head_is(&o, "fuzz finished") && fz->done && fz->mismatches == 0 && h.bad_req == 0 &&
        fz->got_invalid > 0 && fz->got_values > 0 && fz->got_error > 0, "7 reply fuzz",
        "%ld random + %ld mutated valid replies: %ld corrupt, %ld values, %ld errors, %ld mismatches, %ld bad requests "
        "(%.1f s); end \"%s\"%s%s", n_random, n_mutated, fz->got_invalid, fz->got_values, fz->got_error,
        fz->mismatches, h.bad_req, now_s() - t0, show(&o, b, sizeof(b)), fz->first[0] ? "; first mismatch: " : "",
        fz->first);
  out_free(&o);
  host_done(&h);
  free(fz);
}

/* -- 8. Out of memory ------------------------------------------------------------------- */

static void test_oom(void)
{
  host h;
  outcome o;
  cm_machine *m;
  char b[600], first[800];
  int st1, st2;
  long n, allocs;
  size_t base_used, load_used, lim;
  long loaded = 0, nomem = 0, other = 0;
  static const char OOM_BIOS[] =
    "local sizes = {10, 300, 1000, 5000, 70000, 1048576}\n"
    "for _, n in ipairs(sizes) do\n"
    "  local s = string.rep('a', n)\n"
    "  if t.echo(s) ~= s then error('echo mismatch at ' .. n, 0) end\n"
    "end\n"
    "error('done', 0)";
  static const char CHURN_BIOS[] =
    "local d1 = t.used()\n"
    "for i = 1, 400 do local s = string.rep('x', 100000 + i) end\n"
    "error('survived ' .. d1 .. ' ' .. t.used(), 0)";

  host_init(&h);
  /* Cap 4 MiB: string.rep fails, and the machine is still usable. */
  m = cm_new((size_t)4 << 20, KEY);
  h.m = m;
  out_init(&o);
  load_z(m, "local ok, e = pcall(string.rep, 'x', 8 * 1024 * 1024)\n"
            "term.write(tostring(ok) .. ' ' .. tostring(e))\n"
            "coroutine.yield()\n"
            "local s = {}\nfor i = 1, 1000 do s[i] = tostring(i) end\n"
            "error('alive ' .. #s, 0)");
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  acct_note(m, "after a refused string.rep");
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  acct_note(m, "after the run under a 4 MiB cap");
  out_msg(&o, m);
  check(st1 == CM_ST_YIELD && strcmp(h.log, "false not enough memory") == 0 && st2 == CM_ST_ERROR &&
        head_is(&o, "alive 1000"), "8 oom: cap 4 MiB, string.rep fails and the machine goes on",
        "%d \"%s\", %d \"%s\"", st1, h.log, st2, show(&o, b, sizeof(b)));
  cm_free(m);
  out_free(&o);
  expect("8 oom: a 100 MiB string.rep under the 64 MiB cap", &h,
         "local ok, e = pcall(string.rep, 'x', 100 * 1024 * 1024)\n"
         "error(tostring(ok) .. ' ' .. tostring(e) .. ' ' .. t.echo('alive'), 0)", CM_ST_ERROR,
         "false not enough memory alive");
  /* Frees count: 40 MB of garbage strings pass through a 4 MiB cap, and in the middle of the run the cap still
  ** counts what LuaJIT counts (t.used() is the difference).
  */
  run_len(&h, (size_t)4 << 20, CHURN_BIOS, sizeof(CHURN_BIOS) - 1, &o);
  check(o.st == CM_ST_ERROR && head_is(&o, "survived 0 0"),
        "8 oom: garbage far beyond a 4 MiB cap is freed and counted",
        "status %d, \"%s\"", o.st, show(&o, b, sizeof(b)));
  out_free(&o);

  /* A cap so low that cm_load fails; then every cap from 0 to past the load size: 8-byte steps up to 4 KiB above
  ** what a load needs, then 256-byte steps for another 64 KiB (the G1 bios then runs under the cap too).
  */
  m = cm_new(0, KEY);
  out_init(&o);
  o.load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"return", 6);
  out_msg(&o, m);
  check(o.load_st == CM_ST_ERROR && msg_is(&o, "not enough memory") && o.tb == SIZE_MAX, "8 oom: cap 0, cm_load fails",
        "%d \"%s\"", o.load_st, show(&o, b, sizeof(b)));
  cm_free(m);
  m = cm_new(LIMIT, KEY);
  base_used = cm_test_used(m);
  cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)G1_BIOS, sizeof(G1_BIOS) - 1);
  load_used = cm_test_used(m);
  cm_free(m);
  for (lim = 0; lim <= load_used + 65536; lim += lim < load_used + 4096 ? 8 : 256) {
    m = cm_new(lim, KEY);
    h.m = m;
    o.load_st = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)G1_BIOS, sizeof(G1_BIOS) - 1);
    out_msg(&o, m);
    acct_note(m, "after cm_load under a low cap");
    if (o.load_st == CM_ST_OK) {
      o.st = cm_resume(m, host_call, &h, NULL, 0);
      out_msg(&o, m);
      acct_note(m, "after cm_resume under a low cap");
      if (o.st == CM_ST_ERROR && (msg_is(&o, G1_MSG) || head_is(&o, "not enough memory"))) loaded++;
      else other++;
    } else if (o.load_st == CM_ST_ERROR && msg_is(&o, "not enough memory")) {
      nomem++;
    } else {
      other++;
    }
    if (cm_free(m) != CM_ST_OK) other++;
  }
  check(other == 0 && loaded > 0 && nomem > 0, "8 oom: every cap up to the load size",
        "load needs about %lu bytes (state %lu): %ld caps loaded, %ld not enough memory, %ld other",
        (unsigned long)(load_used - base_used), (unsigned long)base_used, loaded, nomem, other);
  out_free(&o);

  /* cm_test_fail_after(n) for every n: cm_new's calloc and each growth of the request buffer. */
  cm_test_fail_after(-1);
  run(&h, OOM_BIOS, &o);
  allocs = cm_test_fail_after(-1);
  check(o.st == CM_ST_ERROR && head_is(&o, "done") && allocs >= 6, "8 oom: the buffer-growth scenario runs",
        "%ld allocations, \"%s\"", allocs, show(&o, b, sizeof(b)));
  out_free(&o);
  other = 0;
  first[0] = '\0';
  for (n = 0; n <= allocs + 1; n++) {
    int good;
    cm_test_fail_after(n);
    run(&h, OOM_BIOS, &o);
    cm_test_fail_after(-1);
    if (n == 0) good = o.load_st == -1;   /* cm_new returned NULL */
    else if (n < allocs) good = o.st == CM_ST_ERROR && head_is(&o, "not enough memory") &&
                                tb_has(&o, "in field 'echo'") && o.free_st == CM_ST_OK;
    else good = o.st == CM_ST_ERROR && head_is(&o, "done") && o.free_st == CM_ST_OK;
    if (!good) {
      other++;
      if (!first[0]) snprintf(first, sizeof(first), "; first bad: n %ld, load %d, resume %d, \"%s\"", n, o.load_st,
                              o.st, o.msg ? show(&o, b, sizeof(b)) : "");
    }
    out_free(&o);
  }
  check(other == 0, "8 oom: cm_test_fail_after(n) for every n", "n = 0..%ld, %ld bad%s", allocs + 1, other, first);
  host_done(&h);
}

/* -- 9. Re-entrancy ------------------------------------------------------------------------ */

typedef struct busy_run { cm_machine *m; host *h; int st; } busy_run;
static void busy_thread(void *p) { busy_run *r = (busy_run *)p; r->st = cm_resume(r->m, host_call, r->h, NULL, 0); }

/* The hammer: refused calls from a second thread while the owner renders its error message. Each round starts with
** the gate closed. The bios calls t.block(), which waits for the gate while the owner holds the busy flag. The
** hammer waits until the owner is blocked there, makes 1,000 rounds of three refused calls, then opens the gate and
** goes on: the owner leaves t.block() and renders its error and traceback while the hammer calls (on a host with 2
** or more CPUs), until the owner's leave hook stops the hammer. Every call must be refused with CM_ST_BUSY: the
** hammer calls only while the owner holds the flag (it starts once the owner is blocked, and the hook waits for it
** to stop before the owner releases the flag), so it never owns the machine.
*/
typedef struct hammer { cm_machine *m; host *h; int stop, done; long refused, other, at_leave; } hammer;

static void hammer_thread(void *p)
{
  hammer *x = (hammer *)p;
  double t0 = now_s();
  long n = 0;
  while (!__atomic_load_n(&x->h->blocked, __ATOMIC_SEQ_CST) && now_s() - t0 < 20) sleep_ms(1);
  if (!__atomic_load_n(&x->h->blocked, __ATOMIC_SEQ_CST)) {   /* the owner never got there: no cm_* call */
    __atomic_store_n(&x->h->gate, 1, __ATOMIC_SEQ_CST);        /* so that a late owner does not wait */
    __atomic_store_n(&x->done, 1, __ATOMIC_SEQ_CST);
    return;
  }
  while (!__atomic_load_n(&x->stop, __ATOMIC_SEQ_CST)) {
    int a = cm_resume(x->m, host_call, x->h, NULL, 0);
    int b = cm_load(x->m, g_spec.data, g_spec.len, (const unsigned char *)"", 0);
    int c = cm_free(x->m);
    if (a == CM_ST_BUSY && b == CM_ST_BUSY && c == CM_ST_BUSY) __atomic_add_fetch(&x->refused, 3, __ATOMIC_SEQ_CST);
    else __atomic_add_fetch(&x->other, 1, __ATOMIC_SEQ_CST);
    if (++n == 1000) __atomic_store_n(&x->h->gate, 1, __ATOMIC_SEQ_CST);   /* the owner goes on and renders */
  }
  __atomic_store_n(&x->done, 1, __ATOMIC_SEQ_CST);
}

/* The owner's leave hook (cm_test_on_leave): its message is written. It lets the hammer make 3,000 more refused
** calls (at most 5 s), then stops it and waits until it has stopped. A hammer that does not stop within 60 s would
** own the machine next, so the test ends the process then.
*/
static void hammer_leave(void *p)
{
  hammer *x = (hammer *)p;
  long base = __atomic_load_n(&x->refused, __ATOMIC_SEQ_CST);
  double t0 = now_s();
  x->at_leave = base;
  while (__atomic_load_n(&x->refused, __ATOMIC_SEQ_CST) < base + 3000 &&
         __atomic_load_n(&x->other, __ATOMIC_SEQ_CST) == 0 && !__atomic_load_n(&x->done, __ATOMIC_SEQ_CST) &&
         now_s() - t0 < 5)
    sleep_ms(1);
  __atomic_store_n(&x->stop, 1, __ATOMIC_SEQ_CST);
  t0 = now_s();
  while (!__atomic_load_n(&x->done, __ATOMIC_SEQ_CST) && now_s() - t0 < 60) sleep_ms(1);
  if (!__atomic_load_n(&x->done, __ATOMIC_SEQ_CST)) {
    fprintf(stderr, "machine-test: the hammer thread did not stop in 60 s\n");
    abort();
  }
}

static void test_reentry(void)
{
  host h;
  outcome o;
  cm_machine *m;
  char b[600];
  int st1, st2, i;
  thr_box box;
  thr_t th;
  busy_run br;

  host_init(&h);
  m = cm_new(LIMIT, KEY);
  h.m = m;
  out_init(&o);
  load_z(m, "local a, b, c, d = t.reenter()\ncoroutine.yield()\nerror(a .. '|' .. b .. '|' .. c .. '|' .. d, 0)");
  st1 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  /* The host reads the message after its refused cm_resume, cm_load and cm_free (white box, see h_reenter): it
  ** must still be the one cm_load left (empty).
  */
  check(st1 == CM_ST_YIELD && h.re_resume == CM_ST_BUSY && h.re_msglen == 0 && h.re_load == CM_ST_BUSY &&
        h.re_free == CM_ST_BUSY,
        "9 re-entrancy: cm_resume, cm_load and cm_free from the host get CM_ST_BUSY and leave the message alone",
        "outer %d; inner resume %d \"%.*s\", load %d, free %d", st1, h.re_resume, (int)h.re_msglen, h.re_msg,
        h.re_load, h.re_free);
  check(o.len == 0 && o.tb == SIZE_MAX, "9 re-entrancy: the owner's YIELD gives its own (empty) message",
        "\"%s\"", show(&o, b, sizeof(b)));
  st2 = cm_resume(m, host_call, &h, NULL, 0);
  out_msg(&o, m);
  check(st2 == CM_ST_ERROR && head_is(&o, "5||5|5"),
        "9 re-entrancy: the machine was not freed or changed",
        "%d \"%s\"", st2, show(&o, b, sizeof(b)));
  check(cm_free(m) == CM_ST_OK, "9 re-entrancy: freed afterwards", NULL);
  out_free(&o);

  /* A second thread while the first is in a host call. */
  m = cm_new(LIMIT, KEY);
  h.m = m;
  h.gate = 0; h.blocked = 0;
  load_z(m, "t.block()\ncoroutine.yield()\nerror('end', 0)");
  br.m = m; br.h = &h; br.st = -1;
  box.fn = busy_thread; box.arg = &br;
  if (!thr_start(&th, &box)) {
    check(0, "9 re-entrancy: second thread", "cannot start a thread");
  } else {
    int fr, rs, ld;
    double t0 = now_s();
    while (!__atomic_load_n(&h.blocked, __ATOMIC_SEQ_CST) && now_s() - t0 < 10) sleep_ms(1);
    if (!__atomic_load_n(&h.blocked, __ATOMIC_SEQ_CST)) {   /* the owner never got there: no cm_* call before join */
      __atomic_store_n(&h.gate, 1, __ATOMIC_SEQ_CST);
      thr_join(th);
      check(0, "9 re-entrancy: cm_free, cm_resume and cm_load from another thread while busy",
            "the owner never reached t.block() (owner %d)", br.st);
      cm_free(m);
    } else {
      fr = cm_free(m);
      rs = cm_resume(m, host_call, &h, NULL, 0);
      ld = cm_load(m, g_spec.data, g_spec.len, (const unsigned char *)"", 0);
      __atomic_store_n(&h.gate, 1, __ATOMIC_SEQ_CST);
      thr_join(th);
      check(fr == CM_ST_BUSY && rs == CM_ST_BUSY && ld == CM_ST_BUSY && br.st == CM_ST_YIELD,
            "9 re-entrancy: cm_free, cm_resume and cm_load from another thread while busy",
            "free %d, resume %d, load %d; owner %d", fr, rs, ld, br.st);
      st1 = cm_resume(m, host_call, &h, NULL, 0);
      out_msg(&o, m);
      check(st1 == CM_ST_ERROR && head_is(&o, "end") && cm_free(m) == CM_ST_OK,
            "9 re-entrancy: the machine works and frees afterwards", "%d \"%s\"", st1, show(&o, b, sizeof(b)));
    }
  }
  out_free(&o);

  /* Refused calls from another thread while the owner renders a 60,000-byte error and its traceback (see
  ** hammer_thread), 10 rounds. Before S1 step 7, each refused cm_resume and cm_load wrote "machine is busy" into
  ** the message the owner was writing.
  */
  {
    static const char race_bios[] =
      "t.block()\n"
      "local function f(n) if n == 0 then error(string.rep('e', 60000), 0) end return (f(n - 1)) end\n"
      "f(30)";
    static const char tb_head[] = "\nstack traceback:\n\t[C]: in function 'error'";
    static const char tb_tail[] = "in main chunk";
    hammer x;
    long refused = 0, other = 0, before = 0;
    int rounds = 0, good = 0;
    char first[200] = "";
    for (i = 0; i < 10; i++) {
      size_t k;
      int ok, st;
      m = cm_new(LIMIT, KEY);
      h.m = m;
      h.gate = 0; h.blocked = 0;   /* the hammer opens the gate (hammer_thread) */
      load_z(m, race_bios);
      memset(&x, 0, sizeof(x));
      x.m = m; x.h = &h;
      box.fn = hammer_thread; box.arg = &x;
      if (!thr_start(&th, &box)) {
        cm_free(m);
        break;
      }
      cm_test_on_leave(m, hammer_leave, &x);
      st = cm_resume(m, host_call, &h, NULL, 0);
      cm_test_on_leave(m, NULL, NULL);
      thr_join(th);
      rounds++;
      refused += x.refused;
      other += x.other;
      before += x.at_leave;
      out_msg(&o, m);
      /* at_leave: the refused calls made before the owner's message was complete; 3,000 come before the gate opens */
      ok = st == CM_ST_ERROR && o.len > 60000 + sizeof(tb_head) && o.tb == 60000 && x.other == 0 &&
           x.at_leave >= 3000 && x.refused > x.at_leave;
      for (k = 0; ok && k < 60000; k++) ok = o.msg[k] == 'e';
      ok = ok && memcmp(o.msg + 60000, tb_head, sizeof(tb_head) - 1) == 0 &&
           memcmp(o.msg + o.len - (sizeof(tb_tail) - 1), tb_tail, sizeof(tb_tail) - 1) == 0;
      if (ok) good++;
      else if (!first[0])
        snprintf(first, sizeof(first), "; round %d: status %d, %lu bytes, tb %ld, hammer %ld refused (%ld before "
                 "the message was complete), %ld other", i, st, (unsigned long)o.len,
                 o.tb == SIZE_MAX ? -1L : (long)o.tb, x.refused, x.at_leave, x.other);
      out_free(&o);
      if (cm_free(m) != CM_ST_OK) other++;
    }
    check(rounds == 10 && good == 10 && other == 0,
          "9 re-entrancy: refused calls from another thread while the owner renders its error change nothing",
          "%d of %d rounds intact, %ld refused calls (%ld before the messages were complete), %ld not refused%s",
          good, rounds, refused, before, other, first);
  }
  host_done(&h);
}

/* -- 10. Threads and churn ---------------------------------------------------------------- */

typedef struct handoff { cm_machine *m; host h; int st_load, st1, st2, st_free; } handoff;
static void ho_create(void *p)
{
  handoff *x = (handoff *)p;
  static const char bios[] =
    "local n = 0\nwhile true do\n  n = n + 1\n  term.write('r' .. n)\n  coroutine.yield()\nend";
  x->m = cm_new(LIMIT, KEY);
  x->st_load = x->m ? cm_load(x->m, g_spec.data, g_spec.len, (const unsigned char *)bios, sizeof(bios) - 1) : -1;
}
static void ho_resume(void *p)
{
  handoff *x = (handoff *)p;
  x->h.m = x->m;
  x->st1 = cm_resume(x->m, host_call, &x->h, NULL, 0);
  x->st2 = cm_resume(x->m, host_call, &x->h, NULL, 0);
}
static void ho_free(void *p) { handoff *x = (handoff *)p; x->st_free = cm_free(x->m); }

typedef struct churn { int count, ok, bad; char first[400]; } churn;
static void churn_thread(void *p)
{
  churn *c = (churn *)p;
  int i;
  for (i = 0; i < c->count; i++) {
    host h;
    outcome o;
    host_init(&h);
    run(&h, G1_BIOS, &o);
    if (o.load_st == CM_ST_OK && o.st == CM_ST_ERROR && msg_is(&o, G1_MSG) && o.free_st == CM_ST_OK &&
        strcmp(h.log, "hi") == 0) {
      c->ok++;
    } else {
      c->bad++;
      if (!c->first[0]) {
        char b[300];
        snprintf(c->first, sizeof(c->first), "load %d resume %d free %d \"%s\"", o.load_st, o.st, o.free_st,
                 show(&o, b, sizeof(b)));
      }
    }
    out_free(&o);
    host_done(&h);
  }
}

static void test_threads(void)
{
  handoff x;
  churn c[3];
  thr_box box[3];
  thr_t th[3];
  int i, started = 0, ok = 0, bad = 0;
  double t0, mem0, mem1;

  memset(&x, 0, sizeof(x));
  host_init(&x.h);
  if (!on_thread(ho_create, &x) || !on_thread(ho_resume, &x) || !on_thread(ho_free, &x)) {
    check(0, "10 threads: new, resume and free on three threads", "cannot start a thread");
  } else {
    check(x.st_load == CM_ST_OK && x.st1 == CM_ST_YIELD && x.st2 == CM_ST_YIELD && strcmp(x.h.log, "r1r2") == 0 &&
          x.st_free == CM_ST_OK, "10 threads: new, resume and free on three threads",
          "load %d, resumes %d %d, log \"%s\", free %d", x.st_load, x.st1, x.st2, x.h.log, x.st_free);
  }
  host_done(&x.h);

  t0 = now_s();
  mem0 = mem_bytes();
  memset(c, 0, sizeof(c));
  c[0].count = 334; c[1].count = 333; c[2].count = 333;
  for (i = 0; i < 3; i++) {
    box[i].fn = churn_thread; box[i].arg = &c[i];
    if (thr_start(&th[i], &box[i])) started++;
    else break;
  }
  for (i = 0; i < started; i++) thr_join(th[i]);
  mem1 = mem_bytes();
  for (i = 0; i < 3; i++) { ok += c[i].ok; bad += c[i].bad; }
  check(started == 3 && ok == 1000 && bad == 0, "10 churn: 1,000 machines on three threads at once",
        "%d ok, %d bad (%.1f s)%s%s%s%s", ok, bad, now_s() - t0, c[0].first[0] || c[1].first[0] || c[2].first[0] ?
        "; first bad: " : "", c[0].first, c[1].first, c[2].first);
  /* cm_free gives LuaJIT its own allocator back before lua_close, so that lua_close destroys the state's arena
  ** (M1.md section 3.7 cm_free step 2); without that, each machine leaves its arena behind. ASan's quarantine
  ** keeps freed blocks, so a sanitized build only prints the figures.
  */
#ifdef __SANITIZE_ADDRESS__
  printf("info 10 churn: process memory %.1f MiB before, %.1f MiB after (not checked under ASan)\n",
         mem0 / 1048576.0, mem1 / 1048576.0);
#else
  check(mem0 > 0 && mem1 > 0 && mem1 - mem0 < (double)MEM_GROWTH_MAX, "10 churn: process memory stays flat",
        "%.1f MiB before, %.1f MiB after the 1,000 machines (at most %.0f MiB more)", mem0 / 1048576.0,
        mem1 / 1048576.0, (double)MEM_GROWTH_MAX / 1048576.0);
#endif
}

/* -- Main ---------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
  int smoke = argc > 1 && strcmp(argv[1], "--smoke") == 0;
  host h;
  outcome o;
  char b[600];

  setvbuf(stdout, NULL, _IOLBF, 4096);
  build_spec();
  printf("machine_test: CCLJ_ABI %d, %d host functions, spec %lu bytes%s\n", CCLJ_ABI, g_nfn,
         (unsigned long)g_spec.len, smoke ? " (smoke)" : "");
  host_init(&h);
  run(&h, G1_BIOS, &o);
  check(o.load_st == CM_ST_OK && o.st == CM_ST_ERROR && msg_is(&o, G1_MSG) && o.free_st == CM_ST_OK,
        "smoke: the G1 bios end to end", "load %d resume %d free %d \"%s\"", o.load_st, o.st, o.free_st,
        show(&o, b, sizeof(b)));
  out_free(&o);
  host_done(&h);
  if (!smoke) {
    test_allow_lists();
    test_load();
    test_spec_fuzz(100000);
    test_resume();
    test_errors();
    test_host();
    test_reply_fuzz(100000, 100000);
    test_oom();
    test_reentry();
    test_threads();
  }
  if (!smoke) {
    check(g_tb_leaks == 0 && g_cotop_errors == 0 && g_tb_badline == 0, "all: no traceback of any test has \"0x\", "
          "\"[builtin\" or a line number below 1, or changed lua_gettop(co)",
          "%ld tracebacks: %ld with an address or builtin name, %ld with a line below 1, %ld changed the top",
          g_cotop_runs, g_tb_leaks, g_tb_badline, g_cotop_errors);
    check(g_acct_checks > 1000 && g_acct_bad == 0, "all: after every load and resume, the cap counted what LuaJIT "
          "counts", "%ld of %ld checks differ%s%s", g_acct_bad, g_acct_checks, g_acct_first[0] ? "; first: " : "",
          g_acct_first);
  }
  cb_free(&g_spec);
  printf("machine_test: %d checks, %d failed\n", n_checks, n_failed);
  return n_failed ? 1 : 0;
}
