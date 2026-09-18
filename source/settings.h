#ifndef BK_SETTINGS_H
#define BK_SETTINGS_H

#define SETTINGS_MAX_LINES 24

typedef struct {
  char language[16];        /* auto | en | de | es | fr | it | pt-BR | ru | tr */
  char variant[16];         /* default | mobile | playables (sets bk.variant) */
  int  cursor_speed;        /* pixels per second at full stick deflection (1080p space) */
  int  cursor_autohide_ms;  /* hide the stick cursor after this long idle; 0 = never */
  int  touch;               /* 1 = touchscreen enabled */
  int  keep_screen_on;      /* 1 = stop the console dimming during play */
  int  gl_aux_context;      /* 1 (default) = let Defold create its second (upload) GL context */
  int  gl_test_clear;       /* 1 = paint each frame a flat colour just before the swap */
  char resolution[8];       /* 1080p | 720p: size of the window framebuffer */
  int  n_config;
  char config[SETTINGS_MAX_LINES][160];  /* extra --config=section.key=value */
  int  n_env;
  char env[SETTINGS_MAX_LINES][160];     /* NAME=VALUE put in the environment */
} Settings;

extern Settings g_settings;

void settings_load(const char *path);

#endif
