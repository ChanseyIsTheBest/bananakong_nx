/* android_host.c -- NativeActivity host: ALooper, AInputQueue, AAsset,
 * AConfiguration, ANativeWindow, ASensor.
 *
 * Banana Kong (Defold) links android_native_app_glue. Its glue thread calls
 * ALooper_prepare + ALooper_addFd(msgread, LOOPER_ID_MAIN) and then polls,
 * while the "UI thread" (our main thread) writes APP_CMD_* bytes into the pipe
 * and blocks until the glue thread has consumed them. So the looper has to
 * report fake-pipe readiness and input-queue readiness for real, and wake up
 * promptly when either changes. One global condvar does that: every pipe write
 * and every injected event bumps it; pollOnce rescans after each wake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "android_host.h"
#include "app.h"
#include "config.h"
#include "fakefd.h"
#include "gl_diag.h"
#include "settings.h"
#include "log.h"
#include "util.h"

#define LOOPER_MAX_ENTRIES 16
#define MAX_LOOPERS 16
#define ALOOPER_POLL_WAKE     -1
#define ALOOPER_POLL_CALLBACK -2
#define ALOOPER_POLL_TIMEOUT  -3
#define ALOOPER_POLL_ERROR    -4
#define ALOOPER_EVENT_INPUT    1

/* ---- events --------------------------------------------------------------- */

struct AInputEvent {
  int32_t type, source, device_id;
  int32_t action, keycode, meta, repeat, scancode, flags;
  int64_t down_time, event_time;
  int32_t count;
  int32_t ids[HOST_MAX_POINTERS];
  float xs[HOST_MAX_POINTERS], ys[HOST_MAX_POINTERS];
  struct AInputEvent *next;
};

#define EVENT_POOL 512
struct AInputQueue {
  Mutex mu;
  AInputEvent pool[EVENT_POOL];
  AInputEvent *free_list, *head, *tail;
  int pending;
  ALooper *looper;
};

typedef struct {
  int fd;                /* -1 for the input queue entry */
  int ident, events;
  ALooper_callbackFunc cb;
  void *data;
  AInputQueue *queue;
} LooperEntry;

struct ALooper {
  Handle owner;
  LooperEntry e[LOOPER_MAX_ENTRIES];
  int n, rr;
};

struct AAssetManager { char root[512]; };
struct AAsset { FILE *f; off_t len; void *buf; Mutex mu; };
struct AConfiguration { char lang[3], country[3]; };

static Mutex    g_poll_mu;
static CondVar  g_poll_cv;
static ALooper  g_loopers[MAX_LOOPERS];
static int      g_nloopers;
static AInputQueue g_queue;
static AAssetManager g_assets;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static char g_internal_path[512];
static int  g_finish;
static int64_t g_key_down_time[256];

static void looper_notify(void) {
  mutexLock(&g_poll_mu);
  condvarWakeAll(&g_poll_cv);
  mutexUnlock(&g_poll_mu);
}

void host_init(const char *internal_path, const char *assets_dir) {
  mutexInit(&g_poll_mu);
  condvarInit(&g_poll_cv);
  fakefd_set_notify(looper_notify);
  snprintf(g_internal_path, sizeof(g_internal_path), "%s", internal_path);
  snprintf(g_assets.root, sizeof(g_assets.root), "%s", assets_dir);
  mutexInit(&g_queue.mu);
  for (int i = 0; i < EVENT_POOL; i++) {
    g_queue.pool[i].next = g_queue.free_list;
    g_queue.free_list = &g_queue.pool[i];
  }
}

ANativeActivity *host_create_activity(void *vm, void *env, void *clazz) {
  memset(&g_activity, 0, sizeof(g_activity));
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = vm;
  g_activity.env = env;
  g_activity.clazz = clazz;
  g_activity.internalDataPath = g_internal_path;
  g_activity.externalDataPath = g_internal_path;
  g_activity.sdkVersion = BK_SDK_INT;
  g_activity.assetManager = &g_assets;
  g_activity.obbPath = g_internal_path;
  return &g_activity;
}

static u32 g_win_w = BK_RENDER_W, g_win_h = BK_RENDER_H;

ANativeWindow *host_window(void) {
  NWindow *w = nwindowGetDefault();
  if (!strcmp(g_settings.resolution, "720p")) { g_win_w = BK_PANEL_W; g_win_h = BK_PANEL_H; }
  /* Dimensions allocate the buffer; the crop pins the presented region to
   * exactly that size (a width-aligned buffer would otherwise scan garbage
   * columns); the identity transform keeps the landscape buffer on a
   * landscape panel; the swap interval lets the compositor pace us. */
  nwindowSetDimensions(w, g_win_w, g_win_h);
  nwindowSetCrop(w, 0, 0, (s32)g_win_w, (s32)g_win_h);
  nwindowSetTransform(w, 0u);
  nwindowSetSwapInterval(w, 1);
  LOGI("window: %ux%u, %s", g_win_w, g_win_h,
       appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
  gl_diag_nwindow("after set_geom");
  return w;
}

void host_window_size(uint32_t *w, uint32_t *h) {
  if (w) *w = g_win_w;
  if (h) *h = g_win_h;
}

void host_window_reassert(void) {
  NWindow *w = nwindowGetDefault();
  nwindowSetCrop(w, 0, 0, (s32)g_win_w, (s32)g_win_h);
  nwindowSetTransform(w, 0u);
}

AInputQueue *host_input_queue(void) { return &g_queue; }
int host_finish_requested(void) { return g_finish; }

/* ---- injection (host input thread) ----------------------------------------- */

static AInputEvent *event_alloc(void) {
  AInputEvent *e = g_queue.free_list;
  if (e) { g_queue.free_list = e->next; memset(e, 0, sizeof(*e)); }
  return e;
}

static void event_push(AInputEvent *e) {
  e->next = NULL;
  if (g_queue.tail) g_queue.tail->next = e; else g_queue.head = e;
  g_queue.tail = e;
  g_queue.pending++;
}

void host_inject_key(int action, int keycode) {
  int64_t now = (int64_t)bk_time_ns();
  mutexLock(&g_queue.mu);
  AInputEvent *e = event_alloc();
  if (!e) { mutexUnlock(&g_queue.mu); return; }
  e->type = 1;                 /* AINPUT_EVENT_TYPE_KEY */
  e->source = 0x101;           /* AINPUT_SOURCE_KEYBOARD */
  e->device_id = 1;
  e->action = action;
  e->keycode = keycode;
  if (keycode >= 0 && keycode < 256) {
    if (action == AKEY_EVENT_ACTION_DOWN) g_key_down_time[keycode] = now;
    e->down_time = g_key_down_time[keycode];
  } else {
    e->down_time = now;
  }
  e->event_time = now;
  event_push(e);
  mutexUnlock(&g_queue.mu);
  looper_notify();
}

void host_inject_motion(int action, int pointer_index, int count,
                        const int *ids, const float *xs, const float *ys) {
  if (count <= 0) return;
  if (count > HOST_MAX_POINTERS) count = HOST_MAX_POINTERS;
  int64_t now = (int64_t)bk_time_ns();
  mutexLock(&g_queue.mu);
  AInputEvent *t = g_queue.tail;
  if (action == AMOTION_EVENT_ACTION_MOVE && t && t->type == 2 && (t->action & 0xff) == AMOTION_EVENT_ACTION_MOVE && t->count == count) {
    /* The engine has not drained the previous MOVE yet: update it in place. */
    for (int i = 0; i < count; i++) { t->ids[i] = ids[i]; t->xs[i] = xs[i]; t->ys[i] = ys[i]; }
    t->event_time = now;
    mutexUnlock(&g_queue.mu);
    return;
  }
  AInputEvent *e = event_alloc();
  if (!e) { mutexUnlock(&g_queue.mu); return; }
  e->type = 2;                 /* AINPUT_EVENT_TYPE_MOTION */
  e->source = 0x1002;          /* AINPUT_SOURCE_TOUCHSCREEN */
  e->device_id = 2;
  e->action = action | (pointer_index << 8);
  e->count = count;
  for (int i = 0; i < count; i++) { e->ids[i] = ids[i]; e->xs[i] = xs[i]; e->ys[i] = ys[i]; }
  e->down_time = e->event_time = now;
  event_push(e);
  mutexUnlock(&g_queue.mu);
  looper_notify();
}

/* ---- AInputQueue / AInputEvent ------------------------------------------------ */

int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **out) {
  mutexLock(&q->mu);
  AInputEvent *e = q->head;
  if (e) {
    q->head = e->next;
    if (!q->head) q->tail = NULL;
    q->pending--;
  }
  mutexUnlock(&q->mu);
  if (!e) return -1;
  *out = e;
  return 0;
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e) { (void)q; (void)e; return 0; }

void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int handled) {
  (void)handled;
  if (!e) return;
  mutexLock(&q->mu);
  e->next = q->free_list;
  q->free_list = e;
  mutexUnlock(&q->mu);
}

int32_t AInputEvent_getType(const AInputEvent *e)     { return e->type; }
int32_t AInputEvent_getSource(const AInputEvent *e)   { return e->source; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e->device_id; }
int32_t AKeyEvent_getAction(const AInputEvent *e)     { return e->action; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e)    { return e->keycode; }
int32_t AKeyEvent_getMetaState(const AInputEvent *e)  { return e->meta; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e){ return e->repeat; }
int32_t AKeyEvent_getScanCode(const AInputEvent *e)   { return e->scancode; }
int32_t AKeyEvent_getFlags(const AInputEvent *e)      { return e->flags; }
int64_t AKeyEvent_getDownTime(const AInputEvent *e)   { return e->down_time; }
int64_t AKeyEvent_getEventTime(const AInputEvent *e)  { return e->event_time; }
int32_t AMotionEvent_getAction(const AInputEvent *e)  { return e->action; }
size_t  AMotionEvent_getPointerCount(const AInputEvent *e) { return (size_t)e->count; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) { return i < (size_t)e->count ? e->ids[i] : 0; }
float   AMotionEvent_getX(const AInputEvent *e, size_t i) { return i < (size_t)e->count ? e->xs[i] : 0.0f; }
float   AMotionEvent_getY(const AInputEvent *e, size_t i) { return i < (size_t)e->count ? e->ys[i] : 0.0f; }
float   AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t i) {
  switch (axis) {
    case 0: return AMotionEvent_getX(e, i);
    case 1: return AMotionEvent_getY(e, i);
    case 2: return 1.0f;    /* PRESSURE */
    case 3: return 0.05f;   /* SIZE */
    default: return 0.0f;
  }
}

/* ---- ALooper ------------------------------------------------------------------ */

static ALooper *looper_for(Handle h, int create) {
  for (int i = 0; i < g_nloopers; i++)
    if (g_loopers[i].owner == h) return &g_loopers[i];
  if (!create || g_nloopers >= MAX_LOOPERS) return NULL;
  ALooper *l = &g_loopers[g_nloopers++];
  memset(l, 0, sizeof(*l));
  l->owner = h;
  return l;
}

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  mutexLock(&g_poll_mu);
  ALooper *l = looper_for(threadGetCurHandle(), 1);
  mutexUnlock(&g_poll_mu);
  return l;
}

static int looper_add(ALooper *l, const LooperEntry *en) {
  for (int i = 0; i < l->n; i++) {
    if ((en->fd >= 0 && l->e[i].fd == en->fd) || (en->queue && l->e[i].queue == en->queue)) {
      l->e[i] = *en;
      return 1;
    }
  }
  if (l->n >= LOOPER_MAX_ENTRIES) return -1;
  l->e[l->n++] = *en;
  return 1;
}

int ALooper_addFd(ALooper *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data) {
  if (!l) return -1;
  LooperEntry en = { fd, cb ? -2 : ident, events, cb, data, NULL };
  mutexLock(&g_poll_mu);
  int r = looper_add(l, &en);
  condvarWakeAll(&g_poll_cv);
  mutexUnlock(&g_poll_mu);
  return r;
}

int ALooper_removeFd(ALooper *l, int fd) {
  if (!l) return -1;
  int removed = 0;
  mutexLock(&g_poll_mu);
  for (int i = 0; i < l->n; i++) {
    if (l->e[i].fd == fd && !l->e[i].queue) { l->e[i] = l->e[--l->n]; removed = 1; break; }
  }
  mutexUnlock(&g_poll_mu);
  return removed;
}

void AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int ident, ALooper_callbackFunc cb, void *data) {
  if (!l) return;
  LooperEntry en = { -1, cb ? -2 : ident, ALOOPER_EVENT_INPUT, cb, data, q };
  mutexLock(&g_poll_mu);
  looper_add(l, &en);
  q->looper = l;
  condvarWakeAll(&g_poll_cv);
  mutexUnlock(&g_poll_mu);
}

void AInputQueue_detachLooper(AInputQueue *q) {
  mutexLock(&g_poll_mu);
  ALooper *l = q->looper;
  if (l) {
    for (int i = 0; i < l->n; i++)
      if (l->e[i].queue == q) { l->e[i] = l->e[--l->n]; break; }
  }
  q->looper = NULL;
  mutexUnlock(&g_poll_mu);
}

static int entry_ready(const LooperEntry *en) {
  if (en->queue) return en->queue->pending > 0;
  int r = 0, w = 0;
  if (fakefd_poll_state(en->fd, &r, &w) != 0) return 0;
  return ((en->events & ALOOPER_EVENT_INPUT) && r) || ((en->events & 2) && w);
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  mutexLock(&g_poll_mu);
  ALooper *l = looper_for(threadGetCurHandle(), 0);
  if (!l) { mutexUnlock(&g_poll_mu); return ALOOPER_POLL_ERROR; }
  uint64_t deadline = timeoutMillis < 0 ? UINT64_MAX : bk_time_ns() + (uint64_t)timeoutMillis * 1000000ull;
  int called_back = 0;
  for (;;) {
    int n = l->n;
    for (int k = 0; k < n; k++) {
      int i = (l->rr + k) % n;
      if (!entry_ready(&l->e[i])) continue;
      LooperEntry en = l->e[i];
      if (en.cb) {
        mutexUnlock(&g_poll_mu);
        int keep = en.cb(en.fd, ALOOPER_EVENT_INPUT, en.data);
        mutexLock(&g_poll_mu);
        if (!keep) {
          for (int j = 0; j < l->n; j++)
            if (l->e[j].fd == en.fd && l->e[j].queue == en.queue) { l->e[j] = l->e[--l->n]; break; }
        }
        called_back = 1;
        n = l->n;
        continue;
      }
      l->rr = i + 1;
      mutexUnlock(&g_poll_mu);
      if (outFd) *outFd = en.fd;
      if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
      if (outData) *outData = en.data;
      return en.ident;
    }
    if (called_back) { mutexUnlock(&g_poll_mu); return ALOOPER_POLL_CALLBACK; }
    uint64_t now = bk_time_ns();
    if (timeoutMillis == 0 || now >= deadline) { mutexUnlock(&g_poll_mu); return ALOOPER_POLL_TIMEOUT; }
    uint64_t wait = deadline - now;
    if (wait > 50000000ull) wait = 50000000ull;   /* rescan at least every 50 ms */
    condvarWaitTimeout(&g_poll_cv, &g_poll_mu, wait);
  }
}

/* ---- activity / window ---------------------------------------------------------- */

void ANativeActivity_finish(ANativeActivity *a) {
  (void)a;
  LOGI("ANativeActivity_finish");
  g_finish = 1;
  app_request_exit(0);
}

void    ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
void    ANativeWindow_release(ANativeWindow *w) { (void)w; }
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format) {
  (void)w;
  LOGI("ANativeWindow_setBuffersGeometry(%d, %d, fmt %d)", width, height, format);
  return 0;
}

/* ---- assets ------------------------------------------------------------------------ */

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
  (void)mode;
  if (!mgr || !filename) return NULL;
  while (*filename == '/') filename++;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", mgr->root, filename);
  FILE *f = fopen(path, "rb");
  if (!f) { LOGI("AAssetManager_open(%s): not found", filename); return NULL; }
  AAsset *a = bk_xcalloc(1, sizeof(*a));
  setvbuf(f, NULL, _IOFBF, 64 * 1024);   /* the archive is read in many small pieces */
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = (off_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  mutexInit(&a->mu);
  LOGI("AAssetManager_open(%s) -> %lld bytes", filename, (long long)a->len);
  return a;
}

void AAsset_close(AAsset *a) {
  if (!a) return;
  fclose(a->f);
  free(a->buf);
  free(a);
}

const void *AAsset_getBuffer(AAsset *a) {
  if (!a) return NULL;
  mutexLock(&a->mu);
  if (!a->buf) {
    void *b = malloc(a->len ? (size_t)a->len : 1);
    if (b) {
      long pos = ftell(a->f);
      fseek(a->f, 0, SEEK_SET);
      size_t got = fread(b, 1, (size_t)a->len, a->f);
      fseek(a->f, pos, SEEK_SET);
      if (got != (size_t)a->len) { free(b); b = NULL; }
    }
    a->buf = b;
  }
  mutexUnlock(&a->mu);
  return a->buf;
}

off_t AAsset_getLength(AAsset *a) { return a ? a->len : 0; }

int AAsset_read(AAsset *a, void *buf, size_t count) {
  if (!a) return -1;
  mutexLock(&a->mu);
  size_t got = fread(buf, 1, count, a->f);
  int err = ferror(a->f);
  mutexUnlock(&a->mu);
  return (got == 0 && err) ? -1 : (int)got;
}

off_t AAsset_seek(AAsset *a, off_t offset, int whence) {
  if (!a) return -1;
  mutexLock(&a->mu);
  off_t r = -1;
  if (fseek(a->f, (long)offset, whence) == 0) r = (off_t)ftell(a->f);
  mutexUnlock(&a->mu);
  return r;
}

/* ---- configuration ------------------------------------------------------------------- */

AConfiguration *AConfiguration_new(void) {
  AConfiguration *c = bk_xcalloc(1, sizeof(*c));
  memcpy(c->lang, "en", 3);
  memcpy(c->country, "US", 3);
  return c;
}
void    AConfiguration_delete(AConfiguration *c) { free(c); }
void    AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *am) { (void)c; (void)am; }
int32_t AConfiguration_getMcc(AConfiguration *c) { (void)c; return 0; }
int32_t AConfiguration_getMnc(AConfiguration *c) { (void)c; return 0; }
void    AConfiguration_getLanguage(AConfiguration *c, char *out) { out[0] = c->lang[0]; out[1] = c->lang[1]; }
void    AConfiguration_getCountry(AConfiguration *c, char *out)  { out[0] = c->country[0]; out[1] = c->country[1]; }
int32_t AConfiguration_getOrientation(AConfiguration *c) { (void)c; return 2; }  /* LAND */
int32_t AConfiguration_getTouchscreen(AConfiguration *c) { (void)c; return 3; }  /* FINGER */
int32_t AConfiguration_getDensity(AConfiguration *c) { (void)c; return 240; }
int32_t AConfiguration_getKeyboard(AConfiguration *c) { (void)c; return 1; }     /* NOKEYS */
int32_t AConfiguration_getNavigation(AConfiguration *c) { (void)c; return 1; }   /* NONAV */
int32_t AConfiguration_getKeysHidden(AConfiguration *c) { (void)c; return 2; }   /* YES */
int32_t AConfiguration_getNavHidden(AConfiguration *c) { (void)c; return 2; }
int32_t AConfiguration_getSdkVersion(AConfiguration *c) { (void)c; return BK_SDK_INT; }
int32_t AConfiguration_getScreenSize(AConfiguration *c) { (void)c; return 3; }   /* LARGE */
int32_t AConfiguration_getScreenLong(AConfiguration *c) { (void)c; return 2; }   /* YES */
int32_t AConfiguration_getUiModeType(AConfiguration *c) { (void)c; return 1; }   /* NORMAL */
int32_t AConfiguration_getUiModeNight(AConfiguration *c) { (void)c; return 1; }  /* NO */
int32_t AConfiguration_getScreenWidthDp(AConfiguration *c) { (void)c; return 1280; }
int32_t AConfiguration_getScreenHeightDp(AConfiguration *c) { (void)c; return 720; }
int32_t AConfiguration_getSmallestScreenWidthDp(AConfiguration *c) { (void)c; return 720; }
int32_t AConfiguration_getLayoutDirection(AConfiguration *c) { (void)c; return 0; }

/* ---- sensors: none ------------------------------------------------------------------- */

static int g_sensor_dummy;
void   *ASensorManager_getInstance(void) { return &g_sensor_dummy; }
void   *ASensorManager_getDefaultSensor(void *mgr, int type) { (void)mgr; (void)type; return NULL; }
void   *ASensorManager_createEventQueue(void *mgr, ALooper *l, int ident, ALooper_callbackFunc cb, void *data) {
  (void)mgr; (void)l; (void)ident; (void)cb; (void)data;
  return &g_sensor_dummy;
}
int     ASensorManager_destroyEventQueue(void *mgr, void *q) { (void)mgr; (void)q; return 0; }
int     ASensorEventQueue_enableSensor(void *q, void *sensor) { (void)q; return sensor ? 0 : -1; }
int     ASensorEventQueue_disableSensor(void *q, void *sensor) { (void)q; (void)sensor; return 0; }
int     ASensorEventQueue_setEventRate(void *q, void *sensor, int32_t usec) { (void)q; (void)sensor; (void)usec; return 0; }
ssize_t ASensorEventQueue_getEvents(void *q, void *events, size_t count) { (void)q; (void)events; (void)count; return 0; }
