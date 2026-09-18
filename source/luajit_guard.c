#include "log.h"
#include "luajit_guard.h"

#define LUAJIT_MODE_MASK   0x00ff
#define LUAJIT_MODE_ENGINE 0
#define LUAJIT_MODE_ON     0x0100

static int  (*real_luaopen_jit)(void *L);
static void (*real_openlibs)(void *L);
static int  (*real_setmode)(void *L, int idx, int mode);

static void jit_off(void *L) {
  if (real_setmode) real_setmode(L, 0, LUAJIT_MODE_ENGINE);   /* ENGINE | OFF */
}

static int guard_setmode(void *L, int idx, int mode) {
  if ((mode & LUAJIT_MODE_MASK) == LUAJIT_MODE_ENGINE && (mode & LUAJIT_MODE_ON)) {
    LOG_ONCE("luaJIT_setmode(ENGINE|ON) refused: no executable memory on this platform");
    mode &= ~LUAJIT_MODE_ON;
  }
  return real_setmode(L, idx, mode);
}

static int guard_luaopen_jit(void *L) {
  int r = real_luaopen_jit(L);
  jit_off(L);
  return r;
}

static void guard_openlibs(void *L) {
  real_openlibs(L);
  jit_off(L);
}

int luajit_guard_install(so_module *mod) {
  real_setmode = (void *)so_try_find_addr_rx(mod, "dm_luaJIT_setmode");
  real_luaopen_jit = (void *)so_try_find_addr_rx(mod, "dm_luaopen_jit");
  real_openlibs = (void *)so_try_find_addr_rx(mod, "dm_luaL_openlibs");
  int n = 0;
  if (real_setmode) {
    n += so_override_symbol(mod, "dm_luaJIT_setmode", (uintptr_t)guard_setmode);
    if (real_luaopen_jit) n += so_override_symbol(mod, "dm_luaopen_jit", (uintptr_t)guard_luaopen_jit);
    if (real_openlibs) n += so_override_symbol(mod, "dm_luaL_openlibs", (uintptr_t)guard_openlibs);
  }
  LOGI("luajit guard: setmode %p luaopen_jit %p openlibs %p, %d slots patched",
       (void *)real_setmode, (void *)real_luaopen_jit, (void *)real_openlibs, n);
  return n;
}
