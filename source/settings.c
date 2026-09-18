/* settings.c -- config.txt next to the .nro. Plain "key value" lines. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "settings.h"

Settings g_settings;

/* The generated config.txt offers the one setting worth changing. The rest of
 * the Settings fields are fixed at their defaults below; the parser still
 * accepts them if written by hand, so a hand-tuned or older config.txt keeps
 * working and the bring-up switches (gl_test_clear, resolution, config, env)
 * stay available without advertising themselves. */
static const char *k_default_file =
  "# Banana Kong for Switch -- settings. Lines are \"key value\"; # starts a comment.\n"
  "\n"
  "# Game language: auto, en, de, es, fr, it, pt-BR, ru, tr.\n"
  "# \"auto\" follows the console's system language.\n"
  "language auto\n";

static void set_defaults(void) {
  memset(&g_settings, 0, sizeof(g_settings));
  snprintf(g_settings.language, sizeof(g_settings.language), "auto");
  snprintf(g_settings.variant, sizeof(g_settings.variant), "default");
  g_settings.cursor_speed = 1500;
  g_settings.cursor_autohide_ms = 2500;
  g_settings.touch = 1;
  g_settings.keep_screen_on = 1;
  g_settings.gl_aux_context = 1;
  snprintf(g_settings.resolution, sizeof(g_settings.resolution), "1080p");
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

void settings_load(const char *path) {
  set_defaults();
  FILE *f = fopen(path, "r");
  if (!f) {
    f = fopen(path, "w");
    if (f) { fputs(k_default_file, f); fclose(f); }
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *s = trim(line);
    if (!*s) continue;
    char *v = s;
    while (*v && !isspace((unsigned char)*v)) v++;
    if (*v) *v++ = 0;
    v = trim(v);
    if (!strcmp(s, "language")) snprintf(g_settings.language, sizeof(g_settings.language), "%s", v);
    else if (!strcmp(s, "variant")) snprintf(g_settings.variant, sizeof(g_settings.variant), "%s", v);
    else if (!strcmp(s, "cursor_speed")) g_settings.cursor_speed = atoi(v);
    else if (!strcmp(s, "cursor_autohide_ms")) g_settings.cursor_autohide_ms = atoi(v);
    else if (!strcmp(s, "touch")) g_settings.touch = atoi(v);
    else if (!strcmp(s, "keep_screen_on")) g_settings.keep_screen_on = atoi(v);
    else if (!strcmp(s, "gl_aux_context")) g_settings.gl_aux_context = atoi(v);
    else if (!strcmp(s, "gl_test_clear")) g_settings.gl_test_clear = atoi(v);
    else if (!strcmp(s, "resolution")) snprintf(g_settings.resolution, sizeof(g_settings.resolution), "%s", v);
    else if (!strcmp(s, "config") && g_settings.n_config < SETTINGS_MAX_LINES)
      snprintf(g_settings.config[g_settings.n_config++], 160, "%s", v);
    else if (!strcmp(s, "env") && g_settings.n_env < SETTINGS_MAX_LINES)
      snprintf(g_settings.env[g_settings.n_env++], 160, "%s", v);
    else LOGI("config.txt: unknown key '%s'", s);
  }
  fclose(f);
  if (g_settings.cursor_speed < 100) g_settings.cursor_speed = 100;
  if (g_settings.cursor_autohide_ms < 0) g_settings.cursor_autohide_ms = 0;
}
