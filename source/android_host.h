#ifndef BK_ANDROID_HOST_H
#define BK_ANDROID_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The NDK surface a NativeActivity game links against, reimplemented. Layouts
 * of ANativeActivity and its callback table match <android/native_activity.h>
 * because the game reads and writes them directly. */

typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct ALooper ALooper;
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AConfiguration AConfiguration;
typedef void ANativeWindow;   /* NWindow* underneath */
typedef struct ANativeActivity ANativeActivity;

typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

typedef struct ANativeActivityCallbacks {
  void  (*onStart)(ANativeActivity *);
  void  (*onResume)(ANativeActivity *);
  void *(*onSaveInstanceState)(ANativeActivity *, size_t *outSize);
  void  (*onPause)(ANativeActivity *);
  void  (*onStop)(ANativeActivity *);
  void  (*onDestroy)(ANativeActivity *);
  void  (*onWindowFocusChanged)(ANativeActivity *, int hasFocus);
  void  (*onNativeWindowCreated)(ANativeActivity *, ANativeWindow *);
  void  (*onNativeWindowResized)(ANativeActivity *, ANativeWindow *);
  void  (*onNativeWindowRedrawNeeded)(ANativeActivity *, ANativeWindow *);
  void  (*onNativeWindowDestroyed)(ANativeActivity *, ANativeWindow *);
  void  (*onInputQueueCreated)(ANativeActivity *, AInputQueue *);
  void  (*onInputQueueDestroyed)(ANativeActivity *, AInputQueue *);
  void  (*onContentRectChanged)(ANativeActivity *, const void *rect);
  void  (*onConfigurationChanged)(ANativeActivity *);
  void  (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;                 /* JavaVM* */
  void *env;                /* JNIEnv* */
  void *clazz;              /* jobject: the activity */
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  AAssetManager *assetManager;
  const char *obbPath;
};

/* Android constants used by the host. */
#define AKEY_EVENT_ACTION_DOWN          0
#define AKEY_EVENT_ACTION_UP            1
#define AMOTION_EVENT_ACTION_DOWN       0
#define AMOTION_EVENT_ACTION_UP         1
#define AMOTION_EVENT_ACTION_MOVE       2
#define AMOTION_EVENT_ACTION_POINTER_DOWN 5
#define AMOTION_EVENT_ACTION_POINTER_UP 6
#define AKEYCODE_BACK       4
#define AKEYCODE_DPAD_UP    19
#define AKEYCODE_DPAD_DOWN  20
#define AKEYCODE_DPAD_LEFT  21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_SPACE      62
#define AKEYCODE_ENTER      66
#define AKEYCODE_ESCAPE     111

#define HOST_MAX_POINTERS 10

void host_init(const char *internal_path, const char *assets_dir);
ANativeActivity *host_create_activity(void *vm, void *env, void *clazz);
ANativeWindow   *host_window(void);
/* switch-mesa can reset the NWindow's crop/transform while it builds its
 * swapchain inside eglCreateWindowSurface; re-assert them afterwards. */
void             host_window_reassert(void);
void             host_window_size(uint32_t *w, uint32_t *h);
AInputQueue     *host_input_queue(void);
int              host_finish_requested(void);

void host_inject_key(int action, int keycode);
/* action is a masked AMOTION_EVENT_ACTION_*; pointer_index says which entry
 * changed for POINTER_DOWN/POINTER_UP. Coordinates are window pixels. */
void host_inject_motion(int action, int pointer_index, int count,
                        const int *ids, const float *xs, const float *ys);

/* NDK entry points bound into the game (imports.c). */
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
void    AAsset_close(AAsset *a);
const void *AAsset_getBuffer(AAsset *a);
off_t   AAsset_getLength(AAsset *a);
int     AAsset_read(AAsset *a, void *buf, size_t count);
off_t   AAsset_seek(AAsset *a, off_t offset, int whence);

AConfiguration *AConfiguration_new(void);
void    AConfiguration_delete(AConfiguration *c);
void    AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *am);
int32_t AConfiguration_getMcc(AConfiguration *c);
int32_t AConfiguration_getMnc(AConfiguration *c);
void    AConfiguration_getLanguage(AConfiguration *c, char *out);
void    AConfiguration_getCountry(AConfiguration *c, char *out);
int32_t AConfiguration_getOrientation(AConfiguration *c);
int32_t AConfiguration_getTouchscreen(AConfiguration *c);
int32_t AConfiguration_getDensity(AConfiguration *c);
int32_t AConfiguration_getKeyboard(AConfiguration *c);
int32_t AConfiguration_getNavigation(AConfiguration *c);
int32_t AConfiguration_getKeysHidden(AConfiguration *c);
int32_t AConfiguration_getNavHidden(AConfiguration *c);
int32_t AConfiguration_getSdkVersion(AConfiguration *c);
int32_t AConfiguration_getScreenSize(AConfiguration *c);
int32_t AConfiguration_getScreenLong(AConfiguration *c);
int32_t AConfiguration_getUiModeType(AConfiguration *c);
int32_t AConfiguration_getUiModeNight(AConfiguration *c);
int32_t AConfiguration_getScreenWidthDp(AConfiguration *c);
int32_t AConfiguration_getScreenHeightDp(AConfiguration *c);
int32_t AConfiguration_getSmallestScreenWidthDp(AConfiguration *c);
int32_t AConfiguration_getLayoutDirection(AConfiguration *c);

int32_t AInputEvent_getType(const AInputEvent *e);
int32_t AInputEvent_getSource(const AInputEvent *e);
int32_t AInputEvent_getDeviceId(const AInputEvent *e);
int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **out);
int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e);
void    AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int handled);
void    AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int ident, ALooper_callbackFunc cb, void *data);
void    AInputQueue_detachLooper(AInputQueue *q);
int32_t AKeyEvent_getAction(const AInputEvent *e);
int32_t AKeyEvent_getKeyCode(const AInputEvent *e);
int32_t AKeyEvent_getMetaState(const AInputEvent *e);
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e);
int32_t AKeyEvent_getScanCode(const AInputEvent *e);
int32_t AKeyEvent_getFlags(const AInputEvent *e);
int64_t AKeyEvent_getDownTime(const AInputEvent *e);
int64_t AKeyEvent_getEventTime(const AInputEvent *e);
int32_t AMotionEvent_getAction(const AInputEvent *e);
size_t  AMotionEvent_getPointerCount(const AInputEvent *e);
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i);
float   AMotionEvent_getX(const AInputEvent *e, size_t i);
float   AMotionEvent_getY(const AInputEvent *e, size_t i);
float   AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t i);

ALooper *ALooper_prepare(int opts);
int      ALooper_addFd(ALooper *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data);
int      ALooper_removeFd(ALooper *l, int fd);
int      ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);

void    ANativeActivity_finish(ANativeActivity *a);
void    ANativeWindow_acquire(ANativeWindow *w);
void    ANativeWindow_release(ANativeWindow *w);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format);

void   *ASensorManager_getInstance(void);
void   *ASensorManager_getDefaultSensor(void *mgr, int type);
void   *ASensorManager_createEventQueue(void *mgr, ALooper *l, int ident, ALooper_callbackFunc cb, void *data);
int     ASensorManager_destroyEventQueue(void *mgr, void *q);
int     ASensorEventQueue_enableSensor(void *q, void *sensor);
int     ASensorEventQueue_disableSensor(void *q, void *sensor);
int     ASensorEventQueue_setEventRate(void *q, void *sensor, int32_t usec);
ssize_t ASensorEventQueue_getEvents(void *q, void *events, size_t count);

#endif
