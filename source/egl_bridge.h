#ifndef BK_EGL_BRIDGE_H
#define BK_EGL_BRIDGE_H

#include <EGL/egl.h>
#include <GLES3/gl3.h>

EGLBoolean egl_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean egl_ChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num);
EGLSurface egl_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attribs);
EGLContext egl_CreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attribs);
EGLBoolean egl_SwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLSurface egl_CreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attribs);
EGLBoolean egl_MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean egl_DestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_DestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean egl_Terminate(EGLDisplay dpy);
EGLBoolean egl_SwapInterval(EGLDisplay dpy, EGLint interval);
EGLint     egl_GetError(void);
EGLBoolean egl_QuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
void       gl_BindVertexArrayOES(GLuint vao);
void       gl_GenVertexArraysOES(GLsizei n, GLuint *arrays);

#endif
