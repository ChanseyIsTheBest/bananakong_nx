#include <math.h>
#include <string.h>
#include <switch.h>

#include "android_host.h"
#include "config.h"
#include "cursor.h"
#include "input.h"
#include "settings.h"
#include "util.h"

#define CURSOR_POINTER_ID 9

typedef struct { int id; float x, y; } Ptr;

static PadState g_pad;
static unsigned char g_key_sent[128];
static Ptr g_prev[HOST_MAX_POINTERS];
static int g_nprev;
static float g_cx = BK_RENDER_W / 2.0f, g_cy = BK_RENDER_H / 2.0f;
static int g_cursor_visible, g_cursor_sticky, g_cursor_pressing;
static uint64_t g_cursor_last_move_ns, g_last_ns;

void input_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  g_last_ns = bk_time_ns();
}

static void emit(int action, int index, const Ptr *set, int n) {
  int ids[HOST_MAX_POINTERS];
  float xs[HOST_MAX_POINTERS], ys[HOST_MAX_POINTERS];
  for (int i = 0; i < n; i++) { ids[i] = set[i].id; xs[i] = set[i].x; ys[i] = set[i].y; }
  host_inject_motion(action, index, n, ids, xs, ys);
}

static int find_ptr(const Ptr *set, int n, int id) {
  for (int i = 0; i < n; i++) if (set[i].id == id) return i;
  return -1;
}

/* Turn the set of pointers down this frame into Android's event sequence:
 * ups first, then downs, then one MOVE if anything moved. */
static void sync_pointers(const Ptr *cur, int ncur) {
  Ptr ws[HOST_MAX_POINTERS];
  int wn = g_nprev;
  memcpy(ws, g_prev, sizeof(Ptr) * (size_t)wn);

  for (int i = 0; i < wn;) {
    if (find_ptr(cur, ncur, ws[i].id) < 0) {
      emit(wn == 1 ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_POINTER_UP, i, ws, wn);
      memmove(&ws[i], &ws[i + 1], sizeof(Ptr) * (size_t)(wn - i - 1));
      wn--;
    } else {
      i++;
    }
  }
  for (int i = 0; i < ncur && wn < HOST_MAX_POINTERS; i++) {
    if (find_ptr(ws, wn, cur[i].id) >= 0) continue;
    ws[wn++] = cur[i];
    emit(wn == 1 ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_POINTER_DOWN, wn - 1, ws, wn);
  }
  int moved = 0;
  for (int i = 0; i < wn; i++) {
    int j = find_ptr(cur, ncur, ws[i].id);
    if (j >= 0 && (fabsf(cur[j].x - ws[i].x) > 0.5f || fabsf(cur[j].y - ws[i].y) > 0.5f)) {
      ws[i].x = cur[j].x;
      ws[i].y = cur[j].y;
      moved = 1;
    }
  }
  if (moved && wn) emit(AMOTION_EVENT_ACTION_MOVE, 0, ws, wn);
  memcpy(g_prev, ws, sizeof(Ptr) * (size_t)wn);
  g_nprev = wn;
}

static void set_key(int keycode, int down) {
  if (keycode <= 0 || keycode >= 128) return;
  if (g_key_sent[keycode] == (unsigned char)down) return;
  g_key_sent[keycode] = (unsigned char)down;
  host_inject_key(down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP, keycode);
}

void input_update(void) {
  uint64_t now = bk_time_ns();
  float dt = (float)(now - g_last_ns) / 1e9f;
  if (dt > 0.1f) dt = 0.1f;
  g_last_ns = now;

  padUpdate(&g_pad);
  u64 held = padGetButtons(&g_pad);
  u64 down = padGetButtonsDown(&g_pad);

  /* touchscreen */
  Ptr cur[HOST_MAX_POINTERS];
  int ncur = 0;
  if (g_settings.touch) {
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1)) {
      for (int i = 0; i < ts.count && ncur < HOST_MAX_POINTERS - 1; i++) {
        cur[ncur].id = (int)(ts.touches[i].finger_id % CURSOR_POINTER_ID);
        cur[ncur].x = (float)ts.touches[i].x * (float)BK_RENDER_W / (float)BK_PANEL_W;
        cur[ncur].y = (float)ts.touches[i].y * (float)BK_RENDER_H / (float)BK_PANEL_H;
        if (find_ptr(cur, ncur, cur[ncur].id) < 0) ncur++;
      }
    }
  }

  /* stick cursor */
  HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
  float sx = (float)ls.x / 32767.0f, sy = -(float)ls.y / 32767.0f;
  float mag = sqrtf(sx * sx + sy * sy);
  if (mag > 0.18f) {
    float k = (mag - 0.18f) / (1.0f - 0.18f);
    k = k * k;
    g_cx += sx / mag * k * (float)g_settings.cursor_speed * dt;
    g_cy += sy / mag * k * (float)g_settings.cursor_speed * dt;
    if (g_cx < 0) g_cx = 0;
    if (g_cy < 0) g_cy = 0;
    if (g_cx > BK_RENDER_W - 1) g_cx = BK_RENDER_W - 1;
    if (g_cy > BK_RENDER_H - 1) g_cy = BK_RENDER_H - 1;
    g_cursor_visible = 1;
    g_cursor_last_move_ns = now;
  }
  if (down & HidNpadButton_Minus) {
    g_cursor_visible = !g_cursor_visible;
    g_cursor_sticky = g_cursor_visible;
    g_cursor_last_move_ns = now;
  }
  if (ncur > 0) { g_cursor_visible = 0; g_cursor_sticky = 0; }
  if (g_cursor_visible && !g_cursor_sticky && !g_cursor_pressing && g_settings.cursor_autohide_ms > 0 &&
      now - g_cursor_last_move_ns > (uint64_t)g_settings.cursor_autohide_ms * 1000000ull)
    g_cursor_visible = 0;

  if (g_cursor_visible && (down & HidNpadButton_A)) g_cursor_pressing = 1;
  if (!(held & HidNpadButton_A) || !g_cursor_visible) g_cursor_pressing = 0;
  if (g_cursor_pressing && ncur == 0) {
    cur[ncur].id = CURSOR_POINTER_ID;
    cur[ncur].x = g_cx;
    cur[ncur].y = g_cy;
    ncur++;
  }
  sync_pointers(cur, ncur);
  cursor_set(g_cursor_visible, g_cx, g_cy);

  /* buttons -> keys; A only jumps while it is not driving the cursor */
  int a_jump = (held & HidNpadButton_A) && !g_cursor_visible && !g_cursor_pressing;
  set_key(AKEYCODE_SPACE, a_jump || (held & HidNpadButton_X));
  set_key(AKEYCODE_DPAD_UP, (held & HidNpadButton_Up) != 0);
  set_key(AKEYCODE_DPAD_DOWN, (held & (HidNpadButton_B | HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_Down)) != 0);
  set_key(AKEYCODE_DPAD_RIGHT, (held & (HidNpadButton_Y | HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_Right)) != 0);
  set_key(AKEYCODE_DPAD_LEFT, (held & HidNpadButton_Left) != 0);
  set_key(AKEYCODE_ESCAPE, (held & HidNpadButton_Plus) != 0);
}
