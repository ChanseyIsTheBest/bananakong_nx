#ifndef BK_UTIL_H
#define BK_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Bionic's arm64 ABI keeps the stack-protector canary at TPIDR_EL0 + 0x28 and
 * every NDK-compiled function with a stack protector reads it in its prologue.
 * Horizon leaves TPIDR_EL0 to user code (libnx uses TPIDRRO_EL0, and this tree
 * is built with -mtp=soft), so each thread that runs game code gets a small
 * zeroed block and TPIDR_EL0 points into it. */
#define BIONIC_TLS_SIZE       0x400
#define BIONIC_TLS_TP_OFFSET  0x200

void  nx_set_tpidr_el0(void *p);   /* tls.s */
void *nx_get_tpidr_el0(void);      /* tls.s */

/* Install a TLS block for the calling thread. If buf is NULL one is
 * allocated (and intentionally never freed: threads may outlive bookkeeping). */
void  bionic_tls_install(void *buf);
int   bionic_tls_present(void);

uint64_t bk_time_ns(void);
uint64_t bk_time_ms(void);

/* Allocation that cannot fail silently: on exhaustion, report and stop. */
void *bk_xmalloc(size_t size);
void *bk_xcalloc(size_t n, size_t size);

int  bk_file_exists(const char *path);
long bk_file_size(const char *path);
void bk_mkdir_p(const char *path);

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))

#endif
