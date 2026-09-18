#ifndef BK_LUAJIT_GUARD_H
#define BK_LUAJIT_GUARD_H

#include "so_util.h"

/* Horizon will not hand LuaJIT writable-then-executable memory, so the trace
 * compiler must never run. The engine calls luaL_openlibs through its PLT and
 * reaches luaopen_jit through one ABS64 relocation, and every
 * luaJIT_setmode call goes through the PLT too. Those slots are pointed at
 * wrappers that call the real functions and then turn the JIT engine off, and
 * that refuse any later request to turn it back on. The interpreter (and the
 * whole jit.* library apart from compilation) keeps working. Must run between
 * so_relocate/so_resolve and so_finalize. Returns the number of slots patched. */
int luajit_guard_install(so_module *mod);

#endif
