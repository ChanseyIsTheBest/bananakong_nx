#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "gl_diag.h"
#include "log.h"
#include "settings.h"

static unsigned g_draws, g_draws_fb0, g_frames, g_clears;
static unsigned g_tex, g_ctex;
static unsigned long long g_tex_bytes, g_ctex_bytes;
static GLuint g_fbo;
#if BK_LOG_ENABLE
static GLenum g_seen[16];
static int g_nseen;
#endif
static GLfloat g_clear[4];
static GLint g_vp[4];

#if BK_LOG_ENABLE
static const char *fmt_name(GLenum f) {
  if (f >= 0x93B0 && f <= 0x93BD) return "ASTC_LDR";
  switch (f) {
    case 0x8D64: return "ETC1_RGB8";
    case 0x9274: return "ETC2_RGB8";
    case 0x9278: return "ETC2_RGBA8_EAC";
    case 0x83F0: return "DXT1_RGB";
    case 0x83F1: return "DXT1_RGBA";
    case 0x83F2: return "DXT3";
    case 0x83F3: return "DXT5";
    case 0x1907: return "RGB";
    case 0x1908: return "RGBA";
    case 0x8058: return "RGBA8";
    case 0x8051: return "RGB8";
    default: return "other";
  }
}

static void note_format(GLenum f) {
  for (int i = 0; i < g_nseen; i++)
    if (g_seen[i] == f) return;
  if (g_nseen < 16) {
    g_seen[g_nseen++] = f;
    LOGI("texture format in use: 0x%x (%s)", f, fmt_name(f));
  }
}
#else
static void note_format(GLenum f) { (void)f; }
#endif

void gl_diag_nwindow(const char *when) {
#if BK_LOG_ENABLE
  NWindow *w = nwindowGetDefault();
  unsigned slots = 0;
  for (u64 m = w->slots_configured; m; m >>= 1) slots += (unsigned)(m & 1);
  LOGI("nwindow %s: %ux%u buffers=%u cur_slot=%d swap_interval=%u behind=%d",
       when, w->width, w->height, slots, (int)w->cur_slot,
       (unsigned)w->swap_interval, (int)w->consumer_running_behind);
#else
  (void)when;
#endif
}

void gl_diag_egl(const char *fn, const void *result) { LOGI("%s -> %p", fn, result); (void)fn; (void)result; }

#if BK_LOG_ENABLE
static void context_info(void) {
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  const char *rend = (const char *)glGetString(GL_RENDERER);
  const char *ver = (const char *)glGetString(GL_VERSION);
  const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  LOGI("GL vendor=%s renderer=%s version=%s glsl=%s", vendor ? vendor : "?", rend ? rend : "?",
       ver ? ver : "?", sl ? sl : "?");
  const char *ext = (const char *)glGetString(GL_EXTENSIONS);
  if (ext)
    LOGI("compressed formats advertised: astc=%d etc1=%d s3tc=%d (extension string %u bytes)",
         strstr(ext, "astc") != NULL, strstr(ext, "ETC1") != NULL, strstr(ext, "s3tc") != NULL,
         (unsigned)strlen(ext));
  GLint v = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
  LOGI("GL_MAX_TEXTURE_SIZE=%d", v);
}
#endif

void gl_diag_frame(void) {
#if BK_LOG_ENABLE
  if (g_frames == 0) { context_info(); gl_diag_nwindow("first frame"); }
  GLenum err = glGetError();
  if (g_frames < 12 || (g_frames % 600) == 0 || err != GL_NO_ERROR) {
    LOGI("frame %u: draws=%u (fb0=%u) clears=%u clearcol=%.2f,%.2f,%.2f vp=%d,%d,%dx%d "
         "tex=%u(%lluKB) ctex=%u(%lluKB) fbo=%u err=0x%x%s",
         g_frames, g_draws, g_draws_fb0, g_clears, g_clear[0], g_clear[1], g_clear[2],
         g_vp[0], g_vp[1], g_vp[2], g_vp[3], g_tex, g_tex_bytes / 1024, g_ctex,
         g_ctex_bytes / 1024, g_fbo, err,
         err == GL_OUT_OF_MEMORY ? "  *** GPU MEMORY EXHAUSTED ***" : "");
  }
  if ((g_frames % 600) == 0 && g_frames) gl_diag_nwindow("running");
#endif
  /* Paint the whole frame a flat colour just before the swap. If the screen
   * shows it, the window, surface and compositor path are fine and the problem
   * is in what the engine draws; if it stays black, the frames are not
   * reaching the display. */
  if (g_settings.gl_test_clear) {
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor((g_frames % 60) < 30 ? 1.0f : 0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
  }
  g_frames++;
  g_draws = g_draws_fb0 = g_clears = 0;
}

void gl_DrawArrays(GLenum mode, GLint first, GLsizei count) {
  g_draws++;
  if (g_fbo == 0) g_draws_fb0++;
  glDrawArrays(mode, first, count);
}
void gl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  g_draws++;
  if (g_fbo == 0) g_draws_fb0++;
  glDrawElements(mode, count, type, indices);
}
void gl_BindFramebuffer(GLenum target, GLuint fb) {
  g_fbo = fb;
  glBindFramebuffer(target, fb);
}
void gl_Clear(GLbitfield mask) { g_clears++; glClear(mask); }
void gl_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  g_clear[0] = r; g_clear[1] = g; g_clear[2] = b; g_clear[3] = a;
  glClearColor(r, g, b, a);
}
void gl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h;
  glViewport(x, y, w, h);
}
void gl_CompileShader(GLuint shader) {
  glCompileShader(shader);
#if BK_LOG_ENABLE
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[1024] = "";
    glGetShaderInfoLog(shader, sizeof(buf) - 1, NULL, buf);
    LOGE("shader %u failed to compile: %s", shader, buf);
  }
#endif
}
void gl_LinkProgram(GLuint program) {
  glLinkProgram(program);
#if BK_LOG_ENABLE
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char buf[1024] = "";
    glGetProgramInfoLog(program, sizeof(buf) - 1, NULL, buf);
    LOGE("program %u failed to link: %s", program, buf);
  }
#endif
}
void gl_TexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px) {
  g_tex++;
  g_tex_bytes += (unsigned long long)w * (unsigned)h * 4u;
  note_format((GLenum)ifmt);
  glTexImage2D(t, l, ifmt, w, h, b, f, ty, px);
}
void gl_CompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *data) {
  g_ctex++;
  g_ctex_bytes += (unsigned long long)sz;
  note_format(ifmt);
  glCompressedTexImage2D(t, l, ifmt, w, h, b, sz, data);
#if BK_LOG_ENABLE
  GLenum e = glGetError();
  if (e != GL_NO_ERROR) LOGE("glCompressedTexImage2D(0x%x, %dx%d, %d bytes) -> error 0x%x", ifmt, w, h, sz, e);
#endif
}
