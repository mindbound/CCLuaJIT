/* plat.h - minimal Windows/POSIX portability layer for the prototype. */
#ifndef PLAT_H
#define PLAT_H

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
typedef ULONG_PTR plat_uptr;
#define THREAD_FN(name) DWORD WINAPI name(LPVOID p)
typedef HANDLE plat_thread;
static inline plat_thread plat_thread_start(LPTHREAD_START_ROUTINE fn, void *arg, size_t stack)
{
  /* Explicit reservation, like a Java thread created with a 1 MB stack. */
  return CreateThread(NULL, stack, fn, arg, stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
}
static inline void plat_thread_join(plat_thread t) { WaitForSingleObject(t, INFINITE); }
static inline void plat_stack_limits(plat_uptr *lo, plat_uptr *hi) { GetCurrentThreadStackLimits(lo, hi); }
#define plat_timer_init() timeBeginPeriod(1)
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <limits.h>
#include <sched.h>
#include <time.h>
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uintptr_t plat_uptr;
typedef pthread_mutex_t CRITICAL_SECTION;
#define InitializeCriticalSection(p) pthread_mutex_init((p), NULL)
#define DeleteCriticalSection(p) pthread_mutex_destroy(p)
#define EnterCriticalSection(p) pthread_mutex_lock(p)
#define LeaveCriticalSection(p) pthread_mutex_unlock(p)
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#if defined(__x86_64__) || defined(__i386__)
#define YieldProcessor() __builtin_ia32_pause()
#else
#define YieldProcessor() sched_yield()
#endif
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#define THREAD_FN(name) void *name(void *p)
typedef pthread_t plat_thread;
static inline plat_thread plat_thread_start(void *(*fn)(void *), void *arg, size_t stack)
{
  pthread_t t; pthread_attr_t a;
  pthread_attr_init(&a);
  if (stack) pthread_attr_setstacksize(&a, stack);
  pthread_create(&t, &a, fn, arg);
  pthread_attr_destroy(&a);
  return t;
}
static inline void plat_thread_join(plat_thread t) { pthread_join(t, NULL); }
static inline void plat_stack_limits(plat_uptr *lo, plat_uptr *hi)
{
  pthread_attr_t a; void *addr; size_t size;
  pthread_getattr_np(pthread_self(), &a);
  pthread_attr_getstack(&a, &addr, &size);
  pthread_attr_destroy(&a);
  *lo = (plat_uptr)addr; *hi = (plat_uptr)addr + size;
}
#define plat_timer_init() ((void)0)
#endif

#endif
