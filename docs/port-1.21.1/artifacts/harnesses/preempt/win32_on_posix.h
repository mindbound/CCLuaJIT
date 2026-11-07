/* Minimal Win32 -> POSIX shim so artifacts/harnesses/preempt/harness.c builds on Linux/macOS unchanged
   (except dropping the <windows.h>/<mmsystem.h> includes). */
#ifndef WIN32_ON_POSIX_H
#define WIN32_ON_POSIX_H
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
typedef long LONG; typedef long long LONGLONG; typedef unsigned int DWORD; typedef void *LPVOID;
typedef pthread_t HANDLE;
typedef union { LONGLONG QuadPart; } LARGE_INTEGER;
#define WINAPI
#define INFINITE 0
static inline int QueryPerformanceCounter(LARGE_INTEGER *t) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec; return 1; }
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return 1; }
static inline void Sleep(DWORD ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, 0); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline void ExitProcess(int c) { _exit(c); }
static inline void timeBeginPeriod(int x) { (void)x; }
typedef DWORD (*win_thread_fn)(LPVOID);
struct win_thunk { win_thread_fn fn; LPVOID arg; };
static void *win_thunk_run(void *p) { struct win_thunk t = *(struct win_thunk *)p; free(p); t.fn(t.arg); return 0; }
static inline HANDLE CreateThread(void *a, int s, win_thread_fn fn, LPVOID arg, int f, void *id) {
  pthread_t th; struct win_thunk *t = malloc(sizeof *t); (void)a; (void)s; (void)f; (void)id;
  t->fn = fn; t->arg = arg; pthread_create(&th, 0, win_thunk_run, t); return th; }
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)ms; pthread_join(h, 0); return 0; }
#include <unistd.h>
#endif
#endif
