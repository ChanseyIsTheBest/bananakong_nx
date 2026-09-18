#ifndef BK_LOG_H
#define BK_LOG_H

#include <stdarg.h>
#include "config.h"

void bk_log_open(const char *path);
void bk_log_close(void);
void bk_log_write(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bk_log_vwrite(const char *tag, const char *fmt, va_list ap);

#if BK_LOG_ENABLE
#define LOGI(...) bk_log_write("nx", __VA_ARGS__)
#define LOGE(...) bk_log_write("nx:ERR", __VA_ARGS__)
#define LOGT(tag, ...) bk_log_write(tag, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGT(tag, ...) ((void)0)
#endif

/* Log a message once per call site (useful for per-frame stubs). */
#define LOG_ONCE(...) do { static int _once; if (!_once) { _once = 1; LOGI(__VA_ARGS__); } } while (0)

#endif
