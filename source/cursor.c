#include <stddef.h>
#include <GLES3/gl3.h>
#include "cursor.h"
#include "config.h"
#include "log.h"

static volatile int   g_visible;
static volatile float g_x = BK_RENDER_W / 2, g_y = BK_RENDER_H / 2;
static GLuint g_prog, g_vao, g_vbo;
static GLint  g_u_xform, g_u_color;
static int    g_failed;

void cursor_set(int visible, float x, float y) { g_x = x; g_y = y; g_visible = visible; }

/* Arrow polygon in pixels, tip at (0,0), split into 5 triangles. The outline
 * pass reuses it scaled up around an interior point. */
static const float k_arrow[] = {
  0, 0,  0, 24,  6, 19,
  0, 0,  6, 19, 10, 17,
  0, 0, 10, 17, 17, 17,
  6, 19, 10, 28, 14, 26,
  6, 19, 14, 26, 10, 17,
};

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { LOGE("cursor shader compile failed"); glDeleteShader(s); return 0; }
  return s;
}

static int init_gl(void) {
  static const char *vs =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_xform;\n"
    "void main() { gl_Position = vec4(a_pos * u_xform.xy + u_xform.zw, 0.0, 1.0); }\n";
  static const char *fs =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return 0;
  g_prog = glCreateProgram();
  glAttachShader(g_prog, v);
  glAttachShader(g_prog, f);
  glBindAttribLocation(g_prog, 0, "a_pos");
  glLinkProgram(g_prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) { LOGE("cursor program link failed"); return 0; }
  g_u_xform = glGetUniformLocation(g_prog, "u_xform");
  g_u_color = glGetUniformLocation(g_prog, "u_color");

  GLint prev_vao = 0, prev_buf = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGenVertexArrays(1, &g_vao);
  glGenBuffers(1, &g_vbo);
  glBindVertexArray(g_vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(k_arrow), k_arrow, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glBindVertexArray((GLuint)prev_vao);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  return 1;
}

static void draw_pass(float s, float ox, float oy, float r, float g, float b, float a, int w, int h) {
  float px = g_x + ox, py = g_y + oy;
  glUniform4f(g_u_xform, 2.0f * s / (float)w, -2.0f * s / (float)h, 2.0f * px / (float)w - 1.0f, 1.0f - 2.0f * py / (float)h);
  glUniform4f(g_u_color, r, g, b, a);
  glDrawArrays(GL_TRIANGLES, 0, 15);
}

void cursor_draw(int w, int h) {
  if (!g_visible || g_failed || w <= 0 || h <= 0) return;

  GLint prog, vao, abuf, fbo, vp[4], src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
  GLboolean blend, depth, cull, scissor, stencil, cmask[4];
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
  glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
  blend = glIsEnabled(GL_BLEND);
  depth = glIsEnabled(GL_DEPTH_TEST);
  cull = glIsEnabled(GL_CULL_FACE);
  scissor = glIsEnabled(GL_SCISSOR_TEST);
  stencil = glIsEnabled(GL_STENCIL_TEST);

  if (!g_prog && !init_gl()) { g_failed = 1; return; }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(g_prog);
  glBindVertexArray(g_vao);

  float scale = 1.6f * (float)h / (float)BK_RENDER_H;
  draw_pass(scale * 1.3f, -1.8f * scale, -4.2f * scale, 0.0f, 0.0f, 0.0f, 0.85f, w, h);
  draw_pass(scale, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, w, h);

  glBindVertexArray((GLuint)vao);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
  glUseProgram((GLuint)prog);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glBlendEquationSeparate((GLenum)eq_rgb, (GLenum)eq_a);
  glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  if (depth) glEnable(GL_DEPTH_TEST);
  if (cull) glEnable(GL_CULL_FACE);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (stencil) glEnable(GL_STENCIL_TEST);
}
