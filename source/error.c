#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include "error.h"
#include "log.h"

static int g_graphics_owned;

void error_set_graphics_owned(int owned) { g_graphics_owned = owned; }

void fatal_error(const char *fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  LOGE("FATAL: %s", msg);
  bk_log_close();

  if (g_graphics_owned) {
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, "Banana Kong stopped.", msg)))
      errorApplicationShow(&c);
    exit(1);
  }

  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  printf("\x1b[1;1H\x1b[31;1mBanana Kong for Switch\x1b[0m\n\n%s\n\nPress + to exit.\n", msg);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
    svcSleepThread(16000000ll);
  }
  consoleExit(NULL);
  exit(1);
}
