/* log.c -- optional text log on the SD card. Compiled to no-ops unless the
 * build sets LOG=1; the log file is only opened in that case, because flushing
 * to the SD card from the render thread costs frames. */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "log.h"

static FILE *g_log;
static Mutex g_log_mu;
static u64 g_t0;

void bk_log_open(const char *path) {
#if BK_LOG_ENABLE
  mutexInit(&g_log_mu);
  g_t0 = armGetSystemTick();
  g_log = fopen(path, "w");
  if (g_log) setvbuf(g_log, NULL, _IOLBF, 4096);
#else
  (void)path;
#endif
}

void bk_log_close(void) {
  if (!g_log) return;
  mutexLock(&g_log_mu);
  fclose(g_log);
  g_log = NULL;
  mutexUnlock(&g_log_mu);
}

void bk_log_vwrite(const char *tag, const char *fmt, va_list ap) {
  if (!g_log) return;
  u64 ms = armTicksToNs(armGetSystemTick() - g_t0) / 1000000ull;
  char line[1024];
  vsnprintf(line, sizeof(line), fmt, ap);
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
  mutexLock(&g_log_mu);
  fprintf(g_log, "%6llu.%03llu [%s] %s\n", (unsigned long long)(ms / 1000),
          (unsigned long long)(ms % 1000), tag ? tag : "-", line);
  fflush(g_log);
  mutexUnlock(&g_log_mu);
}

void bk_log_write(const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bk_log_vwrite(tag, fmt, ap);
  va_end(ap);
}
