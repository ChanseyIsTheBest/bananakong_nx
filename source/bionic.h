#ifndef BK_BIONIC_H
#define BK_BIONIC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Bionic (Android libc) ABI shims. Everything the game imports from libc that
 * differs in layout or constants from newlib goes through a bn_ function; the
 * rest is bound straight to newlib in imports.c. */

void bionic_init(void);

#define BIONIC_FILE_SIZE 152
extern unsigned char bn___sF[3 * BIONIC_FILE_SIZE];
extern const char *bn__ctype_;

/* fortify */
void *bn___memcpy_chk(void *d, const void *s, size_t n, size_t dn);
void *bn___memmove_chk(void *d, const void *s, size_t n, size_t dn);
void *bn___memset_chk(void *d, int c, size_t n, size_t dn);
char *bn___strchr_chk(const char *s, int c, size_t n);
char *bn___strrchr_chk(const char *s, int c, size_t n);
size_t bn___strlen_chk(const char *s, size_t n);
int bn___vsnprintf_chk(char *buf, size_t len, int flags, size_t slen, const char *fmt, va_list ap);
int bn___vsprintf_chk(char *buf, int flags, size_t slen, const char *fmt, va_list ap);
long bn___read_chk(int fd, void *buf, size_t n, size_t bn);
void bn___FD_SET_chk(int fd, void *set, size_t sz);
void bn___FD_CLR_chk(int fd, void *set, size_t sz);
int bn___FD_ISSET_chk(int fd, const void *set, size_t sz);

/* process / diagnostics */
void bn___assert2(const char *file, int line, const char *func, const char *expr);
void bn___stack_chk_fail(void);
void bn_abort(void);
void bn_exit(int code);
void bn__exit(int code);
void bn_android_set_abort_message(const char *msg);
int  bn___cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void bn___cxa_finalize(void *dso);
int *bn___errno(void);
int *bn___get_h_errno(void);
int  bn___system_property_get(const char *name, char *value);
unsigned long bn_getauxval(unsigned long type);
long bn_sysconf(int name);
long bn_syscall(long nr, ...);
int  bn_getpid(void);
int  bn_gethostname(char *name, size_t len);
void *bn_signal(int sig, void *handler);
int  bn_sigaction(int sig, const void *act, void *old);
int  bn_sigemptyset(void *set);
int  bn_raise(int sig);
int  bn_setitimer(int which, const void *nv, void *ov);
int  bn_system(const char *cmd);
FILE *bn_popen(const char *cmd, const char *mode);
int  bn_pclose(FILE *f);
void bn_openlog(const char *ident, int opt, int fac);
void bn_closelog(void);
void bn_syslog(int prio, const char *fmt, ...);
int  bn_android_log_print(int prio, const char *tag, const char *fmt, ...);

/* time / locale / math */
int  bn_clock_gettime(int clk, struct timespec *ts);
void *bn_localtime(const time_t *t);
void *bn_localtime_r(const time_t *t, void *out);
void *bn_gmtime_r(const time_t *t, void *out);
time_t bn_mktime(void *tm);
size_t bn_strftime(char *s, size_t max, const char *fmt, const void *tm);
char *bn_setlocale(int cat, const char *loc);
void bn_sincos(double x, double *s, double *c);
void bn_sincosf(float x, float *s, float *c);
int  bn_strerror_r(int e, char *buf, size_t n);
int  bn_posix_memalign(void **out, size_t align, size_t size);

/* dynamic linker */
void *bn_dlopen(const char *name, int flags);
void *bn_dlsym(void *handle, const char *sym);
int   bn_dlclose(void *handle);
char *bn_dlerror(void);
int   bn_dladdr(const void *addr, void *info);
int   bn_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data);

/* files and descriptors */
int  bn_open(const char *path, int flags, ...);
int  bn___open_2(const char *path, int flags);
long bn_read(int fd, void *buf, size_t n);
long bn_write(int fd, const void *buf, size_t n);
int  bn_close(int fd);
long bn_lseek64(int fd, long off, int whence);
int  bn_fstat(int fd, void *st);
int  bn_stat(const char *path, void *st);
int  bn_mkdir(const char *path, unsigned mode);
int  bn_unlink(const char *path);
int  bn_remove(const char *path);
int  bn_rename(const char *from, const char *to);
int  bn_fcntl(int fd, int cmd, ...);
int  bn_ioctl(int fd, int req, ...);
int  bn_pipe(int fds[2]);
int  bn_poll(void *fds, unsigned long n, int timeout);
int  bn_select(int nfds, void *r, void *w, void *e, void *tv);
int  bn_mkstemp(char *tmpl);

/* stdio */
FILE  *bn_fopen(const char *path, const char *mode);
FILE  *bn_fdopen(int fd, const char *mode);
FILE  *bn_tmpfile(void);
int    bn_fclose(FILE *f);
size_t bn_fread(void *p, size_t sz, size_t n, FILE *f);
size_t bn_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    bn_fseek(FILE *f, long off, int whence);
int    bn_fseeko(FILE *f, long off, int whence);
long   bn_ftell(FILE *f);
long   bn_ftello(FILE *f);
int    bn_fflush(FILE *f);
int    bn_feof(FILE *f);
int    bn_ferror(FILE *f);
void   bn_clearerr(FILE *f);
int    bn_fileno(FILE *f);
int    bn_fgetc(FILE *f);
int    bn_getc(FILE *f);
int    bn_ungetc(int c, FILE *f);
char  *bn_fgets(char *s, int n, FILE *f);
int    bn_fputc(int c, FILE *f);
int    bn_fputs(const char *s, FILE *f);
int    bn_fprintf(FILE *f, const char *fmt, ...);
int    bn_vfprintf(FILE *f, const char *fmt, va_list ap);
int    bn_fscanf(FILE *f, const char *fmt, ...);
int    bn_setvbuf(FILE *f, char *buf, int mode, size_t size);
void   bn_setbuf(FILE *f, char *buf);
int    bn_putchar(int c);
int    bn_puts(const char *s);
int    bn_vprintf(const char *fmt, va_list ap);

/* memory mapping */
void *bn_mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int   bn_munmap(void *addr, size_t len);
void *bn_mremap(void *old, size_t oldlen, size_t newlen, int flags, ...);
int   bn_mprotect(void *addr, size_t len, int prot);

/* network (all offline) */
int  bn_socket(int d, int t, int p);
int  bn_net_fail(void);
int  bn_getaddrinfo(const char *node, const char *svc, const void *hints, void **res);
void bn_freeaddrinfo(void *res);
const char *bn_gai_strerror(int e);
int  bn_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hl, char *serv, unsigned sl, int flags);
void *bn_gethostbyname(const char *name);
void *bn_gethostbyaddr(const void *addr, unsigned len, int type);
const char *bn_hstrerror(int e);
int  bn_inet_aton(const char *cp, uint32_t *out);
char *bn_inet_ntoa(uint32_t addr);
const char *bn_inet_ntop(int af, const void *src, char *dst, unsigned size);
int  bn_inet_pton(int af, const char *src, void *dst);

#endif
