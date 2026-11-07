/* Two-Way string matching (Crochemore & Perrin 1991), as used by
** lj_str_find() in patch 07. Linear time in the worst case, O(1) memory.
** Shared by the standalone test (strfind_test.c); the same code is in lj_str.c.
*/
#ifndef TW_MSize
#define TW_MSize uint32_t
#endif

/* Critical factorization of n[0..nlen-1]: returns the start of the right
** half (the maximal suffix) and stores the period of the right half.
*/
static TW_MSize tw_factor(const uint8_t *n, TW_MSize nlen, TW_MSize *period)
{
  TW_MSize ms, msr, j, k, p, pr;
  /* Maximal suffix for '<'. */
  ms = (TW_MSize)-1; j = 0; k = p = 1;
  while (j + k < nlen) {
    uint8_t a = n[j + k], b = n[ms + k];
    if (a < b) { j += k; k = 1; p = j - ms; }
    else if (a == b) { if (k != p) k++; else { j += p; k = 1; } }
    else { ms = j++; k = p = 1; }
  }
  /* Maximal suffix for '>'. */
  msr = (TW_MSize)-1; j = 0; k = pr = 1;
  while (j + k < nlen) {
    uint8_t a = n[j + k], b = n[msr + k];
    if (a > b) { j += k; k = 1; pr = j - msr; }
    else if (a == b) { if (k != pr) k++; else { j += pr; k = 1; } }
    else { msr = j++; k = pr = 1; }
  }
  /* The longer of the two maximal suffixes gives a critical factorization. */
  if (msr + 1 < ms + 1) { *period = p; return ms + 1; }
  *period = pr; return msr + 1;
}

static const char *tw_find(const uint8_t *h, TW_MSize hlen,
			   const uint8_t *n, TW_MSize nlen)
{
  TW_MSize period, suffix = tw_factor(n, nlen, &period), i, j = 0;
  if (nlen > hlen) return NULL;
  if (memcmp(n, n + period, suffix) == 0) {
    /* Periodic needle: remember how much of the right half matched. */
    TW_MSize memory = 0;
    while (j <= hlen - nlen) {
      i = suffix > memory ? suffix : memory;
      while (i < nlen && n[i] == h[i + j]) i++;
      if (i >= nlen) {
	i = suffix - 1;
	while (memory < i + 1 && n[i] == h[i + j]) i--;
	if (i + 1 < memory + 1) return (const char *)(h + j);
	j += period;
	memory = nlen - period;
      } else {
	j += i - suffix + 1;
	memory = 0;
      }
    }
  } else {
    /* Non-periodic needle: any mismatch in the left half shifts maximally. */
    period = (suffix > nlen - suffix ? suffix : nlen - suffix) + 1;
    while (j <= hlen - nlen) {
      i = suffix;
      while (i < nlen && n[i] == h[i + j]) i++;
      if (i >= nlen) {
	i = suffix - 1;
	while (i != (TW_MSize)-1 && n[i] == h[i + j]) i--;
	if (i == (TW_MSize)-1) return (const char *)(h + j);
	j += period;
      } else {
	j += i - suffix + 1;
      }
    }
  }
  return NULL;
}
