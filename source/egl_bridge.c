/* egl_bridge.c -- the few EGL/GL entry points that need more than a straight
 * pass-through to switch-mesa. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "config.h"
#include "cursor.h"
#include "egl_bridge.h"
#include "error.h"
#include "android_host.h"
#include "gl_diag.h"
#include "log.h"
#include "settings.h"

static EGLint g_surf_w = BK_RENDER_W, g_surf_h = BK_RENDER_H;
static unsigned long g_frames;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;

EGLBoolean egl_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  EGLBoolean r = eglInitialize(dpy, major, minor);
  eglBindAPI(EGL_OPENGL_ES_API);   /* Android's implicit default; be explicit for mesa */
  LOGI("eglInitialize -> %d", r);
  return r;
}

EGLBoolean egl_ChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num) {
#if BK_LOG_ENABLE
  for (const EGLint *a = attribs; a && *a != EGL_NONE; a += 2) LOGT("egl", "config attrib 0x%x = %d", a[0], a[1]);
#endif
  EGLBoolean r = eglChooseConfig(dpy, attribs, configs, size, num);
  LOGI("eglChooseConfig -> %d (%d configs)", r, num ? *num : -1);
  return r;
}

EGLSurface egl_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attribs) {
  EGLSurface s = eglCreateWindowSurface(dpy, cfg, win, attribs);
  if (s != EGL_NO_SURFACE) {
    egl_QuerySurface(dpy, s, EGL_WIDTH, &g_surf_w);
    egl_QuerySurface(dpy, s, EGL_HEIGHT, &g_surf_h);
  }
  LOGI("eglCreateWindowSurface(win %p) -> %p (%dx%d, err 0x%x)", (void *)win, (void *)s, g_surf_w, g_surf_h, eglGetError());
  host_window_reassert();
  gl_diag_nwindow("after eglCreateWindowSurface");
  if (s == EGL_NO_SURFACE)
    LOGE("no window surface: the engine has nothing to draw into (an NWindow backs only one EGLSurface)");
  g_surface = s;
  return s;
}

EGLContext egl_CreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attribs) {
#if BK_LOG_ENABLE
  for (const EGLint *a = attribs; a && *a != EGL_NONE; a += 2) LOGT("egl", "context attrib 0x%x = %d", a[0], a[1]);
#endif
  EGLContext c = eglCreateContext(dpy, cfg, share, attribs);
  LOGI("eglCreateContext(share %p) -> %p", (void *)share, (void *)c);
  if (share == EGL_NO_CONTEXT && c != EGL_NO_CONTEXT) g_context = c;
  return c;
}

EGLBoolean egl_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  cursor_draw(g_surf_w, g_surf_h);
  gl_diag_frame();
  EGLBoolean ok = eglSwapBuffers(dpy, surface);
  if (g_frames++ == 0) LOGI("first frame presented (surface %p, ok %d)", (void *)surface, ok);
  if (!ok) LOGE("eglSwapBuffers(%p) failed: 0x%x", (void *)surface, eglGetError());
  return ok;
}

/* switch-mesa reports a freshly created window surface as 0x0: it only learns
 * the geometry once a buffer has been dequeued. Defold asks straight after
 * eglCreateWindowSurface, concluded its window was zero-sized ("window size
 * changed from 1920x1080 to 0x0"), set a 0x0 viewport and stopped issuing draw
 * calls -- a black screen with the engine, sound and input all running. Fill
 * in the size the window was actually configured with whenever the driver
 * does not know it yet; a real answer from the driver always wins. */
EGLBoolean egl_QuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  EGLBoolean ok = eglQuerySurface(dpy, surface, attribute, value);
  if (value && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) && (!ok || *value <= 0)) {
    uint32_t w = 0, h = 0;
    host_window_size(&w, &h);
    *value = (EGLint)(attribute == EGL_WIDTH ? w : h);
    LOG_ONCE("eglQuerySurface reported no size; using the configured window size instead");
    ok = EGL_TRUE;
  }
  return ok;
}

EGLBoolean egl_MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  EGLBoolean ok = eglMakeCurrent(dpy, draw, read, ctx);
  LOGI("eglMakeCurrent(draw %p, read %p, ctx %p) -> %d%s", (void *)draw, (void *)read, (void *)ctx, ok,
       (ctx == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE) ? "   [unbinding]" : "");
  if (!ok) LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
  return ok;
}

EGLBoolean egl_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  if (surface == g_surface) {
    LOGE("the engine is destroying its window surface -- rendering stops until it makes a new one");
    g_surface = EGL_NO_SURFACE;
  }
  return eglDestroySurface(dpy, surface);
}

EGLBoolean egl_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  LOGI("eglDestroyContext(%p)%s", (void *)ctx, ctx == g_context ? "   [the main context]" : "");
  return eglDestroyContext(dpy, ctx);
}

EGLBoolean egl_Terminate(EGLDisplay dpy) {
  LOGE("eglTerminate: the engine is tearing EGL down");
  return eglTerminate(dpy);
}

EGLBoolean egl_SwapInterval(EGLDisplay dpy, EGLint interval) {
  LOGI("eglSwapInterval(%d)", interval);
  return eglSwapInterval(dpy, interval);
}

EGLint egl_GetError(void) {
  EGLint e = eglGetError();
  if (e != EGL_SUCCESS) LOGI("the engine read eglGetError() = 0x%x", e);
  return e;
}

/* Defold's only pbuffer is the 1x1 surface of its auxiliary context, a second
 * GL context shared with the main one and made current on a worker thread for
 * texture uploads. switch-mesa's nouveau driver predates the locking that makes
 * two contexts on two threads safe. Refusing the pbuffer, however, drops the
 * engine into its "EGL allocation failed / retrying EGL initialization" path,
 * which rebuilds the window surface -- and an NWindow backs only one
 * EGLSurface, so that rebuild cannot succeed. The pbuffer is therefore allowed
 * by default; set gl_aux_context 0 to refuse it and keep uploads on the render
 * thread. */
EGLSurface egl_CreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attribs) {
  if (!g_settings.gl_aux_context) {
    LOGI("eglCreatePbufferSurface refused (gl_aux_context 0)");
    return EGL_NO_SURFACE;
  }
  host_window_reassert();
  EGLSurface s = eglCreatePbufferSurface(dpy, cfg, attribs);
  LOGI("eglCreatePbufferSurface -> %p", (void *)s);
  return s;
}

void gl_BindVertexArrayOES(GLuint vao) { glBindVertexArray(vao); }
void gl_GenVertexArraysOES(GLsizei n, GLuint *arrays) { glGenVertexArrays(n, arrays); }
