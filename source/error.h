#ifndef BK_ERROR_H
#define BK_ERROR_H

/* Before the engine owns the display: print on the libnx console and wait for
 * a button. Afterwards: raise the system error applet (works over EGL). */
void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void error_set_graphics_owned(int owned);

#endif
