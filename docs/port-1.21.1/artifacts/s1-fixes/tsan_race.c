/* One-off ThreadSanitizer harness for review finding R1 (S1 step 7). The race of Tier 0's new section 9 check,
** without its gate: a second thread calls cm_resume, cm_load and cm_free on a machine from the time its owner has
** entered t.block() (the host function returns at once) until the owner's leave hook (cm_test_on_leave) stops it,
** just before the owner releases the busy flag. Its calls can overlap the owner's render of a 60,000-byte error and
** its traceback, but nothing forces that; ThreadSanitizer needs no overlap in time. Build with -DCCLJ_TEST
** -fsanitize=thread against machine.c and an assert LuaJIT.
*/
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cclj_machine.h"

static const unsigned char SPEC[] = {1, 0, 0, 0,   1, 0, 0, 0,   1, 0, 0, 0, 't',   0,   1, 0, 0, 0,
                                     5, 0, 0, 0, 'b', 'l', 'o', 'c', 'k',   0};
static const char BIOS[] =
  "t.block()\n"
  "local function f(n) if n == 0 then error(string.rep('e', 60000), 0) end return (f(n - 1)) end\n"
  "f(30)";

static cm_machine *M;
static int blocked, stop, done;
static long refused, other;

static int host(void *h, uint32_t fn, const cm_buf *req, cm_buf *rep)
{
  (void)h; (void)fn; (void)req;
  if (rep->cap < 5) {
    unsigned char *p = realloc(rep->data, 5);
    if (p == NULL) return CM_HOST_E_NOMEM;
    rep->data = p;
    rep->cap = 5;
  }
  memset(rep->data, 0, 5);   /* VALUES, no values */
  rep->len = 5;
  __atomic_store_n(&blocked, 1, __ATOMIC_SEQ_CST);
  return CM_HOST_OK;
}

static void *hammer(void *x)
{
  (void)x;
  while (!__atomic_load_n(&blocked, __ATOMIC_SEQ_CST)) sched_yield();
  while (!__atomic_load_n(&stop, __ATOMIC_SEQ_CST)) {
    int a = cm_resume(M, host, NULL, NULL, 0);
    int b = cm_load(M, SPEC, sizeof(SPEC), (const unsigned char *)"", 0);
    int c = cm_free(M);
    if (a == CM_ST_BUSY && b == CM_ST_BUSY && c == CM_ST_BUSY) __atomic_add_fetch(&refused, 3, __ATOMIC_SEQ_CST);
    else __atomic_add_fetch(&other, 1, __ATOMIC_SEQ_CST);
  }
  __atomic_store_n(&done, 1, __ATOMIC_SEQ_CST);
  return NULL;
}

static void leave(void *ud)
{
  long base = __atomic_load_n(&refused, __ATOMIC_SEQ_CST);
  int i;
  (void)ud;
  for (i = 0; i < 5000 && __atomic_load_n(&refused, __ATOMIC_SEQ_CST) < base + 3000; i++) usleep(1000);
  __atomic_store_n(&stop, 1, __ATOMIC_SEQ_CST);
  while (!__atomic_load_n(&done, __ATOMIC_SEQ_CST)) usleep(1000);
}

int main(void)
{
  int round, good = 0;
  for (round = 0; round < 10; round++) {
    pthread_t t;
    size_t len, tb, k;
    const unsigned char *msg;
    int st, ok;
    M = cm_new(64u << 20, 1);
    if (M == NULL || cm_load(M, SPEC, sizeof(SPEC), (const unsigned char *)BIOS, strlen(BIOS)) != CM_ST_OK) {
      printf("round %d: load failed\n", round);
      return 2;
    }
    blocked = stop = done = 0;
    if (pthread_create(&t, NULL, hammer, NULL) != 0) return 2;
    cm_test_on_leave(M, leave, NULL);
    st = cm_resume(M, host, NULL, NULL, 0);
    cm_test_on_leave(M, NULL, NULL);
    pthread_join(t, NULL);
    msg = cm_message(M, &len, &tb);
    ok = st == CM_ST_ERROR && len > 60000 && tb == 60000;
    for (k = 0; ok && k < 60000; k++) ok = msg[k] == 'e';
    if (ok) good++;
    else printf("round %d: status %d, %zu bytes, tb %zd\n", round, st, len,
                tb == (size_t)-1 ? (ssize_t)-1 : (ssize_t)tb);
    cm_free(M);
  }
  printf("tsan_race: %d of 10 rounds intact, %ld refused calls, %ld not refused\n", good, refused, other);
  return good == 10 && other == 0 ? 0 : 1;
}
