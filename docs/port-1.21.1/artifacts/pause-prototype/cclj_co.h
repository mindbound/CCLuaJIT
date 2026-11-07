/*
** cclj_co.h - prototype of the CCLuaJIT "pause token protocol" (PLAN.md P10).
**
** One CcljMachine per Lua state. Other threads only call cclj_request();
** everything else runs on the VM thread.
*/
#ifndef CCLJ_CO_H
#define CCLJ_CO_H

#include "plat.h"
#include "lua.h"

#define CCLJ_REQ_PAUSE 1
#define CCLJ_REQ_SOFT  2
#define CCLJ_REQ_HARD  4

#define CCLJ_MAX_TAGS    64
#define CCLJ_MAX_REGIONS 32

typedef struct CcljMachine {
  /* -- Shared with other threads ---------------------------------------- */
  lua_State *L;              /* Main state; NULL after close. Guarded by lock. */
  CRITICAL_SECTION lock;     /* Serialises cclj_request() against close. */
  volatile LONG req;         /* Pending CCLJ_REQ_* bits. */
  volatile LONG running;     /* VM thread is inside lua_resume(top). */
  volatile LONGLONG t_req_pause;  /* QPC time of the last pause request. */
  volatile LONGLONG t_req_hard;   /* QPC time of the hard-abort request. */

  char pad[64];              /* Keep VM-thread fields off the shared cache line. */

  /* -- VM thread only ---------------------------------------------------- */
  lua_State *top;            /* Top-level ("bios") coroutine resumed by the host. */
  int pausing;               /* A pause is propagating, or the machine is paused. */
  int aborting;              /* Hard abort in progress (state will be closed). */
  int nonyield;              /* Active nested resumes whose caller cannot yield. */
  int depth;                 /* Active nested resumes (C-level lua_resume calls). */
  int max_depth;             /* Nesting limit enforced by the resume wrapper. */
  int naive;                 /* Control mode: plain hook-yield, native coroutine lib. */
  int region;                /* Test instrumentation: current code region id. */
  int defer_rearm;           /* 1 = v1 deferral (re-arm intr in the hook). */
  int hookn;                 /* Normal count hook period. */
  int defer_hookn;           /* Shorter period while a pause is deferred (0 = keep). */
  int deferring;             /* The short period is installed. */
  int use_checkpoint;        /* Install the xpcall/load checkpoint wrappers. */
  long ncheckpoint;          /* Pauses started by a checkpoint. */

  /* -- Statistics (VM thread) ------------------------------------------- */
  long nhookpause;           /* Hook-yields that started a pause. */
  long npause;               /* PAUSE results seen by the host. */
  long nprop;                /* Propagation steps (wrapper yielded upward). */
  long nfailprop;            /* Propagation impossible -> pause cancelled (expect 0). */
  long ndefer;               /* Hook runs that had to defer a pause. */
  long nsoft;                /* Soft aborts raised. */
  long nresume;              /* Nested resumes performed by the wrapper. */
  int  depth_hw;             /* High-water mark of depth. */
  long ndepth_err;           /* Nesting limit errors. */
  LONGLONG defer_t0;         /* QPC time of the first deferral of the current pause. */
  double max_defer_us;       /* Longest deferral (first defer -> pause). */
  double max_pause_lat_us;   /* Longest request -> hook-yield latency. */
  double sum_pause_lat_us;
  long rpause[CCLJ_MAX_REGIONS], rdefer[CCLJ_MAX_REGIONS];
  lua_State *tag_co[CCLJ_MAX_TAGS];
  int tag_level[CCLJ_MAX_TAGS];
  int ntags;
  long tagpause[CCLJ_MAX_TAGS];    /* Pauses per tagged level. */
  uintptr_t stack_base;      /* Highest stack address of the VM thread. */
  uintptr_t min_sp;          /* Lowest stack pointer seen in the resume wrapper. */
  uintptr_t stack_floor;     /* Refuse to nest below this SP (set per handleEvent). */
  long nfloor_err;           /* Stack-floor refusals. */
} CcljMachine;

extern double cclj_qpc_us(LONGLONG ticks);
extern LONGLONG cclj_qpc(void);

/* VM thread: create/destroy. */
CcljMachine *cclj_new(int naive, int max_depth);
void cclj_close(CcljMachine *m);
/* VM thread: replace coroutine.resume/wrap, install the count hook. */
void cclj_install(CcljMachine *m, int hookcount);
/* Any thread: request PAUSE/SOFT/HARD. */
void cclj_request(CcljMachine *m, int what);
/* Any thread, every ~250 us: re-interrupt while a deferred pause is pending. */
void cclj_poke(CcljMachine *m);
/* VM thread: resume the top coroutine like CC:T's handleEvent does. */
int cclj_resume_top(CcljMachine *m, int nargs);

#endif
