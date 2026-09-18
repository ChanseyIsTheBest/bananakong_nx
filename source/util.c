#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "error.h"
#include "util.h"

void bionic_tls_install(void *buf) {
  if (!buf) buf = memalign(16, BIONIC_TLS_SIZE);
  if (!buf) return;
  memset(buf, 0, BIONIC_TLS_SIZE);
  void **tp = (void **)((char *)buf + BIONIC_TLS_TP_OFFSET);
  tp[0] = tp;                      /* TLS_SLOT_SELF */
  nx_set_tpidr_el0(tp);
}

int bionic_tls_present(void) { return nx_get_tpidr_el0() != NULL; }

uint64_t bk_time_ns(void) { return armTicksToNs(armGetSystemTick()); }
uint64_t bk_time_ms(void) { return bk_time_ns() / 1000000ull; }

int bk_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

long bk_file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

void bk_mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t n = strlen(tmp);
  if (n && tmp[n - 1] == '/') tmp[n - 1] = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/' && p[-1] != ':') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  mkdir(tmp, 0777);
}

void *bk_xmalloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p) fatal_error("Out of memory (%zu bytes).\n\nLaunch through title takeover (hold R while starting a game).", size);
  return p;
}

void *bk_xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) fatal_error("Out of memory (%zu x %zu bytes).\n\nLaunch through title takeover (hold R while starting a game).", n, size);
  return p;
}
