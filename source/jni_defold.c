/* jni_defold.c -- what the Java side of Banana Kong would have answered.
 *
 * Covers the Defold runtime (DefoldActivity, sys, sound, window), the Android
 * framework calls the engine makes, the game's own BkLangSplits / BkReview /
 * bk_lang_native resource access, and the SDK extensions (AdMob, Firebase,
 * Crashlytics, Google Play Games, IAP, Push). Every SDK behaves like a device
 * without Google services: calls that would eventually call back into native
 * code do call back, with a failure, from the host's main thread the way the
 * Java UI thread would. Flows that wait for an answer therefore get one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "config.h"
#include "jni_host.h"
#include "log.h"
#include "settings.h"
#include "so_util.h"
#include "util.h"

#define ACT "@activity"
#define LANGPACK_RES_ID 0x7f130001

static so_module *g_mod;
static JObj *g_activity, *g_locale, *g_intent, *g_intent_args, *g_push_listener;
static char g_lang[8] = "en", g_country[8] = "US", g_tag[24] = "en-US";
static char g_langpack_path[512];
static int  g_langpack_present;
static int  g_dimming_enabled = 1;

JObj *jni_activity(void) { return g_activity; }

static jvalue jv(void) { jvalue v; memset(&v, 0, sizeof(v)); return v; }
static jvalue jv_obj(JObj *o) { jvalue v = jv(); v.l = o; return v; }
static jvalue jv_int(int32_t i) { jvalue v = jv(); v.i = i; return v; }
static jvalue jv_bool(int b) { jvalue v = jv(); v.z = b ? 1 : 0; return v; }
static jvalue jv_float(float f) { jvalue v = jv(); v.f = f; return v; }
static jvalue jv_str(const char *s) { return jv_obj(s ? jni_new_string(s) : NULL); }

static JObj *singleton(const char *cls) {
  static struct { const char *cls; JObj *o; } cache[48];
  static Mutex mu;
  static int inited;
  if (!inited) { mutexInit(&mu); inited = 1; }
  mutexLock(&mu);
  for (int i = 0; i < 48 && cache[i].cls; i++)
    if (!strcmp(cache[i].cls, cls)) { mutexUnlock(&mu); return cache[i].o; }
  JObj *o = jni_new_object(cls);
  for (int i = 0; i < 48; i++) if (!cache[i].cls) { cache[i].cls = o->cls; cache[i].o = o; break; }
  mutexUnlock(&mu);
  return o;
}

static JObj *make_file(const char *path) {
  JObj *f = jni_new_object("java/io/File");
  f->sval = strdup(path);
  return f;
}

/* ---- deferred Java -> native callbacks -------------------------------------------- */

enum { CB_QUEUE, CB_IAP_PRODUCTS, CB_IAP_PURCHASE, CB_PUSH_REG };
typedef struct Deferred {
  int kind;
  const char *fn;
  JObj *target;
  int code;
  char *json;
  int64_t cmd;
  struct Deferred *next;
} Deferred;

static Deferred *g_def_head, *g_def_tail;
static Mutex g_def_mu;

static void defer(int kind, const char *fn, JObj *target, int code, const char *json, int64_t cmd) {
  Deferred *d = bk_xcalloc(1, sizeof(*d));
  d->kind = kind; d->fn = fn; d->target = target; d->code = code; d->cmd = cmd;
  d->json = json ? strdup(json) : NULL;
  mutexLock(&g_def_mu);
  if (g_def_tail) g_def_tail->next = d; else g_def_head = d;
  g_def_tail = d;
  mutexUnlock(&g_def_mu);
}

#define ADMOB_Q  "Java_com_defold_admob_AdmobJNI_admobAddToQueue"
#define GPGS_Q   "Java_com_defold_gpgs_GpgsJNI_gpgsAddToQueue"
#define FB_Q     "Java_com_defold_firebase_FirebaseJNI_firebaseAddToQueue"
#define FBA_Q    "Java_com_defold_firebase_analytics_FirebaseAnalyticsJNI_firebaseAddToQueue"
#define IAP_PROD "Java_com_defold_iap_IapJNI_onProductsResult"
#define IAP_BUY  "Java_com_defold_iap_IapJNI_onPurchaseResult__ILjava_lang_String_2"
#define PUSH_REG "Java_com_defold_push_PushJNI_onRegistration"

void jni_pump(void) {
  mutexLock(&g_def_mu);
  Deferred *d = g_def_head;
  g_def_head = g_def_tail = NULL;
  mutexUnlock(&g_def_mu);
  while (d) {
    Deferred *next = d->next;
    uintptr_t fn = g_mod ? so_try_find_addr_rx(g_mod, d->fn) : 0;
    void *env = jni_env();
    if (fn) {
      JObj *json = d->json ? jni_new_string(d->json) : NULL;
      LOGT("jni", "callback %s(%d, %s)", d->fn, d->code, d->json ? d->json : "null");
      switch (d->kind) {
        case CB_QUEUE:
          ((void (*)(void *, JObj *, int32_t, JObj *))fn)(env, jni_class("java/lang/Object"), d->code, json);
          break;
        case CB_IAP_PRODUCTS:
          ((void (*)(void *, JObj *, int32_t, JObj *, int64_t))fn)(env, d->target, d->code, json, d->cmd);
          break;
        case CB_IAP_PURCHASE:
          ((void (*)(void *, JObj *, int32_t, JObj *))fn)(env, d->target, d->code, json);
          break;
        case CB_PUSH_REG: {
          JObj *err = jni_new_string("Push notifications are not available on this platform");
          ((void (*)(void *, JObj *, JObj *, JObj *))fn)(env, d->target, NULL, err);
          jni_release(err);
          break;
        }
      }
      jni_release(json);
    }
    free(d->json);
    free(d);
    d = next;
  }
}

/* ---- class matching ----------------------------------------------------------------- */

static int is_activity_class(const char *c) {
  return !strcmp(c, "com/dynamo/android/DefoldActivity") || !strcmp(c, "android/app/NativeActivity") ||
         !strcmp(c, "android/app/Activity") || !strcmp(c, "android/content/Context") ||
         !strcmp(c, "android/content/ContextWrapper") || !strcmp(c, "android/view/ContextThemeWrapper");
}

static int cls_match(const char *want, const char *have) {
  if (!want) return 1;
  if (!strcmp(want, ACT)) return is_activity_class(have);
  if (want[0] == '*') {
    size_t lw = strlen(want + 1), lh = strlen(have);
    return lh >= lw && !strcmp(have + lh - lw, want + 1);
  }
  return !strcmp(want, have);
}

static int arg_index(const JMethod *m, char type, int nth) {
  for (int i = 0; i < m->nargs; i++)
    if ((m->args[i] == type || (type == 'L' && m->args[i] == '[')) && nth-- == 0) return i;
  return -1;
}

/* ---- Android framework -------------------------------------------------------------- */

static jvalue h_classloader(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(singleton("java/lang/ClassLoader")); }
static jvalue h_loadclass(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; const char *n = jni_str(a[0].l); return jv_obj(n ? jni_class(n) : NULL); }
static jvalue h_self_activity(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(g_activity); }
static jvalue h_window_manager(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(singleton("android/view/WindowManager")); }
static jvalue h_display(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(singleton("android/view/Display")); }
static jvalue h_refresh_rate(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_float(60.0f); }
static jvalue h_files_dir(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(make_file(app_files_dir())); }
static jvalue h_cache_dir(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s; (void)a;
  char p[512];
  snprintf(p, sizeof(p), "%s/cache", app_files_dir());
  bk_mkdir_p(p);
  return jv_obj(make_file(p));
}
static jvalue h_package_name(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_str(BK_PACKAGE); }
static jvalue h_intent(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(g_intent); }
static jvalue h_zero(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv(); }
static jvalue h_true(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_bool(1); }
static jvalue h_self(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_obj(s); }

static jvalue h_singleton_by_return(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  const char *r = strrchr(m->sig, ')');
  if (!r || r[1] != 'L') return jv();
  char cls[160];
  snprintf(cls, sizeof(cls), "%s", r + 2);
  char *semi = strchr(cls, ';');
  if (semi) *semi = 0;
  return jv_obj(singleton(cls));
}

static jvalue h_system_service(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  const char *n = jni_str(a[0].l);
  if (n && !strcmp(n, "locale")) return jv_obj(singleton("android/app/LocaleManager"));
  if (n && !strcmp(n, "window")) return jv_obj(singleton("android/view/WindowManager"));
  LOGT("jni", "getSystemService(%s)", n ? n : "null");
  return jv_obj(singleton("java/lang/Object"));
}

static jvalue h_start_activity(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  JObj *intent = a[0].l;
  JObj *uri = intent ? intent->ref[0] : NULL;
  LOGI("startActivity(%s) ignored", uri && uri->sval ? uri->sval : "?");
  (void)uri;
  return jv();
}

static jvalue h_safe_insets(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(jni_new_array('I', 4)); }
static jvalue h_empty_int_array(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(jni_new_array('I', 0)); }

static jvalue h_file_path(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_str(s && s->sval ? s->sval : ""); }
static jvalue h_file_parent(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  if (!s || !s->sval) return jv();
  char p[512];
  snprintf(p, sizeof(p), "%s", s->sval);
  char *slash = strrchr(p, '/');
  if (!slash || slash == p) return jv();
  *slash = 0;
  return jv_str(p);
}

static jvalue h_locale_default(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(g_locale); }
static jvalue h_locale_language(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_str(g_lang); }
static jvalue h_locale_country(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_str(g_country); }
static jvalue h_locale_tag(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_str(g_tag); }
static jvalue h_locale_string(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s; (void)a;
  char b[24];
  snprintf(b, sizeof(b), "%s_%s", g_lang, g_country);
  return jv_str(b);
}
static JObj *make_locale_list(const char *tags) {
  JObj *l = jni_new_object("android/os/LocaleList");
  l->sval = strdup(tags);
  return l;
}
static jvalue h_app_locales(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(make_locale_list("")); }
static jvalue h_system_locales(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(make_locale_list(g_tag)); }
static jvalue h_localelist_tags(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_str(s && s->sval ? s->sval : ""); }
static jvalue h_localelist_size(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_int(s && s->sval && s->sval[0] ? 1 : 0); }
static jvalue h_localelist_empty(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_bool(!(s && s->sval && s->sval[0])); }

static jvalue h_get_identifier(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  const char *name = jni_str(a[0].l), *type = jni_str(a[1].l);
  int id = 0;
  if (name && !strcmp(name, "bk_lang_pack") && (!type || !strcmp(type, "raw")) && g_langpack_present) id = LANGPACK_RES_ID;
  LOGT("jni", "getIdentifier(%s, %s) -> 0x%x", name ? name : "null", type ? type : "null", id);
  return jv_int(id);
}

static jvalue h_open_raw(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  if (a[0].i != LANGPACK_RES_ID || !g_langpack_present) return jv();
  FILE *f = fopen(g_langpack_path, "rb");
  if (!f) return jv();
  JObj *in = jni_new_object("java/io/InputStream");
  in->native = f;
  return jv_obj(in);
}

static jvalue h_stream_read(JMethod *m, JObj *s, const jvalue *a) {
  JObj *arr = a[0].l;
  FILE *f = s ? s->native : NULL;
  if (!f || !arr || arr->kind != JO_ARRAY) return jv_int(-1);
  int32_t off = 0, len = arr->length;
  if (m->nargs >= 3) { off = a[1].i; len = a[2].i; }
  if (off < 0 || len < 0 || off + len > arr->length) return jv_int(-1);
  if (len == 0) return jv_int(0);
  size_t n = fread((char *)arr->elems + off, 1, (size_t)len, f);
  return jv_int(n ? (int32_t)n : -1);
}
static jvalue h_stream_available(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  FILE *f = s ? s->native : NULL;
  if (!f) return jv_int(0);
  long pos = ftell(f);
  fseek(f, 0, SEEK_END);
  long end = ftell(f);
  fseek(f, pos, SEEK_SET);
  return jv_int((int32_t)(end - pos));
}
static jvalue h_stream_close(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  if (s && s->native) { fclose(s->native); s->native = NULL; }
  return jv();
}

static jvalue h_uri_parse(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  JObj *u = jni_new_object("android/net/Uri");
  const char *str = jni_str(a[0].l);
  u->sval = strdup(str ? str : "");
  return jv_obj(u);
}
static jvalue h_sval_string(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)a; return jv_str(s && s->sval ? s->sval : ""); }

static jvalue h_string_array_extra(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)s;
  const char *key = jni_str(a[0].l);
  if (key && !strcmp(key, "com.dynamo.android.EXTRA_COMMAND_LINE_ARGUMENTS")) return jv_obj(g_intent_args);
  return jv();
}

static jvalue h_unicode_char(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  int k = s ? (int)s->ival[0] : 0;
  if (k >= 29 && k <= 54) return jv_int('a' + (k - 29));
  if (k >= 7 && k <= 16) return jv_int('0' + (k - 7));
  if (k == 62) return jv_int(' ');
  if (k == 66) return jv_int('\n');
  return jv_int(0);
}

static jvalue h_android_id(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_str("5e1f0c0a5e1f0c0a"); }

static jvalue h_to_string(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  if (!s) return jv_str("");
  if (s->kind == JO_STRING) return jv_str(s->utf);
  if (s == g_locale) return h_locale_string(m, s, a);
  if (s->sval) return jv_str(s->sval);
  char name[160];
  snprintf(name, sizeof(name), "%s", s->cls);
  for (char *p = name; *p; p++) if (*p == '/') *p = '.';
  return jv_str(name);
}

static jvalue h_string_getbytes(JMethod *m, JObj *s, const jvalue *a) {
  (void)m; (void)a;
  const char *u = jni_str(s);
  if (!u) return jv();
  JObj *arr = jni_new_array('B', (int32_t)strlen(u));
  memcpy(arr->elems, u, strlen(u));
  return jv_obj(arr);
}

/* ---- Defold runtime --------------------------------------------------------------------- */

static jvalue h_sample_rate(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_int(BK_AUDIO_RATE); }
static jvalue h_frames_per_buffer(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_int(960); }

static jvalue h_dimming(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  int enable = !strcmp(m->name, "enableScreenDimming");
  g_dimming_enabled = enable;
  if (!g_settings.keep_screen_on) appletSetMediaPlaybackState(!enable);
  return jv();
}
static jvalue h_dimming_enabled(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_bool(g_dimming_enabled); }

/* ---- SDKs ----------------------------------------------------------------------------------- */

/* Shape of AdmobJNI.sendConsentStatus(event). */
#define CONSENT_JSON(ev) "{\"event\":" #ev ",\"can_request_ads\":0,\"privacy_options_required\":0,\"consent_required\":0}"

static jvalue h_admob(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  const char *n = m->name;
  static const struct { const char *suffix; int msg; } kinds[] = {
    { "RewardedInterstitial", 6 }, { "Interstitial", 1 }, { "Rewarded", 2 }, { "AppOpen", 7 }, { "Banner", 3 },
  };
  int msg = 0;
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
    if (strstr(n, kinds[i].suffix)) { msg = kinds[i].msg; break; }

  if (!strcmp(n, "initialize")) {
    defer(CB_QUEUE, ADMOB_Q, NULL, 4, "{\"event\":8}", 0);                       /* INITIALIZATION / COMPLETE */
  } else if (!strcmp(n, "requestConsent")) {
    /* AdmobJNI.onConsentInfoUpdateFailure -> sendConsentStatus(4) */
    defer(CB_QUEUE, ADMOB_Q, NULL, 8, CONSENT_JSON(4), 0);
  } else if (strstr(n, "Consent") || strstr(n, "PrivacyOptions")) {
    if (!strncmp(n, "show", 4) || !strncmp(n, "load", 4))
      defer(CB_QUEUE, ADMOB_Q, NULL, 8, CONSENT_JSON(2), 0);       /* FAILED_TO_SHOW */
  } else if (!strcmp(n, "requestIDFA")) {
    defer(CB_QUEUE, ADMOB_Q, NULL, 5, "{\"event\":17}", 0);                      /* NOT_SUPPORTED */
  } else if (msg && !strncmp(n, "load", 4)) {
    defer(CB_QUEUE, ADMOB_Q, NULL, msg, "{\"event\":4,\"code\":3,\"error\":\"No fill\"}", 0);   /* FAILED_TO_LOAD */
  } else if (msg && !strncmp(n, "show", 4)) {
    defer(CB_QUEUE, ADMOB_Q, NULL, msg, "{\"event\":6,\"error\":\"Ad not loaded\"}", 0);        /* NOT_LOADED */
  }
  return jv();   /* is*Loaded, canRequestAds, isPrivacyOptionsRequired: false */
}

static jvalue h_gpgs(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  static const struct { const char *name; int msg; } calls[] = {
    { "login", 1 }, { "silentLogin", 2 }, { "showSavedGamesUI", 4 }, { "loadSnapshot", 5 },
    { "commitAndCloseSnapshot", 6 }, { "getAchievements", 7 }, { "loadTopScores", 8 },
    { "loadPlayerCenteredScores", 9 }, { "loadCurrentPlayerLeaderboardScore", 10 },
    { "loadEvents", 11 }, { "requestServerAuthCode", 12 }, { "submitScoreImmediate", 13 },
  };
  for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
    if (!strcmp(m->name, calls[i].name)) {
      defer(CB_QUEUE, GPGS_Q, NULL, calls[i].msg, "{\"status\":2,\"error\":\"Google Play Games is not available\"}", 0);
      return jv();
    }
  }
  if (!strcmp(m->name, "setSave")) return jv_str("Not signed in");
  return jv();   /* isSupported / isLoggedIn / isSnapshotOpened: false; getters: null */
}

static jvalue h_firebase(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  if (!strcmp(m->name, "setOption")) return jv_bool(1);
  if (!strcmp(m->name, "initialize")) defer(CB_QUEUE, FB_Q, NULL, 1, "{}", 0);
  else if (!strncmp(m->name, "getInstallation", 15))
    defer(CB_QUEUE, FB_Q, NULL, 0, "{\"error\":\"Firebase is not available\"}", 0);
  return jv();
}

static jvalue h_firebase_analytics(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  if (!strcmp(m->name, "getInstanceId"))
    defer(CB_QUEUE, FBA_Q, NULL, 0, "{\"error\":\"Firebase Analytics is not available\"}", 0);
  return jv();
}

#define BILLING_UNAVAILABLE 3

static jvalue h_iap(JMethod *m, JObj *s, const jvalue *a) {
  (void)s;
  int li = arg_index(m, 'L', 1), ci = arg_index(m, 'J', 0);
  if (!strcmp(m->name, "listItems") || !strcmp(m->name, "queryOwnedPurchases")) {
    JObj *listener = NULL;
    for (int i = 0; i < m->nargs; i++) if (m->args[i] == 'L' && a[i].l && a[i].l->kind == JO_OBJECT) listener = a[i].l;
    (void)li;
    defer(CB_IAP_PRODUCTS, IAP_PROD, listener, BILLING_UNAVAILABLE, NULL, ci >= 0 ? a[ci].j : 0);
  } else if (!strcmp(m->name, "buy")) {
    JObj *listener = NULL;
    for (int i = 0; i < m->nargs; i++) if (m->args[i] == 'L' && a[i].l && a[i].l->kind == JO_OBJECT) listener = a[i].l;
    defer(CB_IAP_PURCHASE, IAP_BUY, listener, BILLING_UNAVAILABLE, NULL, 0);
  }
  return jv();
}

static jvalue h_push_instance(JMethod *m, JObj *s, const jvalue *a) { (void)m; (void)s; (void)a; return jv_obj(singleton("com/defold/push/Push")); }
static jvalue h_push(JMethod *m, JObj *s, const jvalue *a) {
  (void)s;
  if (!strcmp(m->name, "start")) {
    for (int i = 0; i < m->nargs; i++)
      if (m->args[i] == 'L' && a[i].l && a[i].l->kind == JO_OBJECT && a[i].l != g_activity) { g_push_listener = a[i].l; break; }
  } else if (!strcmp(m->name, "register") && g_push_listener) {
    defer(CB_PUSH_REG, PUSH_REG, g_push_listener, 0, NULL, 0);
  }
  return jv();
}

static jvalue h_lang_splits(JMethod *m, JObj *s, const jvalue *a) {
  (void)s; (void)a;
  if (!strcmp(m->name, "getInstalledLanguages")) return jv_str(g_langpack_present ? g_lang : "");
  if (!strcmp(m->name, "getInstallStatus")) return jv_str("idle:0:0:0");   /* BkLangSplits initial state: status:errorCode:bytes:total */
  if (m->ret == 'L' && strstr(m->sig, ")Ljava/lang/String;")) return jv_str("");
  return jv();
}

/* ---- tables ------------------------------------------------------------------------------- */

typedef struct { const char *cls, *name, *sig; JHandler fn; } MethodDef;

static const MethodDef k_methods[] = {
  { ACT, "getClassLoader", NULL, h_classloader },
  { ACT, "getWindowManager", NULL, h_window_manager },
  { ACT, "getFilesDir", NULL, h_files_dir },
  { ACT, "getExternalFilesDir", NULL, h_files_dir },
  { ACT, "getNoBackupFilesDir", NULL, h_files_dir },
  { ACT, "getCacheDir", NULL, h_cache_dir },
  { ACT, "getExternalCacheDir", NULL, h_cache_dir },
  { ACT, "getPackageName", NULL, h_package_name },
  { ACT, "getIntent", NULL, h_intent },
  { ACT, "getApplicationContext", NULL, h_self_activity },
  { ACT, "getBaseContext", NULL, h_self_activity },
  { ACT, "getSystemService", NULL, h_system_service },
  { ACT, "startActivity", NULL, h_start_activity },
  { ACT, "getSafeAreaInsets", NULL, h_safe_insets },
  { ACT, "isAlphaTransparencyEnabled", NULL, h_zero },
  { ACT, "setUseHiddenInputField", NULL, h_zero },
  { ACT, "setFullscreenParameters", NULL, h_zero },
  { ACT, "showSoftInput", NULL, h_zero },
  { ACT, "hideSoftInput", NULL, h_zero },
  { ACT, "resetSoftInput", NULL, h_zero },
  { ACT, "getConnectivity", NULL, h_zero },
  { ACT, "isAppInstalled", NULL, h_zero },
  { "com/defold/firebase/crashlytics/FirebaseCrashlyticsJNI", "initialize", NULL, h_true },
  { ACT, "getGameControllerDeviceIds", NULL, h_empty_int_array },
  { ACT, "getContentResolver", NULL, h_singleton_by_return },
  { ACT, "getApplicationInfo", NULL, h_singleton_by_return },
  { ACT, "getResources", NULL, h_singleton_by_return },
  { ACT, "getWindow", NULL, h_singleton_by_return },
  { ACT, "getPackageManager", NULL, h_singleton_by_return },
  { "java/lang/ClassLoader", "loadClass", NULL, h_loadclass },
  { "android/view/WindowManager", "getDefaultDisplay", NULL, h_display },
  { "android/view/Display", "getRefreshRate", NULL, h_refresh_rate },
  { "java/io/File", "getPath", NULL, h_file_path },
  { "java/io/File", "getAbsolutePath", NULL, h_file_path },
  { "java/io/File", "getCanonicalPath", NULL, h_file_path },
  { "java/io/File", "toString", NULL, h_file_path },
  { "java/io/File", "getParent", NULL, h_file_parent },
  { "java/io/File", "exists", NULL, h_true },
  { "java/io/File", "mkdirs", NULL, h_true },
  { "java/util/Locale", "getDefault", NULL, h_locale_default },
  { "java/util/Locale", "getLanguage", NULL, h_locale_language },
  { "java/util/Locale", "getCountry", NULL, h_locale_country },
  { "java/util/Locale", "toLanguageTag", NULL, h_locale_tag },
  { "java/util/Locale", "toString", NULL, h_locale_string },
  { "android/os/LocaleList", "toLanguageTags", NULL, h_localelist_tags },
  { "android/os/LocaleList", "size", NULL, h_localelist_size },
  { "android/os/LocaleList", "isEmpty", NULL, h_localelist_empty },
  { "android/os/LocaleList", "get", NULL, h_locale_default },
  { "android/os/LocaleList", "getDefault", NULL, h_system_locales },
  { "android/app/LocaleManager", "getApplicationLocales", NULL, h_app_locales },
  { "android/app/LocaleManager", "getSystemLocales", NULL, h_system_locales },
  { "android/content/res/Resources", "getSystem", NULL, h_singleton_by_return },
  { "android/content/res/Resources", "getConfiguration", NULL, h_singleton_by_return },
  { "android/content/res/Resources", "getDisplayMetrics", NULL, h_singleton_by_return },
  { "android/content/res/Resources", "getIdentifier", NULL, h_get_identifier },
  { "android/content/res/Resources", "openRawResource", NULL, h_open_raw },
  { "android/content/res/Configuration", "getLocales", NULL, h_system_locales },
  { "java/io/InputStream", "read", NULL, h_stream_read },
  { "java/io/InputStream", "available", NULL, h_stream_available },
  { "java/io/InputStream", "close", NULL, h_stream_close },
  { "android/net/Uri", "parse", NULL, h_uri_parse },
  { "android/net/Uri", "toString", NULL, h_sval_string },
  { "android/content/Intent", "getStringArrayExtra", NULL, h_string_array_extra },
  { "android/content/Intent", "addFlags", NULL, h_self },
  { "android/content/Intent", "setFlags", NULL, h_self },
  { "android/view/KeyEvent", "getUnicodeChar", NULL, h_unicode_char },
  { "android/provider/Settings$Secure", "getString", NULL, h_android_id },
  { "java/lang/String", "getBytes", NULL, h_string_getbytes },
  { "com/defold/sound/Sound", "getSampleRate", NULL, h_sample_rate },
  { "com/defold/sound/Sound", "getFramesPerBuffer", NULL, h_frames_per_buffer },
  { "com/defold/sound/SoundManager", "isMusicPlaying", NULL, h_zero },
  { "com/defold/window/WindowJNI", "enableScreenDimming", NULL, h_dimming },
  { "com/defold/window/WindowJNI", "disableScreenDimming", NULL, h_dimming },
  { "com/defold/window/WindowJNI", "isScreenDimmingEnabled", NULL, h_dimming_enabled },
  { "com/defold/admob/AdmobJNI", NULL, NULL, h_admob },
  { "com/defold/gpgs/GpgsJNI", NULL, NULL, h_gpgs },
  { "com/defold/firebase/FirebaseJNI", NULL, NULL, h_firebase },
  { "com/defold/firebase/analytics/FirebaseAnalyticsJNI", NULL, NULL, h_firebase_analytics },
  { "*/IapGooglePlay", NULL, NULL, h_iap },
  { "*/IapAmazon", NULL, NULL, h_iap },
  { "com/defold/push/Push", "getInstance", NULL, h_push_instance },
  { "com/defold/push/Push", NULL, NULL, h_push },
  { "*/BkLangSplits", NULL, NULL, h_lang_splits },
  { NULL, "toString", "()Ljava/lang/String;", h_to_string },
};

JHandler jni_defold_method(const char *cls, const char *name, const char *sig) {
  for (size_t i = 0; i < sizeof(k_methods) / sizeof(k_methods[0]); i++) {
    const MethodDef *d = &k_methods[i];
    if (!cls_match(d->cls, cls)) continue;
    if (d->name && strcmp(d->name, name)) continue;
    if (d->sig && strcmp(d->sig, sig)) continue;
    if (!strcmp(name, "<init>")) return NULL;
    return d->fn;
  }
  return NULL;
}

jvalue jni_defold_field(const char *cls, const char *name, const char *sig, JObj *self) {
  (void)self; (void)sig;
  if (!strcmp(cls, "android/util/DisplayMetrics")) {
    if (!strcmp(name, "xdpi") || !strcmp(name, "ydpi")) return jv_float(237.0f);
    if (!strcmp(name, "density") || !strcmp(name, "scaledDensity")) return jv_float(1.5f);
    if (!strcmp(name, "densityDpi")) return jv_int(240);
    if (!strcmp(name, "widthPixels")) return jv_int(BK_RENDER_W);
    if (!strcmp(name, "heightPixels")) return jv_int(BK_RENDER_H);
  } else if (!strcmp(cls, "android/content/pm/ApplicationInfo")) {
    if (!strcmp(name, "FLAG_DEBUGGABLE")) return jv_int(2);
    if (!strcmp(name, "flags")) return jv_int(0);
    if (!strcmp(name, "dataDir") || !strcmp(name, "sourceDir") || !strcmp(name, "nativeLibraryDir"))
      return jv_str(app_files_dir());
  } else if (!strcmp(cls, "android/os/Build")) {
    if (!strcmp(name, "MANUFACTURER") || !strcmp(name, "BRAND")) return jv_str("Nintendo");
    if (!strcmp(name, "MODEL")) return jv_str("Switch");
    if (!strcmp(name, "FINGERPRINT")) return jv_str("Nintendo/nx/nx:11/NX/1:user/release-keys");
    return jv_str("nx");
  } else if (!strcmp(cls, "android/os/Build$VERSION")) {
    if (!strcmp(name, "SDK_INT")) return jv_int(BK_SDK_INT);
    if (!strcmp(name, "RELEASE")) return jv_str("11");
    if (!strcmp(name, "CODENAME")) return jv_str("REL");
    return jv_str("1");
  } else if (!strcmp(cls, "android/content/Intent")) {
    if (!strcmp(name, "ACTION_VIEW")) return jv_str("android.intent.action.VIEW");
  } else if (!strcmp(cls, "android/content/res/Configuration")) {
    if (!strcmp(name, "locale")) return jv_obj(g_locale);
    if (!strcmp(name, "orientation")) return jv_int(2);
  }
  LOGT("jni", "field %s.%s (%s) -> default", cls, name, sig);
  return jv();
}

void jni_defold_construct(JObj *obj, const char *cls, const char *sig, const jvalue *args) {
  if (!strcmp(cls, "android/view/KeyEvent") && !strcmp(sig, "(JJIIIIIIII)V")) {
    obj->ival[0] = args[3].i;   /* keyCode   */
    obj->ival[1] = args[5].i;   /* metaState */
  } else if (!strcmp(cls, "android/content/Intent")) {
    for (int i = 0; sig[i]; i++) if (!strncmp(sig + i, "Landroid/net/Uri;", 17)) { obj->ref[0] = args[1].l; break; }
  } else if (!strcmp(cls, "java/io/File") && !strcmp(sig, "(Ljava/lang/String;)V")) {
    const char *p = jni_str(args[0].l);
    obj->sval = strdup(p ? p : "");
  }
}

/* ---- setup ----------------------------------------------------------------------------------- */

static const char *system_language_tag(void) {
  static const char *k_tags[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-Hans", "ko", "en-US", "pt-BR",
    "ru", "zh-Hant", "en-GB", "fr", "es", "zh-Hans", "zh-Hant", "pt-BR",
  };
  const char *tag = "en-US";
  if (R_SUCCEEDED(setInitialize())) {
    u64 code = 0;
    SetLanguage lang;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang)) &&
        (int)lang >= 0 && (size_t)lang < sizeof(k_tags) / sizeof(k_tags[0]))
      tag = k_tags[lang];
    setExit();
  }
  return tag;
}

static int is_cjk(const char *tag) {
  return !strncmp(tag, "ja", 2) || !strncmp(tag, "ko", 2) || !strncmp(tag, "zh", 2);
}

static void set_language(const char *tag) {
  snprintf(g_tag, sizeof(g_tag), "%s", tag);
  snprintf(g_lang, sizeof(g_lang), "%.2s", tag);
  const char *dash = strchr(tag, '-');
  if (!dash) snprintf(g_country, sizeof(g_country), "%s", !strcmp(g_lang, "en") ? "US" : "");
  else if (!strcmp(dash + 1, "Hans")) snprintf(g_country, sizeof(g_country), "CN");
  else if (!strcmp(dash + 1, "Hant")) snprintf(g_country, sizeof(g_country), "TW");
  else snprintf(g_country, sizeof(g_country), "%.2s", dash + 1);
  if (!g_country[0]) {
    for (int i = 0; i < 2 && g_lang[i]; i++) g_country[i] = (char)(g_lang[i] - 32);
    g_country[2] = 0;
  }
}

void jni_defold_init(void *so_mod) {
  g_mod = so_mod;
  mutexInit(&g_def_mu);
  g_activity = jni_new_object("com/dynamo/android/DefoldActivity");
  g_intent = jni_new_object("android/content/Intent");
  g_locale = jni_new_object("java/util/Locale");

  static const char *k_pack_names[] = { "bk_lang_pack", "bk_lang_pack.zip", "bk_lang_pack.bin",
                                        "res/raw/bk_lang_pack", "res/raw/bk_lang_pack.zip" };
  for (size_t i = 0; i < sizeof(k_pack_names) / sizeof(k_pack_names[0]); i++) {
    snprintf(g_langpack_path, sizeof(g_langpack_path), "%s/%s", app_game_dir(), k_pack_names[i]);
    if (bk_file_exists(g_langpack_path)) { g_langpack_present = 1; break; }
  }

  /* modules/lang_pack.lua only fetches and mounts bk_lang_pack for its CJK set;
   * every other language boots straight from the base archive ("latin_ready").
   * The pack ships in the Play language splits and is not obtainable, so
   * Japanese, Korean and Chinese are answered with English rather than left to
   * spend BOOT_FETCH_ATTEMPTS x BOOT_RETRY_SECONDS failing to fetch it. */
  const char *want = g_settings.language;
  int explicit_lang = strcmp(want, "auto") != 0;
  const char *tag = explicit_lang ? want : system_language_tag();
  if (is_cjk(tag) && !g_langpack_present) {
    LOGI("language %s needs bk_lang_pack, which this build has no source for; using English", tag);
    tag = "en-US";
    explicit_lang = 0;
  }
  set_language(tag);
  LOGI("language %s (lang %s country %s), lang pack %s", g_tag, g_lang, g_country,
       g_langpack_present ? g_langpack_path : "absent");

  const char *args[8 + SETTINGS_MAX_LINES];
  char bufs[4 + SETTINGS_MAX_LINES][200];
  int n = 0, nb = 0;
  args[n++] = "--nx-host";
  args[n++] = "--config=graphics.verify_graphics_calls=0";
  if (explicit_lang) {
    const char *code = (!strcmp(g_tag, "en-US") || !strcmp(g_tag, "en-GB")) ? "en" : g_tag;
    snprintf(bufs[nb], 200, "--config=bk.lang_override=%s", code);
    args[n++] = bufs[nb++];
  }
  if (strcmp(g_settings.variant, "default") != 0 && g_settings.variant[0]) {
    snprintf(bufs[nb], 200, "--config=bk.variant=%s", g_settings.variant);
    args[n++] = bufs[nb++];
  }
  for (int i = 0; i < g_settings.n_config; i++) {
    snprintf(bufs[nb], 200, "--config=%s", g_settings.config[i]);
    args[n++] = bufs[nb++];
  }
  g_intent_args = jni_new_array('L', n);
  g_intent_args->immortal = 1;
  for (int i = 0; i < n; i++) {
    JObj *s = jni_new_string(args[i]);
    s->immortal = 1;
    ((JObj **)g_intent_args->elems)[i] = s;
    LOGI("engine arg: %s", args[i]);
  }
}
