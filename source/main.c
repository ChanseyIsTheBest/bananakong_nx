/* main.c -- Banana Kong (Defold / NativeActivity) host for Nintendo Switch.
 *
 * Boot order mirrors what Android does for a NativeActivity:
 *   load libBananaKong.so, relocate, bind imports, switch the LuaJIT JIT off,
 *   map it executable, run its constructors;
 *   build the fake Java world and the ANativeActivity;
 *   ANativeActivity_onCreate (spawns the glue thread = engine main loop);
 *   DefoldActivity.nativeOnCreate;
 *   onStart, onResume, onNativeWindowCreated, onInputQueueCreated,
 *   onWindowFocusChanged(true).
 * The main thread then plays the Android UI thread: it feeds input, delivers
 * Java->native callbacks and watches for exit. */
#include <elf.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "android_host.h"
#include "app.h"
#include "bionic.h"
#include "bk_savetool.h"
#include "config.h"
#include "error.h"
#include "imports.h"
#include "input.h"
#include "jni_host.h"
#include "log.h"
#include "luajit_guard.h"
#include "opensles.h"
#include "pthread_bionic.h"
#include "settings.h"
#include "so_util.h"
#include "util.h"

extern void __libnx_exit(int rc) __attribute__((noreturn));

static char g_game_dir[512], g_files_dir[512], g_files_dir_sd[512], g_assets_dir[512];
static volatile int g_exit_requested, g_exit_code;
static Handle g_main_thread;
static so_module g_mod;

void app_request_exit(int code) { g_exit_code = code; g_exit_requested = 1; }
int  app_exit_requested(void) { return g_exit_requested; }
int  app_is_main_thread(void) { return threadGetCurHandle() == g_main_thread; }
const char *app_game_dir(void) { return g_game_dir; }
const char *app_files_dir(void) { return g_files_dir; }
const char *app_assets_dir(void) { return g_assets_dir; }

static void resolve_paths(int argc, char **argv) {
  snprintf(g_game_dir, sizeof(g_game_dir), "%s", BK_DEFAULT_DIR);
  if (argc > 0 && argv && argv[0] && strstr(argv[0], ":/")) {
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", argv[0]);
    char *slash = strrchr(g_game_dir, '/');
    if (slash) *slash = 0;
  }
  snprintf(g_files_dir_sd, sizeof(g_files_dir_sd), "%s/files", g_game_dir);
  snprintf(g_assets_dir, sizeof(g_assets_dir), "%s/assets", g_game_dir);
  /* The engine gets device-less paths ("/switch/..."): they resolve against
   * the default device (sdmc, set by chdir below) and survive any path code
   * that expects a leading slash. */
  const char *colon = strstr(g_files_dir_sd, ":/");
  snprintf(g_files_dir, sizeof(g_files_dir), "%s", colon ? colon + 1 : g_files_dir_sd);
}

static void check_install(void) {
  static const char *k_required[] = {
    BK_LIB_NAME, "assets/game.projectc", "assets/game.dmanifest", "assets/game.arci", "assets/game.arcd",
  };
  char missing[1024] = "";
  for (size_t i = 0; i < sizeof(k_required) / sizeof(k_required[0]); i++) {
    char p[768];
    snprintf(p, sizeof(p), "%s/%s", g_game_dir, k_required[i]);
    if (!bk_file_exists(p)) {
      strncat(missing, "  ", sizeof(missing) - strlen(missing) - 1);
      strncat(missing, k_required[i], sizeof(missing) - strlen(missing) - 1);
      strncat(missing, "\n", sizeof(missing) - strlen(missing) - 1);
    }
  }
  if (missing[0])
    fatal_error("Game files are missing from %s:\n\n%s\n"
                "Copy lib/arm64-v8a/" BK_LIB_NAME " and the whole assets/ folder\n"
                "from your Banana Kong APK next to the .nro (see README).", g_game_dir, missing);
}

static size_t elf_load_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  Elf64_Ehdr eh;
  size_t size = 0;
  if (fread(&eh, sizeof(eh), 1, f) == 1 && !memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64) {
      fclose(f);
      fatal_error(BK_LIB_NAME " is not a 64-bit ARM library.\n\n"
                  "Use the file from lib/arm64-v8a/ in the APK, not armeabi-v7a.");
    }
    for (int i = 0; i < eh.e_phnum; i++) {
      Elf64_Phdr ph;
      if (fseek(f, (long)(eh.e_phoff + (Elf64_Off)i * eh.e_phentsize), SEEK_SET) != 0 || fread(&ph, sizeof(ph), 1, f) != 1) break;
      if (ph.p_type == PT_LOAD && ph.p_vaddr + ph.p_memsz > size) size = ph.p_vaddr + ph.p_memsz;
    }
  }
  fclose(f);
  return ALIGN_UP(size, 0x1000);
}

static void load_game(void) {
  char path[768];
  snprintf(path, sizeof(path), "%s/%s", g_game_dir, BK_LIB_NAME);
  size_t load_size = elf_load_size(path);
  if (!load_size) fatal_error("Could not read %s.", path);
  void *base = memalign(0x1000, load_size);
  if (!base) fatal_error("Out of memory loading " BK_LIB_NAME " (%zu bytes).\n\nLaunch through title takeover (hold R while starting a game).", load_size);

  int r = so_load(&g_mod, path, base, load_size);
  if (r < 0) fatal_error("Could not load " BK_LIB_NAME " (error %d).", r);
  so_relocate(&g_mod);
  so_resolve(&g_mod, g_imports, g_num_imports, 1);
  if (luajit_guard_install(&g_mod) < 2)
    LOGE("LuaJIT guard incomplete: JIT traces may request executable memory");
  so_finalize(&g_mod);
  so_flush_caches(&g_mod);
  LOGI("module mapped at %p (%zu bytes); running constructors", g_mod.load_virtbase, g_mod.load_size);
  so_execute_init_array(&g_mod);
}

static void apply_env(void) {
  for (int i = 0; i < g_settings.n_env; i++) {
    char kv[160];
    snprintf(kv, sizeof(kv), "%s", g_settings.env[i]);
    char *eq = strchr(kv, '=');
    if (!eq) continue;
    *eq = 0;
    setenv(kv, eq + 1, 1);
    LOGI("env %s=%s", kv, eq + 1);
  }
}

static void do_exit(void) {
  LOGI("exiting (code %d)", g_exit_code);
  /* Freeze the engine's threads first: libnx teardown must not race a thread
   * that is mid-frame or holding a newlib lock. */
  threads_pause_all();
  appletSetMediaPlaybackState(false);
  /* No bk_log_close(): a frozen thread may own the log mutex. Lines are flushed as written. */
  __libnx_exit(0);
}

int main(int argc, char **argv) {
  g_main_thread = threadGetCurHandle();
  resolve_paths(argc, argv);
  bk_mkdir_p(g_game_dir);
  if (chdir(g_game_dir) != 0) LOGE("chdir(%s) failed", g_game_dir);

  char p[768];
  snprintf(p, sizeof(p), "%s/debug.log", g_game_dir);
  bk_log_open(p);
  LOGI("Banana Kong NX host starting in %s", g_game_dir);

  snprintf(p, sizeof(p), "%s/config.txt", g_game_dir);
  settings_load(p);
  check_install();
  bk_mkdir_p(g_files_dir_sd);
  apply_env();
  bk_savetool_apply();   /* edit saves.txt before the engine reads anything */

  bionic_tls_install(NULL);
  bionic_init();
  threads_init();

  load_game();

  jni_init();
  jni_defold_init(&g_mod);
  host_init(g_files_dir, g_assets_dir);
  ANativeActivity *act = host_create_activity(jni_vm(), jni_env(), jni_activity());

  opensles_init();
  input_init();
  if (g_settings.keep_screen_on) appletSetMediaPlaybackState(true);

  void (*on_create)(ANativeActivity *, void *, size_t) = (void *)so_try_find_addr_rx(&g_mod, "ANativeActivity_onCreate");
  if (!on_create) fatal_error(BK_LIB_NAME " has no ANativeActivity_onCreate.\n\nIs this the Banana Kong library?");

  error_set_graphics_owned(1);
  LOGI("ANativeActivity_onCreate");
  on_create(act, NULL, 0);

  void (*native_on_create)(void *, void *, void *) =
    (void *)so_try_find_addr_rx(&g_mod, "Java_com_dynamo_android_DefoldActivity_nativeOnCreate");
  if (native_on_create) native_on_create(jni_env(), jni_class("com/dynamo/android/DefoldActivity"), jni_activity());

  ANativeActivityCallbacks *cb = act->callbacks;
  if (cb->onStart) cb->onStart(act);
  if (cb->onResume) cb->onResume(act);
  LOGI("activity resumed; creating window");
  if (cb->onNativeWindowCreated) cb->onNativeWindowCreated(act, host_window());
  if (cb->onInputQueueCreated) cb->onInputQueueCreated(act, host_input_queue());
  if (cb->onWindowFocusChanged) cb->onWindowFocusChanged(act, 1);
  LOGI("lifecycle delivered; entering host loop");

  while (!g_exit_requested && appletMainLoop()) {
    input_update();
    jni_pump();
    svcSleepThread(8000000ll);
  }
  do_exit();
  return 0;
}
