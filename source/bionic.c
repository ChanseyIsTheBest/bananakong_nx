/* bionic.c -- bionic libc ABI shims for libBananaKong.so (Defold, arm64). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>

#include "app.h"
#include "bionic.h"
#include "error.h"
#include "fakefd.h"
#include "imports.h"
#include "log.h"
#include "so_util.h"
#include "util.h"

/* Bionic (arm64 Linux) constants that differ from newlib. */
#define B_O_ACCMODE   03
#define B_O_CREAT     0100
#define B_O_EXCL      0200
#define B_O_TRUNC     01000
#define B_O_APPEND    02000
#define B_PROT_EXEC   4
#define B_MAP_FIXED   0x10
#define B_MAP_ANON    0x20
#define B_MREMAP_MAYMOVE 1
#define B_EAFNOSUPPORT 97
#define B_ENETDOWN    100
#define B_ENOTTY      25
#define B_EAI_FAIL    4
#define B_MAP_FAILED  ((void *)-1)

/* open("/dev/urandom") hands out this descriptor; reads are filled by randomGet. */
#define BN_RANDOM_FD  (FAKE_FD_BASE + 0x1000)

static char g_abort_msg[512];

/* ---- init ----------------------------------------------------------------- */

static char g_ctype_table[257];
const char *bn__ctype_ = g_ctype_table;
unsigned char bn___sF[3 * BIONIC_FILE_SIZE];
static Mutex g_map_mu;

void bionic_init(void) {
  /* BSD ctype bits: _U 01 _L 02 _N 04 _S 010 _P 020 _C 040 _X 0100 _B 0200 */
  for (int c = 0; c < 128; c++) {
    int f = 0;
    if (isupper(c)) f |= 0x01;
    if (islower(c)) f |= 0x02;
    if (isdigit(c)) f |= 0x04;
    if (isspace(c)) f |= 0x08;
    if (ispunct(c)) f |= 0x10;
    if (iscntrl(c)) f |= 0x20;
    if (isxdigit(c)) f |= 0x40;
    if (c == ' ') f |= 0x80;
    g_ctype_table[1 + c] = (char)f;
  }
  mutexInit(&g_map_mu);
}

/* ---- fortify -------------------------------------------------------------- */

static void fortify_fail(const char *what) {
  fatal_error("FORTIFY: %s buffer overflow detected in game code.", what);
}

void *bn___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { if (n > dn) fortify_fail("memcpy"); return memcpy(d, s, n); }
void *bn___memmove_chk(void *d, const void *s, size_t n, size_t dn) { if (n > dn) fortify_fail("memmove"); return memmove(d, s, n); }
void *bn___memset_chk(void *d, int c, size_t n, size_t dn) { if (n > dn) fortify_fail("memset"); return memset(d, c, n); }
char *bn___strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
char *bn___strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
size_t bn___strlen_chk(const char *s, size_t n) {
  size_t l = strlen(s);
  if (l >= n) fortify_fail("strlen");
  return l;
}
int bn___vsnprintf_chk(char *buf, size_t len, int flags, size_t slen, const char *fmt, va_list ap) {
  (void)flags;
  if (len > slen) fortify_fail("vsnprintf");
  return vsnprintf(buf, len, fmt, ap);
}
int bn___vsprintf_chk(char *buf, int flags, size_t slen, const char *fmt, va_list ap) {
  (void)flags;
  int r = vsnprintf(buf, slen, fmt, ap);
  if (r >= 0 && (size_t)r >= slen) fortify_fail("vsprintf");
  return r;
}
long bn___read_chk(int fd, void *buf, size_t n, size_t bn) {
  if (n > bn) fortify_fail("read");
  return bn_read(fd, buf, n);
}
void bn___FD_SET_chk(int fd, void *set, size_t sz) { if (fd >= 0 && (size_t)fd < sz * 8) ((unsigned long *)set)[fd / 64] |= 1ul << (fd % 64); }
void bn___FD_CLR_chk(int fd, void *set, size_t sz) { if (fd >= 0 && (size_t)fd < sz * 8) ((unsigned long *)set)[fd / 64] &= ~(1ul << (fd % 64)); }
int  bn___FD_ISSET_chk(int fd, const void *set, size_t sz) {
  if (fd < 0 || (size_t)fd >= sz * 8) return 0;
  return (((const unsigned long *)set)[fd / 64] >> (fd % 64)) & 1;
}

/* ---- process / diagnostics ------------------------------------------------ */

void bn___assert2(const char *file, int line, const char *func, const char *expr) {
  fatal_error("Assertion failed in game code:\n%s\n\n%s:%d\n%s", expr ? expr : "?",
              file ? file : "?", line, func ? func : "?");
}

void bn___stack_chk_fail(void) { fatal_error("Stack corruption detected in game code (__stack_chk_fail)."); }

void bn_android_set_abort_message(const char *msg) {
  snprintf(g_abort_msg, sizeof(g_abort_msg), "%s", msg ? msg : "");
  LOGE("abort message: %s", g_abort_msg);
}

void bn_abort(void) {
  fatal_error("The game aborted.\n\n%s", g_abort_msg[0] ? g_abort_msg : "(no abort message)");
}

void bn_exit(int code) {
  LOGI("game called exit(%d)", code);
  if (app_is_main_thread()) exit(code);
  app_request_exit(code);
  for (;;) svcSleepThread(1000000000ll);
}
void bn__exit(int code) { bn_exit(code); }

int  bn___cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void bn___cxa_finalize(void *dso) { (void)dso; }

int *bn___errno(void) { return &errno; }
static int g_h_errno;
int *bn___get_h_errno(void) { return &g_h_errno; }

int bn___system_property_get(const char *name, char *value) {
  const char *v = "";
  if (!strcmp(name, "ro.build.version.sdk")) v = "30";
  else if (!strcmp(name, "ro.build.version.release")) v = "11";
  else if (!strcmp(name, "ro.product.manufacturer") || !strcmp(name, "ro.product.brand")) v = "Nintendo";
  else if (!strcmp(name, "ro.product.model")) v = "Switch";
  else if (!strcmp(name, "ro.product.device")) v = "nx";
  else if (!strcmp(name, "ro.product.cpu.abi")) v = "arm64-v8a";
  snprintf(value, 92, "%s", v);
  return (int)strlen(value);
}

unsigned long bn_getauxval(unsigned long type) {
  switch (type) {
    case 6:  return 0x1000;    /* AT_PAGESZ */
    case 16: return 0xFB;      /* AT_HWCAP: FP ASIMD AES PMULL SHA1 SHA2 CRC32 (Cortex-A57) */
    default: return 0;
  }
}

long bn_sysconf(int name) {
  switch (name) {
    case 0x0006: return 100;                          /* _SC_CLK_TCK */
    case 0x000b: return 1024;                         /* _SC_OPEN_MAX */
    case 0x0027: case 0x0028: return 0x1000;          /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 0x0060: case 0x0061: return 3;               /* _SC_NPROCESSORS_CONF / ONLN */
    case 0x0062: return (4LL << 30) / 0x1000;         /* _SC_PHYS_PAGES */
    case 0x0063: return (1LL << 30) / 0x1000;         /* _SC_AVPHYS_PAGES */
    default: LOGI("sysconf(0x%x) unknown", name); errno = EINVAL; return -1;
  }
}

long bn_syscall(long nr, ...) {
  va_list ap;
  va_start(ap, nr);
  long a0 = va_arg(ap, long), a1 = va_arg(ap, long), a2 = va_arg(ap, long);
  va_end(ap);
  switch (nr) {
    case 172: return 1;                                   /* getpid */
    case 178: return (long)threadGetCurHandle();          /* gettid */
    case 124: svcSleepThread(0); return 0;                /* sched_yield */
    case 278: randomGet((void *)a0, (size_t)a1); return a1; /* getrandom */
    case 98: {                                            /* futex */
      int op = (int)(a1 & 0x7f);
      if (op == 0 || op == 9) {
        if (*(int *)a0 != (int)a2) { errno = EAGAIN; return -1; }
        svcSleepThread(1000000ll);
        return 0;
      }
      if (op == 1 || op == 10) return 0;
      break;
    }
  }
  LOGI("syscall(%ld) unsupported", nr);
  errno = ENOSYS;
  return -1;
}

int bn_getpid(void) { return 1; }
int bn_gethostname(char *name, size_t len) { snprintf(name, len, "nintendo-switch"); return 0; }
void *bn_signal(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int bn_sigaction(int sig, const void *act, void *old) { (void)sig; (void)act; if (old) memset(old, 0, 32); return 0; }
int bn_sigemptyset(void *set) { if (set) memset(set, 0, 8); return 0; }
int bn_raise(int sig) { LOGI("raise(%d)", sig); if (sig == 6) bn_abort(); return 0; }
int bn_setitimer(int which, const void *nv, void *ov) { (void)which; (void)nv; if (ov) memset(ov, 0, 32); return 0; }
int bn_system(const char *cmd) { (void)cmd; return -1; }
FILE *bn_popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; errno = ENOSYS; return NULL; }
int bn_pclose(FILE *f) { (void)f; return -1; }
void bn_openlog(const char *ident, int opt, int fac) { (void)ident; (void)opt; (void)fac; }
void bn_closelog(void) {}
void bn_syslog(int prio, const char *fmt, ...) {
#if BK_LOG_ENABLE
  va_list ap; va_start(ap, fmt); bk_log_vwrite("syslog", fmt, ap); va_end(ap);
#else
  (void)fmt;
#endif
  (void)prio;
}

int bn_android_log_print(int prio, const char *tag, const char *fmt, ...) {
#if BK_LOG_ENABLE
  va_list ap; va_start(ap, fmt); bk_log_vwrite(tag, fmt, ap); va_end(ap);
#else
  (void)tag; (void)fmt;
#endif
  (void)prio;
  return 0;
}

/* ---- time / locale / math -------------------------------------------------- */

int bn_clock_gettime(int clk, struct timespec *ts) {
  if (clk == 0) return clock_gettime(CLOCK_REALTIME, ts);      /* CLOCK_REALTIME */
  uint64_t ns = bk_time_ns();                                       /* every monotonic flavour */
  ts->tv_sec = (time_t)(ns / 1000000000ull);
  ts->tv_nsec = (long)(ns % 1000000000ull);
  return 0;
}

typedef struct {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
} BionicTm;

static void tm_to_b(const struct tm *t, BionicTm *b) {
  b->tm_sec = t->tm_sec; b->tm_min = t->tm_min; b->tm_hour = t->tm_hour;
  b->tm_mday = t->tm_mday; b->tm_mon = t->tm_mon; b->tm_year = t->tm_year;
  b->tm_wday = t->tm_wday; b->tm_yday = t->tm_yday; b->tm_isdst = t->tm_isdst;
  b->tm_gmtoff = 0; b->tm_zone = "UTC";
}
static void b_to_tm(const BionicTm *b, struct tm *t) {
  memset(t, 0, sizeof(*t));
  t->tm_sec = b->tm_sec; t->tm_min = b->tm_min; t->tm_hour = b->tm_hour;
  t->tm_mday = b->tm_mday; t->tm_mon = b->tm_mon; t->tm_year = b->tm_year;
  t->tm_wday = b->tm_wday; t->tm_yday = b->tm_yday; t->tm_isdst = b->tm_isdst;
}

void *bn_localtime_r(const time_t *t, void *out) {
  struct tm tmp;
  if (!localtime_r(t, &tmp)) return NULL;
  tm_to_b(&tmp, out);
  return out;
}
void *bn_localtime(const time_t *t) { static BionicTm s; return bn_localtime_r(t, &s); }
void *bn_gmtime_r(const time_t *t, void *out) {
  struct tm tmp;
  if (!gmtime_r(t, &tmp)) return NULL;
  tm_to_b(&tmp, out);
  return out;
}
time_t bn_mktime(void *tmv) {
  BionicTm *b = tmv;
  struct tm t;
  b_to_tm(b, &t);
  time_t r = mktime(&t);
  tm_to_b(&t, b);
  return r;
}
size_t bn_strftime(char *s, size_t max, const char *fmt, const void *tmv) {
  struct tm t;
  b_to_tm(tmv, &t);
  return strftime(s, max, fmt, &t);
}

char *bn_setlocale(int cat, const char *loc) {
  int c;
  switch (cat) {
    case 0: c = LC_CTYPE; break;
    case 1: c = LC_NUMERIC; break;
    case 2: c = LC_TIME; break;
    case 3: c = LC_COLLATE; break;
    case 4: c = LC_MONETARY; break;
    case 5: c = LC_MESSAGES; break;
    case 6: c = LC_ALL; break;
    default: return NULL;
  }
  return setlocale(c, loc);
}

void bn_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void bn_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

int bn_strerror_r(int e, char *buf, size_t n) { snprintf(buf, n, "%s", strerror(e)); return 0; }

int bn_posix_memalign(void **out, size_t align, size_t size) {
  if (align < sizeof(void *)) align = sizeof(void *);
  void *p = memalign(align, size ? size : 1);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

/* ---- dynamic linker --------------------------------------------------------- */

static const char *g_dlerror;

void *bn_dlopen(const char *name, int flags) {
  (void)flags;
  if (!name) return (void *)1;
  if (strstr(name, "libBananaKong") || strstr(name, "libdmengine")) return (void *)2;
  LOGI("dlopen(%s) -> not available", name);
  g_dlerror = "library not available on this platform";
  return NULL;
}
void *bn_dlsym(void *handle, const char *sym) {
  (void)handle;
  void *p = so_resolve_external(sym);
  if (!p) p = (void *)imports_lookup(sym);
  if (!p) { g_dlerror = "symbol not found"; LOGI("dlsym(%s) -> NULL", sym); }
  return p;
}
int bn_dlclose(void *handle) { (void)handle; return 0; }
char *bn_dlerror(void) { const char *e = g_dlerror; g_dlerror = NULL; return (char *)e; }
int bn_dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }
int bn_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) { return so_dl_iterate_phdr(cb, data); }

/* ---- files and descriptors -------------------------------------------------- */

static void write_log_stream(int stream, const char *p, size_t n) {
#if BK_LOG_ENABLE
  if (n) bk_log_write(stream == 2 ? "stderr" : "stdout", "%.*s", (int)n, p);
#else
  (void)stream; (void)p; (void)n;
#endif
}

static int conv_open_flags(int f) {
  int o;
  switch (f & B_O_ACCMODE) {
    case 0: o = O_RDONLY; break;
    case 1: o = O_WRONLY; break;
    default: o = O_RDWR; break;
  }
  if (f & B_O_CREAT)  o |= O_CREAT;
  if (f & B_O_EXCL)   o |= O_EXCL;
  if (f & B_O_TRUNC)  o |= O_TRUNC;
  if (f & B_O_APPEND) o |= O_APPEND;
  return o;
}

int bn_open(const char *path, int flags, ...) {
  if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) return BN_RANDOM_FD;
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) { errno = ENOENT; return -1; }
  int mode = 0666;
  if (flags & B_O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int fd = open(path, conv_open_flags(flags), mode);
  if (fd < 0) LOGI("open(%s, 0%o) failed: %d", path, flags, errno);
  return fd;
}
int bn___open_2(const char *path, int flags) { return bn_open(path, flags); }

long bn_read(int fd, void *buf, size_t n) {
  if (fd == BN_RANDOM_FD) { randomGet(buf, n); return (long)n; }
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, n);
  if (fd == 0) return 0;
  return read(fd, buf, n);
}
long bn_write(int fd, const void *buf, size_t n) {
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, n);
  if (fd == 1 || fd == 2) { write_log_stream(fd, buf, n); return (long)n; }
  return write(fd, buf, n);
}
int bn_close(int fd) {
  if (fd == BN_RANDOM_FD) return 0;
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  if (fd >= 0 && fd <= 2) return 0;
  return close(fd);
}
long bn_lseek64(int fd, long off, int whence) {
  if (fd == BN_RANDOM_FD) return 0;
  if (fakefd_is_fake(fd)) { errno = ESPIPE; return -1; }
  return (long)lseek(fd, (off_t)off, whence);
}

typedef struct {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, pad1;
  int64_t  st_size;
  int32_t  st_blksize, pad2;
  int64_t  st_blocks;
  int64_t  st_atime_s, st_atime_ns, st_mtime_s, st_mtime_ns, st_ctime_s, st_ctime_ns;
  uint32_t unused4, unused5;
} BionicStat;  /* 128 bytes */

static void stat_to_b(const struct stat *s, BionicStat *b) {
  memset(b, 0, sizeof(*b));
  b->st_dev = (uint64_t)s->st_dev;
  b->st_ino = (uint64_t)s->st_ino;
  b->st_mode = (uint32_t)s->st_mode;
  b->st_nlink = (uint32_t)s->st_nlink;
  b->st_size = (int64_t)s->st_size;
  b->st_blksize = 4096;
  b->st_blocks = ((int64_t)s->st_size + 511) / 512;
  b->st_atime_s = (int64_t)s->st_atime;
  b->st_mtime_s = (int64_t)s->st_mtime;
  b->st_ctime_s = (int64_t)s->st_ctime;
}

int bn_fstat(int fd, void *st) {
  BionicStat *b = st;
  if (fd == BN_RANDOM_FD) { memset(b, 0, sizeof(*b)); b->st_mode = 0020000 | 0444; return 0; }
  if (fakefd_is_fake(fd) || (fd >= 0 && fd <= 2)) {
    memset(b, 0, sizeof(*b));
    b->st_mode = 0010000 | 0666;   /* S_IFIFO */
    return 0;
  }
  struct stat s;
  if (fstat(fd, &s) != 0) return -1;
  stat_to_b(&s, b);
  return 0;
}

int bn_stat(const char *path, void *st) {
  struct stat s;
  if (stat(path, &s) != 0) return -1;
  stat_to_b(&s, st);
  return 0;
}

int bn_mkdir(const char *path, unsigned mode) { return mkdir(path, (mode_t)mode); }
int bn_unlink(const char *path) { return unlink(path); }
int bn_remove(const char *path) { return remove(path); }

/* POSIX rename replaces the target atomically; the Switch filesystem refuses
 * to rename onto an existing file. Defold's sys.save writes a temp file and
 * renames it over the old save, so without this every save after the first
 * would fail. Keep a backup until the new file is in place. */
int bn_rename(const char *from, const char *to) {
  if (rename(from, to) == 0) return 0;
  int err = errno;
  struct stat st;
  if (stat(from, &st) != 0 || stat(to, &st) != 0 || S_ISDIR(st.st_mode)) { errno = err; return -1; }
  char bak[1024];
  snprintf(bak, sizeof(bak), "%s.nxbak", to);
  unlink(bak);
  if (rename(to, bak) != 0) { errno = err; return -1; }
  if (rename(from, to) != 0) {
    err = errno;
    rename(bak, to);
    errno = err;
    return -1;
  }
  unlink(bak);
  return 0;
}

int bn_fcntl(int fd, int cmd, ...) {
  long arg = 0;
  if (cmd == 2 || cmd == 4) {       /* F_SETFD / F_SETFL take an argument */
    va_list ap;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
  }
  /* The engine sets O_NONBLOCK (bionic 0x800) on its command pipe. */
  if (fakefd_is_fake(fd)) {
    if (cmd == 3) return 02 | (fakefd_get_nonblock(fd) ? 0x800 : 0);   /* F_GETFL */
    if (cmd == 4) fakefd_set_nonblock(fd, (arg & 0x800) != 0);          /* F_SETFL */
    return 0;
  }
  if (cmd == 3) return 02;
  return 0;
}
int bn_ioctl(int fd, int req, ...) { (void)fd; (void)req; errno = B_ENOTTY; return -1; }
int bn_pipe(int fds[2]) { return fakefd_pipe(fds); }

typedef struct { int fd; short events; short revents; } BPollFd;

int bn_poll(void *fdsv, unsigned long n, int timeout) {
  BPollFd *fds = fdsv;
  uint64_t deadline = timeout < 0 ? UINT64_MAX : bk_time_ms() + (uint64_t)timeout;
  for (;;) {
    int ready = 0;
    for (unsigned long i = 0; i < n; i++) {
      fds[i].revents = 0;
      int r = 0, w = 0;
      if (fds[i].fd == BN_RANDOM_FD) {
        fds[i].revents = fds[i].events & 0x1;
      } else if (fakefd_is_fake(fds[i].fd) && fakefd_poll_state(fds[i].fd, &r, &w) == 0) {
        if (r && (fds[i].events & 0x1)) fds[i].revents |= 0x1;   /* POLLIN */
        if (w && (fds[i].events & 0x4)) fds[i].revents |= 0x4;   /* POLLOUT */
      } else if (fds[i].fd >= 0) {
        fds[i].revents = 0x20;                                  /* POLLNVAL */
      }
      if (fds[i].revents) ready++;
    }
    if (ready || timeout == 0 || bk_time_ms() >= deadline) return ready;
    svcSleepThread(2000000ll);
  }
}

int bn_select(int nfds, void *r, void *w, void *e, void *tv) {
  size_t words = (size_t)((nfds > 1024 ? 1024 : (nfds < 0 ? 0 : nfds)) + 63) / 64;
  if (r) memset(r, 0, words * 8);
  if (w) memset(w, 0, words * 8);
  if (e) memset(e, 0, words * 8);
  if (tv) {
    const int64_t *t = tv;
    int64_t ms = t[0] * 1000 + t[1] / 1000;
    if (ms > 100) ms = 100;
    if (ms > 0) svcSleepThread(ms * 1000000ll);
  }
  return 0;
}

int bn_mkstemp(char *tmpl) { return mkstemp(tmpl); }

/* ---- stdio ------------------------------------------------------------------ */

static int sf_index(FILE *f) {
  uintptr_t p = (uintptr_t)f, b = (uintptr_t)bn___sF;
  if (p >= b && p < b + sizeof(bn___sF)) return (int)((p - b) / BIONIC_FILE_SIZE);
  return -1;
}

static const char k_meminfo[] =
  "MemTotal:        3906256 kB\n"
  "MemFree:         2097152 kB\n"
  "MemAvailable:    2621440 kB\n"
  "Buffers:               0 kB\n"
  "Cached:                0 kB\n";

static char g_rand_buf[4096];

FILE *bn_fopen(const char *path, const char *mode) {
  if (!path || !mode) { errno = EINVAL; return NULL; }
  char m[8];
  size_t k = 0;
  for (const char *c = mode; *c && k < sizeof(m) - 1; c++)
    if (*c == 'r' || *c == 'w' || *c == 'a' || *c == '+' || *c == 'b') m[k++] = *c;
  m[k] = 0;

  if (!strcmp(path, "/proc/meminfo"))
    return fmemopen((void *)k_meminfo, sizeof(k_meminfo) - 1, "r");
  if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
    randomGet(g_rand_buf, sizeof(g_rand_buf));
    return fmemopen(g_rand_buf, sizeof(g_rand_buf), "r");
  }
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5) || !strncmp(path, "/dev/", 5)) {
    errno = ENOENT;
    return NULL;
  }
  FILE *f = fopen(path, m);
  if (!f) LOGI("fopen(%s, %s) failed: %d", path, mode, errno);
  return f;
}

FILE *bn_fdopen(int fd, const char *mode) {
  if (fakefd_is_fake(fd) || fd == BN_RANDOM_FD || (fd >= 0 && fd <= 2)) { errno = EINVAL; return NULL; }
  return fdopen(fd, mode);
}

FILE *bn_tmpfile(void) {
  static unsigned counter;
  char path[512];
  snprintf(path, sizeof(path), "%s/tmp_%08x_%u", app_files_dir(), (unsigned)bk_time_ms(), counter++);
  return fopen(path, "w+b");
}

int bn_fclose(FILE *f) { return sf_index(f) >= 0 ? 0 : fclose(f); }

size_t bn_fread(void *p, size_t sz, size_t n, FILE *f) { return sf_index(f) >= 0 ? 0 : fread(p, sz, n, f); }

size_t bn_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  int i = sf_index(f);
  if (i >= 0) { write_log_stream(i, p, sz * n); return n; }
  return fwrite(p, sz, n, f);
}

int  bn_fseek(FILE *f, long off, int whence)  { if (sf_index(f) >= 0) { errno = ESPIPE; return -1; } return fseek(f, off, whence); }
int  bn_fseeko(FILE *f, long off, int whence) { return bn_fseek(f, off, whence); }
long bn_ftell(FILE *f)  { if (sf_index(f) >= 0) { errno = ESPIPE; return -1; } return ftell(f); }
long bn_ftello(FILE *f) { return bn_ftell(f); }
int  bn_fflush(FILE *f) { if (f && sf_index(f) >= 0) return 0; return fflush(f); }
int  bn_feof(FILE *f)   { int i = sf_index(f); if (i >= 0) return i == 0; return feof(f); }
int  bn_ferror(FILE *f) { return sf_index(f) >= 0 ? 0 : ferror(f); }
void bn_clearerr(FILE *f) { if (sf_index(f) < 0) clearerr(f); }
int  bn_fileno(FILE *f) { int i = sf_index(f); return i >= 0 ? i : fileno(f); }
int  bn_fgetc(FILE *f)  { return sf_index(f) >= 0 ? EOF : fgetc(f); }
int  bn_getc(FILE *f)   { return bn_fgetc(f); }
int  bn_ungetc(int c, FILE *f) { return sf_index(f) >= 0 ? EOF : ungetc(c, f); }
char *bn_fgets(char *s, int n, FILE *f) { return sf_index(f) >= 0 ? NULL : fgets(s, n, f); }

int bn_fputc(int c, FILE *f) {
  int i = sf_index(f);
  if (i >= 0) { char ch = (char)c; if (ch != '\n') write_log_stream(i, &ch, 1); return c & 0xff; }
  return fputc(c, f);
}
int bn_fputs(const char *s, FILE *f) {
  int i = sf_index(f);
  if (i >= 0) { write_log_stream(i, s, strlen(s)); return 1; }
  return fputs(s, f);
}
int bn_vfprintf(FILE *f, const char *fmt, va_list ap) {
  int i = sf_index(f);
  if (i >= 0) {
    char buf[1024];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    write_log_stream(i, buf, strlen(buf));
    return r;
  }
  return vfprintf(f, fmt, ap);
}
int bn_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = bn_vfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}
int bn_fscanf(FILE *f, const char *fmt, ...) {
  if (sf_index(f) >= 0) return EOF;
  va_list ap; va_start(ap, fmt);
  int r = vfscanf(f, fmt, ap);
  va_end(ap);
  return r;
}
int  bn_setvbuf(FILE *f, char *buf, int mode, size_t size) { return sf_index(f) >= 0 ? 0 : setvbuf(f, buf, mode, size); }
void bn_setbuf(FILE *f, char *buf) { if (sf_index(f) < 0) setbuf(f, buf); }
int  bn_putchar(int c) { return c & 0xff; }
int  bn_puts(const char *s) { write_log_stream(1, s, strlen(s)); return 1; }
int  bn_vprintf(const char *fmt, va_list ap) { return bn_vfprintf((FILE *)(bn___sF + BIONIC_FILE_SIZE), fmt, ap); }

/* ---- memory mapping ----------------------------------------------------------
 * LuaJIT's allocator is the main client (anonymous RW chunks, partial unmaps
 * from dlmalloc's trimming, mremap growth). Executable mappings are refused:
 * the JIT is switched off at load time (luajit_guard.c), and anything that
 * still asks for PROT_EXEC gets a clean failure instead of a later jump into
 * non-executable memory. */

typedef struct { void *alloc; uintptr_t start, end; } MapRec;
static MapRec *g_maps;
static int g_nmaps, g_capmaps;

static int map_find(uintptr_t a) {
  for (int i = 0; i < g_nmaps; i++)
    if (a >= g_maps[i].start && a < g_maps[i].end) return i;
  return -1;
}
static void map_remove(int i) { g_maps[i] = g_maps[--g_nmaps]; }

void *bn_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  if (!len) { errno = EINVAL; return B_MAP_FAILED; }
  if (prot & B_PROT_EXEC) {
    LOGE("mmap: refusing PROT_EXEC mapping of %zu bytes", len);
    errno = EACCES;
    return B_MAP_FAILED;
  }
  size_t sz = ALIGN_UP(len, 0x1000);
  if (addr && (flags & B_MAP_FIXED)) {
    mutexLock(&g_map_mu);
    int i = map_find((uintptr_t)addr);
    int ok = i >= 0 && (uintptr_t)addr + sz <= g_maps[i].end;
    mutexUnlock(&g_map_mu);
    if (!ok) { errno = EINVAL; return B_MAP_FAILED; }
    memset(addr, 0, sz);
    return addr;
  }
  void *p = memalign(0x1000, sz);
  if (!p) { errno = ENOMEM; return B_MAP_FAILED; }
  memset(p, 0, sz);
  if (!(flags & B_MAP_ANON) && fd >= 0) {
    if (fakefd_is_fake(fd) || fd == BN_RANDOM_FD) { free(p); errno = ENODEV; return B_MAP_FAILED; }
    off_t save = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, (off_t)off, SEEK_SET) >= 0) {
      size_t got = 0;
      while (got < len) {
        ssize_t r = read(fd, (char *)p + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
    }
    lseek(fd, save, SEEK_SET);
  }
  mutexLock(&g_map_mu);
  if (g_nmaps == g_capmaps) {
    g_capmaps = g_capmaps ? g_capmaps * 2 : 64;
    g_maps = realloc(g_maps, (size_t)g_capmaps * sizeof(MapRec));
  }
  g_maps[g_nmaps].alloc = p;
  g_maps[g_nmaps].start = (uintptr_t)p;
  g_maps[g_nmaps].end = (uintptr_t)p + sz;
  g_nmaps++;
  mutexUnlock(&g_map_mu);
  return p;
}

int bn_munmap(void *addr, size_t len) {
  uintptr_t s = (uintptr_t)addr, e = s + ALIGN_UP(len, 0x1000);
  mutexLock(&g_map_mu);
  for (int i = 0; i < g_nmaps;) {
    MapRec *m = &g_maps[i];
    if (e <= m->start || s >= m->end) { i++; continue; }
    if (s <= m->start && e >= m->end) { free(m->alloc); map_remove(i); continue; }
    if (s <= m->start) m->start = e;           /* head trimmed */
    else if (e >= m->end) m->end = s;          /* tail trimmed */
    i++;                                       /* hole in the middle: keep */
  }
  mutexUnlock(&g_map_mu);
  return 0;
}

void *bn_mremap(void *old, size_t oldlen, size_t newlen, int flags, ...) {
  (void)oldlen;
  size_t nsz = ALIGN_UP(newlen, 0x1000);
  mutexLock(&g_map_mu);
  int i = map_find((uintptr_t)old);
  if (i < 0 || g_maps[i].start != (uintptr_t)old) { mutexUnlock(&g_map_mu); errno = EINVAL; return B_MAP_FAILED; }
  MapRec *m = &g_maps[i];
  size_t cur = m->end - m->start;
  if (nsz <= cur) { m->end = m->start + nsz; mutexUnlock(&g_map_mu); return old; }
  uintptr_t cap = (uintptr_t)m->alloc + malloc_usable_size(m->alloc);
  if (m->start + nsz <= cap) { m->end = m->start + nsz; mutexUnlock(&g_map_mu); return old; }
  if (!(flags & B_MREMAP_MAYMOVE)) { mutexUnlock(&g_map_mu); errno = ENOMEM; return B_MAP_FAILED; }
  void *p = memalign(0x1000, nsz);
  if (!p) { mutexUnlock(&g_map_mu); errno = ENOMEM; return B_MAP_FAILED; }
  memcpy(p, old, cur);
  memset((char *)p + cur, 0, nsz - cur);
  free(m->alloc);
  m->alloc = p;
  m->start = (uintptr_t)p;
  m->end = (uintptr_t)p + nsz;
  mutexUnlock(&g_map_mu);
  return p;
}

int bn_mprotect(void *addr, size_t len, int prot) {
  if (prot & B_PROT_EXEC) {
    LOGE("mprotect: refusing PROT_EXEC on %p+%zu", addr, len);
    errno = EACCES;
    return -1;
  }
  return 0;
}

/* ---- network: offline ------------------------------------------------------- */

int bn_socket(int d, int t, int p) { (void)d; (void)t; (void)p; errno = B_ENETDOWN; return -1; }
int bn_net_fail(void) { errno = 9 /* EBADF */; return -1; }
int bn_getaddrinfo(const char *node, const char *svc, const void *hints, void **res) {
  (void)svc; (void)hints;
  LOGI("getaddrinfo(%s): offline", node ? node : "(null)");
  if (res) *res = NULL;
  return B_EAI_FAIL;
}
void bn_freeaddrinfo(void *res) { (void)res; }
const char *bn_gai_strerror(int e) { (void)e; return "Network unavailable"; }
int bn_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hl, char *serv, unsigned sl, int flags) {
  (void)sa; (void)salen; (void)host; (void)hl; (void)serv; (void)sl; (void)flags;
  return B_EAI_FAIL;
}
void *bn_gethostbyname(const char *name) { (void)name; g_h_errno = 1; return NULL; }
void *bn_gethostbyaddr(const void *addr, unsigned len, int type) { (void)addr; (void)len; (void)type; g_h_errno = 1; return NULL; }
const char *bn_hstrerror(int e) { (void)e; return "Unknown host"; }

int bn_inet_aton(const char *cp, uint32_t *out) {
  unsigned a, b, c, d;
  char tail;
  if (sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
  if (out) *out = (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));   /* network byte order */
  return 1;
}
char *bn_inet_ntoa(uint32_t addr) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff);
  return buf;
}
const char *bn_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  if (af == 2) {
    const uint8_t *a = src;
    snprintf(dst, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    return dst;
  }
  if (af == 10) {
    const uint8_t *a = src;
    size_t o = 0;
    for (int i = 0; i < 16 && o < size; i += 2)
      o += (size_t)snprintf(dst + o, size - o, i ? ":%x" : "%x", (a[i] << 8) | a[i + 1]);
    return dst;
  }
  errno = B_EAFNOSUPPORT;
  return NULL;
}
int bn_inet_pton(int af, const char *src, void *dst) {
  if (af == 2) {
    uint32_t v;
    if (!bn_inet_aton(src, &v)) return 0;
    memcpy(dst, &v, 4);
    return 1;
  }
  if (af == 10) return 0;
  errno = B_EAFNOSUPPORT;
  return -1;
}
