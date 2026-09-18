/* pthread_bionic.c -- see pthread_bionic.h. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "log.h"
#include "pthread_bionic.h"
#include "util.h"

/* ---- lazily created objects ---------------------------------------------- */

#define LZ_TAG      0x5A000000u
#define LZ_TAG_MASK 0xFF000000u
#define LZ_IDX_MASK 0x00FFFFFFu
#define LZ_CHUNK    4096
#define LZ_CHUNKS   512

static void  **g_lz_chunks[LZ_CHUNKS];
static uint32_t g_lz_next = 1;
static uint32_t *g_lz_free;
static size_t   g_lz_nfree, g_lz_capfree;
static Mutex    g_lz_mu;

static uint32_t lz_register(void *obj) {
  mutexLock(&g_lz_mu);
  uint32_t idx;
  if (g_lz_nfree) idx = g_lz_free[--g_lz_nfree];
  else idx = g_lz_next++;
  uint32_t c = idx / LZ_CHUNK;
  if (c >= LZ_CHUNKS) { mutexUnlock(&g_lz_mu); LOGE("pthread object table full"); abort(); }
  if (!g_lz_chunks[c]) g_lz_chunks[c] = bk_xcalloc(LZ_CHUNK, sizeof(void *));
  g_lz_chunks[c][idx % LZ_CHUNK] = obj;
  mutexUnlock(&g_lz_mu);
  return idx;
}

static void lz_unregister(uint32_t idx) {
  mutexLock(&g_lz_mu);
  g_lz_chunks[idx / LZ_CHUNK][idx % LZ_CHUNK] = NULL;
  if (g_lz_nfree == g_lz_capfree) {
    g_lz_capfree = g_lz_capfree ? g_lz_capfree * 2 : 256;
    g_lz_free = realloc(g_lz_free, g_lz_capfree * sizeof(uint32_t));
  }
  g_lz_free[g_lz_nfree++] = idx;
  mutexUnlock(&g_lz_mu);
}

static inline void *lz_lookup(uint32_t idx) {
  void **chunk = g_lz_chunks[idx / LZ_CHUNK];
  return chunk ? chunk[idx % LZ_CHUNK] : NULL;
}

typedef void *(*lz_create_fn)(uint32_t init_word);

static void *lz_get(void *storage, lz_create_fn create) {
  uint32_t *slot = (uint32_t *)storage;
  for (;;) {
    uint32_t w = *slot;
    if ((w & LZ_TAG_MASK) == LZ_TAG) {
      void *o = lz_lookup(w & LZ_IDX_MASK);
      if (o) return o;
    }
    void *obj = create(w);
    uint32_t idx = lz_register(obj);
    if (__sync_bool_compare_and_swap(slot, w, LZ_TAG | idx)) return obj;
    lz_unregister(idx);
    free(obj);
  }
}

/* ---- mutex ---------------------------------------------------------------- */

enum { BM_NORMAL = 0, BM_RECURSIVE = 1, BM_ERRORCHECK = 2 };

/* Recursion is tracked here (owner handle + depth) rather than with libnx's
 * RMutex, whose internal fields changed between libnx releases and would have
 * to be touched by pthread_cond_wait. */
typedef struct { int type; Mutex m; Handle owner; u32 count; } BMutex;

static void *mutex_create_type(int type) {
  BMutex *b = bk_xcalloc(1, sizeof(*b));
  b->type = type;
  mutexInit(&b->m);
  return b;
}
static void *mutex_create(uint32_t w) {
  int type = BM_NORMAL;
  if ((w & 0xC000) == 0x4000) type = BM_RECURSIVE;
  return mutex_create_type(type);
}

int bn_pthread_mutexattr_init(void *attr)    { if (attr) *(int *)attr = 0; return 0; }
int bn_pthread_mutexattr_destroy(void *attr) { (void)attr; return 0; }
int bn_pthread_mutexattr_settype(void *attr, int type) {
  if (!attr || type < 0 || type > 2) return EINVAL;
  *(int *)attr = type;
  return 0;
}

int bn_pthread_mutex_init(void *m, const void *attr) {
  int type = attr ? *(const int *)attr : BM_NORMAL;
  BMutex *b = mutex_create_type(type == BM_RECURSIVE ? BM_RECURSIVE : BM_NORMAL);
  *(uint32_t *)m = LZ_TAG | lz_register(b);
  return 0;
}

int bn_pthread_mutex_destroy(void *m) {
  uint32_t w = *(uint32_t *)m;
  if ((w & LZ_TAG_MASK) == LZ_TAG) {
    void *o = lz_lookup(w & LZ_IDX_MASK);
    if (o) { lz_unregister(w & LZ_IDX_MASK); free(o); }
  }
  *(uint32_t *)m = 0;
  return 0;
}

int bn_pthread_mutex_lock(void *m) {
  BMutex *b = lz_get(m, mutex_create);
  if (b->type != BM_RECURSIVE) { mutexLock(&b->m); return 0; }
  const Handle self = threadGetCurHandle();
  if (b->owner == self) { b->count++; return 0; }
  mutexLock(&b->m);
  b->owner = self;
  b->count = 1;
  return 0;
}

int bn_pthread_mutex_trylock(void *m) {
  BMutex *b = lz_get(m, mutex_create);
  if (b->type != BM_RECURSIVE) return mutexTryLock(&b->m) ? 0 : EBUSY;
  const Handle self = threadGetCurHandle();
  if (b->owner == self) { b->count++; return 0; }
  if (!mutexTryLock(&b->m)) return EBUSY;
  b->owner = self;
  b->count = 1;
  return 0;
}

int bn_pthread_mutex_unlock(void *m) {
  BMutex *b = lz_get(m, mutex_create);
  if (b->type != BM_RECURSIVE) { mutexUnlock(&b->m); return 0; }
  if (b->owner != threadGetCurHandle() || b->count == 0) return EPERM;
  if (--b->count == 0) {
    b->owner = 0;
    mutexUnlock(&b->m);
  }
  return 0;
}

/* ---- condition variables -------------------------------------------------- */

typedef struct { CondVar cv; } BCond;

static void *cond_create(uint32_t w) {
  (void)w;
  BCond *c = bk_xcalloc(1, sizeof(*c));
  condvarInit(&c->cv);
  return c;
}

int bn_pthread_cond_init(void *c, const void *attr) {
  (void)attr;
  BCond *b = cond_create(0);
  *(uint32_t *)c = LZ_TAG | lz_register(b);
  return 0;
}

int bn_pthread_cond_destroy(void *c) {
  uint32_t w = *(uint32_t *)c;
  if ((w & LZ_TAG_MASK) == LZ_TAG) {
    void *o = lz_lookup(w & LZ_IDX_MASK);
    if (o) { lz_unregister(w & LZ_IDX_MASK); free(o); }
  }
  *(uint32_t *)c = 0;
  return 0;
}

int bn_pthread_cond_wait(void *c, void *m) {
  BCond *bc = lz_get(c, cond_create);
  BMutex *bm = lz_get(m, mutex_create);
  if (bm->type == BM_RECURSIVE) {
    /* Release all recursion levels for the wait, restore them afterwards. */
    const Handle self = threadGetCurHandle();
    u32 saved = bm->count;
    bm->count = 0;
    bm->owner = 0;
    condvarWait(&bc->cv, &bm->m);
    bm->owner = self;
    bm->count = saved;
  } else {
    condvarWait(&bc->cv, &bm->m);
  }
  return 0;
}

int bn_pthread_cond_signal(void *c)    { condvarWakeOne(&((BCond *)lz_get(c, cond_create))->cv); return 0; }
int bn_pthread_cond_broadcast(void *c) { condvarWakeAll(&((BCond *)lz_get(c, cond_create))->cv); return 0; }

/* ---- rwlock --------------------------------------------------------------- */

typedef struct { RwLock rw; } BRwLock;

static void *rwlock_create(uint32_t w) {
  (void)w;
  BRwLock *r = bk_xcalloc(1, sizeof(*r));
  rwlockInit(&r->rw);
  return r;
}

int bn_pthread_rwlock_rdlock(void *rw) { rwlockReadLock(&((BRwLock *)lz_get(rw, rwlock_create))->rw); return 0; }
int bn_pthread_rwlock_wrlock(void *rw) { rwlockWriteLock(&((BRwLock *)lz_get(rw, rwlock_create))->rw); return 0; }
int bn_pthread_rwlock_unlock(void *rw) {
  BRwLock *r = lz_get(rw, rwlock_create);
  if (rwlockIsWriteLockHeldByCurrentThread(&r->rw)) rwlockWriteUnlock(&r->rw);
  else rwlockReadUnlock(&r->rw);
  return 0;
}

/* ---- once ----------------------------------------------------------------- */

int bn_pthread_once(int *once, void (*fn)(void)) {
  for (;;) {
    int v = *once;
    if (v == 2) return 0;
    if (v == 0) {
      if (__sync_bool_compare_and_swap(once, 0, 1)) {
        fn();
        *once = 2;
        return 0;
      }
      continue;
    }
    svcSleepThread(1000000ll);
  }
}

/* ---- keys: 128 bionic keys multiplexed over one real key ------------------ */

#define MAX_KEYS 128
static pthread_key_t g_master_key;
static int           g_key_used[MAX_KEYS];
static void        (*g_key_dtor[MAX_KEYS])(void *);
static Mutex         g_key_mu;

static void master_dtor(void *p) {
  void **vals = p;
  if (!vals) return;
  for (int round = 0; round < 4; round++) {
    int again = 0;
    for (int i = 0; i < MAX_KEYS; i++) {
      if (vals[i] && g_key_dtor[i]) {
        void *v = vals[i];
        vals[i] = NULL;
        g_key_dtor[i](v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(vals);
}

static void **key_values(int create) {
  void **vals = pthread_getspecific(g_master_key);
  if (!vals && create) {
    vals = bk_xcalloc(MAX_KEYS, sizeof(void *));
    pthread_setspecific(g_master_key, vals);
  }
  return vals;
}

int bn_pthread_key_create(int *key, void (*dtor)(void *)) {
  mutexLock(&g_key_mu);
  for (int i = 0; i < MAX_KEYS; i++) {
    if (!g_key_used[i]) {
      g_key_used[i] = 1;
      g_key_dtor[i] = dtor;
      mutexUnlock(&g_key_mu);
      *key = i;
      return 0;
    }
  }
  mutexUnlock(&g_key_mu);
  LOGE("pthread_key_create: out of keys");
  return EAGAIN;
}

int bn_pthread_key_delete(int key) {
  if (key < 0 || key >= MAX_KEYS) return EINVAL;
  mutexLock(&g_key_mu);
  g_key_used[key] = 0;
  g_key_dtor[key] = NULL;
  mutexUnlock(&g_key_mu);
  return 0;
}

void *bn_pthread_getspecific(int key) {
  if (key < 0 || key >= MAX_KEYS) return NULL;
  void **vals = key_values(0);
  return vals ? vals[key] : NULL;
}

int bn_pthread_setspecific(int key, const void *value) {
  if (key < 0 || key >= MAX_KEYS) return EINVAL;
  void **vals = key_values(1);
  if (!vals) return ENOMEM;
  vals[key] = (void *)value;
  return 0;
}

/* ---- attributes ----------------------------------------------------------- */

#define ATTR_MAGIC 0x42415454u  /* 'BATT' */
typedef struct { uint32_t magic; int32_t detached; uint64_t stacksize; } BAttr;  /* fits bionic's 56 bytes */

int bn_pthread_attr_init(void *attr) {
  BAttr *a = attr;
  a->magic = ATTR_MAGIC; a->detached = 0; a->stacksize = 0;
  return 0;
}
int bn_pthread_attr_destroy(void *attr) { (void)attr; return 0; }
int bn_pthread_attr_setstacksize(void *attr, size_t size) { ((BAttr *)attr)->stacksize = size; return 0; }
int bn_pthread_attr_getstacksize(const void *attr, size_t *size) {
  const BAttr *a = attr;
  *size = (a->magic == ATTR_MAGIC && a->stacksize) ? a->stacksize : 1024 * 1024;
  return 0;
}
int bn_pthread_attr_setdetachstate(void *attr, int state) { ((BAttr *)attr)->detached = (state == 1); return 0; }

int bn_pthread_getattr_np(unsigned long thread, void *attr) {
  (void)thread;
  BAttr *a = attr;
  a->magic = ATTR_MAGIC; a->detached = 0; a->stacksize = 1024 * 1024;
  return 0;
}

int bn_pthread_setname_np(unsigned long thread, const char *name) {
  (void)thread;
  LOGI("thread name: %s", name ? name : "(null)");
  return 0;
}

/* ---- threads -------------------------------------------------------------- */

#define MAX_THREADS 64
static Handle g_thread_handles[MAX_THREADS];
static int    g_nthreads;
static Mutex  g_thread_mu;

typedef struct { void *(*fn)(void *); void *arg; int index; } Tramp;

static void *thread_trampoline(void *p) {
  Tramp t = *(Tramp *)p;
  free(p);
  bionic_tls_install(NULL);
  Handle h = threadGetCurHandle();
  mutexLock(&g_thread_mu);
  if (g_nthreads < MAX_THREADS) g_thread_handles[g_nthreads++] = h;
  mutexUnlock(&g_thread_mu);
  /* libnx starts every pthread on the default core. Spread game threads over
   * cores 0-2: the first one created (the native_app_glue thread, i.e. the
   * engine's main loop) lands on core 1, away from the host input thread. */
  int core = (t.index + 1) % 3;
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
  return t.fn(t.arg);
}

int bn_pthread_create(void *out, const void *attr, void *(*fn)(void *), void *arg) {
  static int counter;
  const BAttr *a = attr;
  int detached = 0;
  size_t stack = 4 * 1024 * 1024;
  if (a && a->magic == ATTR_MAGIC) {
    detached = a->detached;
    if (a->stacksize) stack = a->stacksize < 512 * 1024 ? 512 * 1024 : a->stacksize;
  }
  Tramp *t = malloc(sizeof(*t));
  if (!t) return EAGAIN;
  t->fn = fn; t->arg = arg; t->index = __sync_fetch_and_add(&counter, 1);

  pthread_attr_t na;
  pthread_attr_init(&na);
  pthread_attr_setstacksize(&na, stack);
  pthread_t th;
  int r = pthread_create(&th, &na, thread_trampoline, t);
  pthread_attr_destroy(&na);
  if (r != 0) { free(t); LOGE("pthread_create failed: %d", r); return r; }
  *(unsigned long *)out = (unsigned long)th;
  if (detached) pthread_detach(th);
  return 0;
}

unsigned long bn_pthread_self(void) { return (unsigned long)pthread_self(); }
int bn_pthread_join(unsigned long thread, void **ret) { return pthread_join((pthread_t)thread, ret); }
int bn_pthread_detach(unsigned long thread) { return pthread_detach((pthread_t)thread); }

void threads_pause_all(void) {
  mutexLock(&g_thread_mu);
  for (int i = 0; i < g_nthreads; i++) svcSetThreadActivity(g_thread_handles[i], 1);
  mutexUnlock(&g_thread_mu);
}

void threads_init(void) {
  mutexInit(&g_lz_mu);
  mutexInit(&g_key_mu);
  mutexInit(&g_thread_mu);
  pthread_key_create(&g_master_key, master_dtor);
}
