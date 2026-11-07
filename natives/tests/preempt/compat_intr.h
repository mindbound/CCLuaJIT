/* luaJIT_interrupt() only exists in -DLUAJIT_ENABLE_INTERRUPT builds. */
#ifdef HAVE_LUAJIT_INTERRUPT
#define luaJIT_interrupt_compat(L) luaJIT_interrupt(L)
#else
#define luaJIT_interrupt_compat(L) \
  (fprintf(stderr, "luaJIT_interrupt not available in this build\n"), ExitProcess(4))
#endif
