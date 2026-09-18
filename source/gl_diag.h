#ifndef BK_GL_DIAG_H
#define BK_GL_DIAG_H

#include <GLES3/gl3.h>

/* Instrumentation for bring-up: the wrappers are always linked but only log
 * when the build sets LOG=1. They answer the questions a black screen raises:
 * is a context current, are draws being issued, to which framebuffer, in which
 * texture formats, and does the compositor accept the frames. */
void gl_diag_frame(void);                 /* once per eglSwapBuffers */
void gl_diag_nwindow(const char *when);
void gl_diag_egl(const char *fn, const void *result);

void gl_DrawArrays(GLenum mode, GLint first, GLsizei count);
void gl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void gl_BindFramebuffer(GLenum target, GLuint fb);
void gl_Clear(GLbitfield mask);
void gl_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void gl_CompileShader(GLuint shader);
void gl_LinkProgram(GLuint program);
void gl_TexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px);
void gl_CompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *data);

#endif
