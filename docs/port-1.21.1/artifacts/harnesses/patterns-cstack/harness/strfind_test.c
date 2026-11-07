/* strfind_test.c -- check the Two-Way fallback of patch 07's lj_str_find().
** 1. Random differential test: tw_find() and the hybrid vs a naive search.
** 2. Timing of the hybrid vs the stock memchr/memcmp loop on typical and
**    adversarial inputs.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include "strfind_twoway.h"

typedef uint32_t MSize;
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(uint32_t n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs % n); }

static const char *naive(const char *s, const char *p, MSize slen, MSize plen)
{
  MSize i;
  if (plen > slen) return NULL;
  for (i = 0; i + plen <= slen; i++)
    if (memcmp(s + i, p, plen) == 0) return s + i;
  return NULL;
}

/* Stock LuaJIT lj_str_find. */
static const char *stock(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      plen--; slen -= plen;
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
      }
    }
  }
  return NULL;
}

/* Patched lj_str_find (same code as in lj_str.c). BUDGET_MUL is the factor. */
static const char *hybrid(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      uint64_t b;
      MSize budget;
      plen--; slen -= plen;
      b = 64 + ((uint64_t)slen * 8) / (plen + 1);
      budget = b > 0xffffffffu ? 0xffffffffu : (MSize)b;
#ifdef TINY_BUDGET
      budget = 1 + rnd(3);  /* Exercise the hand-over to Two-Way mid-way. */
#endif
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
	if (--budget == 0)
	  return tw_find((const uint8_t *)s, slen + plen, (const uint8_t *)p - 1, plen + 1);
      }
    }
  }
  return NULL;
}


static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }

int main(int argc, char **argv)
{
  long iters = argc > 1 ? atol(argv[1]) : 2000000, it, bad = 0, found = 0;
  static char hs[4096], nd[4096];
  for (it = 0; it < iters; it++) {
    int alpha = 1 + rnd(4), mode = rnd(4);
    MSize hl = rnd(mode == 3 ? 3000 : 60), nl = rnd(mode == 3 ? 40 : 12), i;
    const char *a, *b, *c, *d;
    for (i = 0; i < hl; i++) hs[i] = (char)('a' + rnd(alpha));
    if (mode == 1 && hl) {  /* Needle taken from the haystack. */
      MSize off = rnd(hl); if (nl > hl - off) nl = hl - off;
      memcpy(nd, hs + off, nl);
      if (nl && rnd(2)) nd[rnd(nl)] = (char)('a' + rnd(alpha));
    } else if (mode == 2) {  /* Periodic needle. */
      MSize per = 1 + rnd(3);
      for (i = 0; i < nl; i++) nd[i] = (char)('a' + (i % per == 0 ? 0 : rnd(alpha)));
      for (i = per; i < nl; i++) nd[i] = nd[i - per];
    } else {
      for (i = 0; i < nl; i++) nd[i] = (char)('a' + rnd(alpha));
    }
    if (rnd(8) == 0) { hs[rnd(hl ? hl : 1)] = (char)0xff; }
    a = naive(hs, nd, hl, nl);
    b = nl ? (nl <= hl ? tw_find((const uint8_t *)hs, hl, (const uint8_t *)nd, nl) : NULL) : hs;
    c = hybrid(hs, nd, hl, nl);
    d = stock(hs, nd, hl, nl);
    if (a) found++;
    if (a != b || a != c || a != d) {
      if (bad++ < 10) printf("MISMATCH hl=%u nl=%u naive=%d tw=%d hybrid=%d stock=%d\n", hl, nl,
	     a ? (int)(a - hs) : -1, b ? (int)(b - hs) : -1, c ? (int)(c - hs) : -1, d ? (int)(d - hs) : -1);
    }
  }
  printf("random: %ld cases, %ld found, %ld mismatches\n", iters, found, bad);
  {  /* Timing. */
    static const struct { const char *name; } T[] = {{"text, 5-byte needle"}, {"text, 40-byte needle"},
      {"text, 300-byte needle, absent"}, {"adversarial a^n vs a^(m)b, n=4e6 m=2e6"},
      {"adversarial a^n vs a^m b a^m, n=4e6 m=1e6"}, {"random 4-letter, 12-byte needle"}};
    MSize n = 4000000, k, j;
    char *h = malloc(n + 1), *p = malloc(n + 1);
    for (k = 0; k < sizeof(T) / sizeof(T[0]); k++) {
      MSize pl;
      double t0, t1, t2; const char *r1, *r2;
      if (k <= 2) {
	static const char *words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "and ", "then ", "some "};
	MSize w = 0;
	for (j = 0; j < n; ) { const char *wd = words[rnd(11)]; size_t L = strlen(wd); if (j + L > n) break; memcpy(h + j, wd, L); j += L; }
	for (; j < n; j++) h[j] = ' ';
	pl = k == 0 ? 5 : k == 1 ? 40 : 300;
	memcpy(p, h + n / 2, pl); p[pl / 2] = 'Z';  /* absent */
      } else if (k == 3) {
	memset(h, 'a', n); pl = 2000000; memset(p, 'a', pl); p[pl - 1] = 'b';
      } else if (k == 4) {
	memset(h, 'a', n); pl = 2000001; memset(p, 'a', pl); p[1000000] = 'b';
      } else {
	for (j = 0; j < n; j++) h[j] = (char)('a' + rnd(4));
	pl = 12; for (j = 0; j < pl; j++) p[j] = (char)('a' + rnd(4));
      }
      t0 = now(); r1 = hybrid(h, p, n, pl); t1 = now();
      if (k >= 3 && k <= 4) { r2 = r1; t2 = t1 + 0; printf("  %-45s hybrid %.4f s  stock: skipped (est. ~90 s from the n=2e5 rate)\n", T[k].name, t1 - t0); continue; }
      r2 = stock(h, p, n, pl); t2 = now();
      printf("  %-45s hybrid %.4f s  stock %.4f s %s\n", T[k].name, t1 - t0, t2 - t1, r1 == r2 ? "" : "MISMATCH");
    }
    {  /* Stock on a smaller adversarial case to show the quadratic cost. */
      MSize n2 = 200000, m2 = 100000; double t0, t1, t3;
      memset(h, 'a', n2); memset(p, 'a', m2); p[m2 - 1] = 'b';
      t0 = now(); stock(h, p, n2, m2); t1 = now(); hybrid(h, p, n2, m2); t3 = now();
      printf("  adversarial n=2e5 m=1e5: stock %.3f s, hybrid %.5f s\n", t1 - t0, t3 - t1);
    }
  }
  return bad != 0;
}
