/* jni_host.c -- the JNIEnv / JavaVM function tables. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "jni_host.h"
#include "jni_slots.h"
#include "log.h"
#include "util.h"

#define JOBJ_MAGIC   0x4A4F424Au
#define JMETH_MAGIC  0x4A4D5448u
#define JFIELD_MAGIC 0x4A464C44u

static void  *g_env_table[JNI_SLOT_COUNT];
static void **g_env = g_env_table;
static void  *g_vm_table[JVM_SLOT_COUNT];
static void **g_vm = g_vm_table;

void *jni_env(void) { return (void *)&g_env; }
void *jni_vm(void)  { return (void *)&g_vm; }

static Mutex g_mu;
typedef struct ClassEnt { JObj obj; struct ClassEnt *next; } ClassEnt;
static ClassEnt *g_classes;
static JMethod  *g_methods;
static JField   *g_fields;

static jvalue jzero(void) { jvalue v; memset(&v, 0, sizeof(v)); return v; }
static int valid(const JObj *o) { return o && o->magic == JOBJ_MAGIC; }

/* ---- objects ---------------------------------------------------------------- */

JObj *jni_class(const char *name) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", name ? name : "?");
  for (char *p = buf; *p; p++) if (*p == '.') *p = '/';
  mutexLock(&g_mu);
  for (ClassEnt *c = g_classes; c; c = c->next)
    if (!strcmp(c->obj.cls, buf)) { mutexUnlock(&g_mu); return &c->obj; }
  ClassEnt *c = bk_xcalloc(1, sizeof(*c));
  c->obj.magic = JOBJ_MAGIC;
  c->obj.kind = JO_CLASS;
  c->obj.immortal = 1;
  c->obj.cls = strdup(buf);
  c->next = g_classes;
  g_classes = c;
  mutexUnlock(&g_mu);
  return &c->obj;
}

JObj *jni_new_object(const char *cls) {
  JObj *o = bk_xcalloc(1, sizeof(*o));
  o->magic = JOBJ_MAGIC;
  o->kind = JO_OBJECT;
  o->immortal = 1;
  o->refs = 1;
  o->cls = jni_class(cls)->cls;
  return o;
}

JObj *jni_new_string(const char *utf) {
  if (!utf) return NULL;
  JObj *o = bk_xcalloc(1, sizeof(*o));
  o->magic = JOBJ_MAGIC;
  o->kind = JO_STRING;
  o->refs = 1;
  o->cls = "java/lang/String";
  o->utf = strdup(utf);
  o->length = (int32_t)strlen(utf);
  return o;
}

static size_t elem_size(char e) {
  switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8;
  }
}

JObj *jni_new_array(char elem, int32_t len) {
  if (len < 0) return NULL;
  JObj *o = bk_xcalloc(1, sizeof(*o));
  o->magic = JOBJ_MAGIC;
  o->kind = JO_ARRAY;
  o->refs = 1;
  o->elem = elem;
  o->cls = "[";
  o->length = len;
  o->elems = bk_xcalloc((size_t)(len ? len : 1), elem_size(elem));
  return o;
}

const char *jni_str(const JObj *s) { return (valid(s) && s->kind == JO_STRING) ? s->utf : NULL; }

/* Strings and arrays die when their last reference goes; the free is delayed
 * through a quarantine ring because native code sometimes keeps using a
 * buffer just past DeleteLocalRef. Everything else is immortal. */
#define QUARANTINE 1024
static JObj *g_quarantine[QUARANTINE];
static int g_qpos;

void jni_release(JObj *o) {
  if (!valid(o) || o->immortal || o->kind == JO_CLASS) return;
  if (__sync_sub_and_fetch(&o->refs, 1) != 0) return;
  mutexLock(&g_mu);
  if (o->queued) { mutexUnlock(&g_mu); return; }
  o->queued = 1;
  JObj *old = g_quarantine[g_qpos];
  g_quarantine[g_qpos] = o;
  g_qpos = (g_qpos + 1) % QUARANTINE;
  if (old) old->queued = 0;
  int free_old = old && old->refs <= 0;
  mutexUnlock(&g_mu);
  if (free_old) {
    old->magic = 0;
    free(old->utf);
    free(old->elems);
    free(old);
  }
}

static JObj *retain(JObj *o) {
  if (valid(o) && !o->immortal) __sync_add_and_fetch(&o->refs, 1);
  return o;
}

/* ---- method / field IDs --------------------------------------------------------- */

static void parse_sig(JMethod *m) {
  const char *p = m->sig;
  m->nargs = 0;
  m->ret = 'V';
  if (!p || *p != '(') return;
  p++;
  while (*p && *p != ')') {
    char t = *p;
    if (t == '[') {
      while (*p == '[') p++;
      if (*p == 'L') { p = strchr(p, ';'); if (!p) return; }
      p++;
    } else if (t == 'L') {
      p = strchr(p, ';');
      if (!p) return;
      p++;
    } else {
      p++;
    }
    if (m->nargs < 32) m->args[m->nargs++] = t;
  }
  if (*p == ')') {
    p++;
    m->ret = (*p == 'L' || *p == '[') ? 'L' : (*p ? *p : 'V');
  }
}

static jvalue h_unhandled(JMethod *m, JObj *self, const jvalue *args) {
  (void)self; (void)args;
  if (m->calls++ < 2) LOGT("jni", "unhandled call %s.%s%s", m->cls, m->name, m->sig);
  return jzero();
}

static JMethod *get_method(JObj *clazz, const char *name, const char *sig, int is_static) {
  if (!name || !sig) return NULL;
  const char *cls = valid(clazz) ? (clazz->kind == JO_CLASS ? clazz->cls : jni_class(clazz->cls)->cls) : "?";
  mutexLock(&g_mu);
  for (JMethod *m = g_methods; m; m = m->next) {
    if (m->cls == cls && m->is_static == is_static && !strcmp(m->name, name) && !strcmp(m->sig, sig)) {
      mutexUnlock(&g_mu);
      return m;
    }
  }
  JMethod *m = bk_xcalloc(1, sizeof(*m));
  m->magic = JMETH_MAGIC;
  m->cls = cls;
  m->name = strdup(name);
  m->sig = strdup(sig);
  m->is_static = is_static;
  parse_sig(m);
  m->fn = jni_defold_method(cls, name, sig);
  if (!m->fn) m->fn = h_unhandled;
  m->next = g_methods;
  g_methods = m;
  mutexUnlock(&g_mu);
  LOGT("jni", "%s %s.%s%s%s", is_static ? "GetStaticMethodID" : "GetMethodID", cls, name, sig,
       (m->fn == h_unhandled && strcmp(name, "<init>") != 0) ? "  [unhandled]" : "");
  return m;
}

static JField *get_field(JObj *clazz, const char *name, const char *sig, int is_static) {
  if (!name || !sig) return NULL;
  const char *cls = valid(clazz) ? (clazz->kind == JO_CLASS ? clazz->cls : jni_class(clazz->cls)->cls) : "?";
  mutexLock(&g_mu);
  for (JField *f = g_fields; f; f = f->next) {
    if (f->cls == cls && f->is_static == is_static && !strcmp(f->name, name) && !strcmp(f->sig, sig)) {
      mutexUnlock(&g_mu);
      return f;
    }
  }
  JField *f = bk_xcalloc(1, sizeof(*f));
  f->magic = JFIELD_MAGIC;
  f->cls = cls;
  f->name = strdup(name);
  f->sig = strdup(sig);
  f->is_static = is_static;
  f->next = g_fields;
  g_fields = f;
  mutexUnlock(&g_mu);
  LOGT("jni", "GetFieldID %s.%s %s", cls, name, sig);
  return f;
}

static void args_from_va(const JMethod *m, jvalue *out, va_list ap) {
  for (int i = 0; i < m->nargs && i < 32; i++) {
    out[i] = jzero();
    switch (m->args[i]) {
      case 'Z': case 'B': case 'C': case 'S': case 'I': out[i].i = va_arg(ap, int); break;
      case 'J': out[i].j = va_arg(ap, long long); break;
      case 'F': out[i].f = (float)va_arg(ap, double); break;
      case 'D': out[i].d = va_arg(ap, double); break;
      default:  out[i].l = va_arg(ap, JObj *); break;
    }
  }
}

static jvalue invoke(JObj *self, JMethod *m, const jvalue *args) {
  if (!m || m->magic != JMETH_MAGIC) { LOGE("JNI call through an invalid method ID"); return jzero(); }
  jvalue none[1];
  memset(none, 0, sizeof(none));
  return m->fn(m, self, args ? args : none);
}

/* ---- JNIEnv functions ------------------------------------------------------------ */

static long jni_trap(void) { LOG_ONCE("JNI: unimplemented function called (see jni_slots.h)"); return 0; }

static jint GetVersion(void *env) { (void)env; return 0x00010006; }
static JObj *DefineClass(void *env, const char *n, JObj *l, const jbyte *b, jsize len) { (void)env; (void)n; (void)l; (void)b; (void)len; return NULL; }
static JObj *FindClass(void *env, const char *name) { (void)env; return name ? jni_class(name) : NULL; }
static JObj *GetSuperclass(void *env, JObj *c) { (void)env; (void)c; return jni_class("java/lang/Object"); }
static jboolean IsAssignableFrom(void *env, JObj *a, JObj *b) { (void)env; (void)a; (void)b; return 1; }
static jint Throw(void *env, JObj *t) { (void)env; (void)t; return 0; }
static jint ThrowNew(void *env, JObj *c, const char *msg) { (void)env; LOGE("ThrowNew %s: %s", valid(c) ? c->cls : "?", msg ? msg : ""); return 0; }
static JObj *ExceptionOccurred(void *env) { (void)env; return NULL; }
static void ExceptionDescribe(void *env) { (void)env; }
static void ExceptionClear(void *env) { (void)env; }
static void FatalError(void *env, const char *msg) { (void)env; LOGE("JNI FatalError: %s", msg ? msg : ""); abort(); }
static jint PushLocalFrame(void *env, jint cap) { (void)env; (void)cap; return 0; }
static JObj *PopLocalFrame(void *env, JObj *res) { (void)env; return res; }
static JObj *NewGlobalRef(void *env, JObj *o) { (void)env; return retain(o); }
static void DeleteGlobalRef(void *env, JObj *o) { (void)env; jni_release(o); }
static void DeleteLocalRef(void *env, JObj *o) { (void)env; jni_release(o); }
static jboolean IsSameObject(void *env, JObj *a, JObj *b) { (void)env; return a == b; }
static JObj *NewLocalRef(void *env, JObj *o) { (void)env; return retain(o); }
static jint EnsureLocalCapacity(void *env, jint cap) { (void)env; (void)cap; return 0; }
static JObj *AllocObject(void *env, JObj *c) { (void)env; return valid(c) ? jni_new_object(c->cls) : NULL; }

static JObj *NewObjectA(void *env, JObj *c, JMethod *m, const jvalue *args) {
  (void)env;
  if (!valid(c)) return NULL;
  JObj *o = jni_new_object(c->cls);
  if (m && m->magic == JMETH_MAGIC) jni_defold_construct(o, o->cls, m->sig, args);
  return o;
}
static JObj *NewObjectV(void *env, JObj *c, JMethod *m, va_list ap) {
  jvalue a[32];
  if (m && m->magic == JMETH_MAGIC) args_from_va(m, a, ap); else memset(a, 0, sizeof(a));
  return NewObjectA(env, c, m, a);
}
static JObj *NewObject(void *env, JObj *c, JMethod *m, ...) {
  va_list ap; va_start(ap, m);
  JObj *o = NewObjectV(env, c, m, ap);
  va_end(ap);
  return o;
}

static JObj *GetObjectClass(void *env, JObj *o) {
  (void)env;
  if (!valid(o)) return NULL;
  if (o->kind == JO_CLASS) return jni_class("java/lang/Class");
  return jni_class(o->cls);
}

static jboolean IsInstanceOf(void *env, JObj *o, JObj *c) {
  (void)env;
  if (!o) return 1;
  if (!valid(o) || !valid(c)) return 0;
  if (!strcmp(c->cls, "java/lang/Object")) return 1;
  if (o->kind == JO_STRING) return !strcmp(c->cls, "java/lang/String") || !strcmp(c->cls, "java/lang/CharSequence");
  if (o->kind == JO_ARRAY) return c->cls[0] == '[';
  if (o == jni_activity()) return 1;
  return !strcmp(o->cls, c->cls);
}

static JMethod *GetMethodID(void *env, JObj *c, const char *n, const char *s) { (void)env; return get_method(c, n, s, 0); }
static JMethod *GetStaticMethodID(void *env, JObj *c, const char *n, const char *s) { (void)env; return get_method(c, n, s, 1); }
static JField *GetFieldID(void *env, JObj *c, const char *n, const char *s) { (void)env; return get_field(c, n, s, 0); }
static JField *GetStaticFieldID(void *env, JObj *c, const char *n, const char *s) { (void)env; return get_field(c, n, s, 1); }

#define CALL_FAMILY(Name, jtype, member)                                                          \
  static jtype Call##Name##MethodA(void *env, JObj *o, JMethod *m, const jvalue *a) {              \
    (void)env; return invoke(o, m, a).member; }                                                    \
  static jtype Call##Name##MethodV(void *env, JObj *o, JMethod *m, va_list ap) {                   \
    jvalue a[32]; memset(a, 0, sizeof(a)); if (m && m->magic == JMETH_MAGIC) args_from_va(m, a, ap); \
    return Call##Name##MethodA(env, o, m, a); }                                                    \
  static jtype Call##Name##Method(void *env, JObj *o, JMethod *m, ...) {                           \
    va_list ap; va_start(ap, m); jtype r = Call##Name##MethodV(env, o, m, ap); va_end(ap); return r; } \
  static jtype CallNonvirtual##Name##MethodA(void *env, JObj *o, JObj *c, JMethod *m, const jvalue *a) { \
    (void)c; return Call##Name##MethodA(env, o, m, a); }                                           \
  static jtype CallNonvirtual##Name##MethodV(void *env, JObj *o, JObj *c, JMethod *m, va_list ap) { \
    (void)c; return Call##Name##MethodV(env, o, m, ap); }                                          \
  static jtype CallNonvirtual##Name##Method(void *env, JObj *o, JObj *c, JMethod *m, ...) {        \
    (void)c; va_list ap; va_start(ap, m); jtype r = Call##Name##MethodV(env, o, m, ap); va_end(ap); return r; } \
  static jtype CallStatic##Name##MethodA(void *env, JObj *c, JMethod *m, const jvalue *a) {        \
    return Call##Name##MethodA(env, c, m, a); }                                                    \
  static jtype CallStatic##Name##MethodV(void *env, JObj *c, JMethod *m, va_list ap) {             \
    return Call##Name##MethodV(env, c, m, ap); }                                                   \
  static jtype CallStatic##Name##Method(void *env, JObj *c, JMethod *m, ...) {                     \
    va_list ap; va_start(ap, m); jtype r = Call##Name##MethodV(env, c, m, ap); va_end(ap); return r; }

CALL_FAMILY(Object, JObj *, l)
CALL_FAMILY(Boolean, jboolean, z)
CALL_FAMILY(Byte, jbyte, b)
CALL_FAMILY(Char, jchar, c)
CALL_FAMILY(Short, jshort, s)
CALL_FAMILY(Int, jint, i)
CALL_FAMILY(Long, jlong, j)
CALL_FAMILY(Float, jfloat, f)
CALL_FAMILY(Double, jdouble, d)

static void CallVoidMethodA(void *env, JObj *o, JMethod *m, const jvalue *a) { (void)env; invoke(o, m, a); }
static void CallVoidMethodV(void *env, JObj *o, JMethod *m, va_list ap) {
  jvalue a[32]; memset(a, 0, sizeof(a));
  if (m && m->magic == JMETH_MAGIC) args_from_va(m, a, ap);
  CallVoidMethodA(env, o, m, a);
}
static void CallVoidMethod(void *env, JObj *o, JMethod *m, ...) { va_list ap; va_start(ap, m); CallVoidMethodV(env, o, m, ap); va_end(ap); }
static void CallNonvirtualVoidMethodA(void *env, JObj *o, JObj *c, JMethod *m, const jvalue *a) { (void)c; CallVoidMethodA(env, o, m, a); }
static void CallNonvirtualVoidMethodV(void *env, JObj *o, JObj *c, JMethod *m, va_list ap) { (void)c; CallVoidMethodV(env, o, m, ap); }
static void CallNonvirtualVoidMethod(void *env, JObj *o, JObj *c, JMethod *m, ...) { (void)c; va_list ap; va_start(ap, m); CallVoidMethodV(env, o, m, ap); va_end(ap); }
static void CallStaticVoidMethodA(void *env, JObj *c, JMethod *m, const jvalue *a) { CallVoidMethodA(env, c, m, a); }
static void CallStaticVoidMethodV(void *env, JObj *c, JMethod *m, va_list ap) { CallVoidMethodV(env, c, m, ap); }
static void CallStaticVoidMethod(void *env, JObj *c, JMethod *m, ...) { va_list ap; va_start(ap, m); CallVoidMethodV(env, c, m, ap); va_end(ap); }

static jvalue field_get(JObj *self, JField *f) {
  if (!f || f->magic != JFIELD_MAGIC) return jzero();
  return jni_defold_field(f->cls, f->name, f->sig, self);
}

#define FIELD_FAMILY(Name, jtype, member)                                                          \
  static jtype Get##Name##Field(void *env, JObj *o, JField *f) { (void)env; return field_get(o, f).member; } \
  static void Set##Name##Field(void *env, JObj *o, JField *f, jtype v) { (void)env; (void)o; (void)f; (void)v; } \
  static jtype GetStatic##Name##Field(void *env, JObj *c, JField *f) { (void)env; (void)c; return field_get(NULL, f).member; } \
  static void SetStatic##Name##Field(void *env, JObj *c, JField *f, jtype v) { (void)env; (void)c; (void)f; (void)v; }

FIELD_FAMILY(Object, JObj *, l)
FIELD_FAMILY(Boolean, jboolean, z)
FIELD_FAMILY(Byte, jbyte, b)
FIELD_FAMILY(Char, jchar, c)
FIELD_FAMILY(Short, jshort, s)
FIELD_FAMILY(Int, jint, i)
FIELD_FAMILY(Long, jlong, j)
FIELD_FAMILY(Float, jfloat, f)
FIELD_FAMILY(Double, jdouble, d)

/* strings */
static size_t utf8_decode(const char *s, size_t i, uint32_t *cp) {
  const unsigned char *u = (const unsigned char *)s + i;
  if (u[0] < 0x80) { *cp = u[0]; return 1; }
  if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3; }
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) { *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4; }
  *cp = u[0];
  return 1;
}

static jchar *to_utf16(const char *s, jsize *out_len) {
  size_t n = strlen(s);
  jchar *w = bk_xmalloc((n + 1) * 2 * sizeof(jchar));
  jsize k = 0;
  for (size_t i = 0; i < n;) {
    uint32_t cp;
    i += utf8_decode(s, i, &cp);
    if (cp >= 0x10000) { cp -= 0x10000; w[k++] = (jchar)(0xD800 | (cp >> 10)); w[k++] = (jchar)(0xDC00 | (cp & 0x3FF)); }
    else w[k++] = (jchar)cp;
  }
  w[k] = 0;
  if (out_len) *out_len = k;
  return w;
}

static JObj *NewString(void *env, const jchar *u, jsize len) {
  (void)env;
  char *buf = bk_xmalloc((size_t)(len > 0 ? len : 0) * 3 + 1);
  size_t o = 0;
  for (jsize i = 0; i < len; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len) { cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
    if (cp < 0x80) buf[o++] = (char)cp;
    else if (cp < 0x800) { buf[o++] = (char)(0xC0 | (cp >> 6)); buf[o++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { buf[o++] = (char)(0xE0 | (cp >> 12)); buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[o++] = (char)(0x80 | (cp & 0x3F)); }
    else { buf[o++] = (char)(0xF0 | (cp >> 18)); buf[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[o++] = (char)(0x80 | (cp & 0x3F)); }
  }
  buf[o] = 0;
  JObj *s = jni_new_string(buf);
  free(buf);
  return s;
}
static jsize GetStringLength(void *env, JObj *s) {
  (void)env;
  const char *u = jni_str(s);
  if (!u) return 0;
  jsize n; free(to_utf16(u, &n));
  return n;
}
static const jchar *GetStringChars(void *env, JObj *s, jboolean *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 1;
  const char *u = jni_str(s);
  return u ? to_utf16(u, NULL) : NULL;
}
static void ReleaseStringChars(void *env, JObj *s, const jchar *c) { (void)env; (void)s; free((void *)c); }
static JObj *NewStringUTF(void *env, const char *u) { (void)env; return jni_new_string(u); }
static jsize GetStringUTFLength(void *env, JObj *s) { (void)env; const char *u = jni_str(s); return u ? (jsize)strlen(u) : 0; }
static const char *GetStringUTFChars(void *env, JObj *s, jboolean *isCopy) { (void)env; if (isCopy) *isCopy = 0; return jni_str(s); }
static void ReleaseStringUTFChars(void *env, JObj *s, const char *c) { (void)env; (void)s; (void)c; }
static void GetStringRegion(void *env, JObj *s, jsize start, jsize len, jchar *buf) {
  (void)env;
  const char *u = jni_str(s);
  if (!u || len <= 0) return;
  jsize n;
  jchar *w = to_utf16(u, &n);
  if (start >= 0 && start + len <= n) memcpy(buf, w + start, (size_t)len * sizeof(jchar));
  free(w);
}
static void GetStringUTFRegion(void *env, JObj *s, jsize start, jsize len, char *buf) {
  (void)env;
  const char *u = jni_str(s);
  if (!u || len < 0 || start < 0 || (size_t)(start + len) > strlen(u)) return;
  memcpy(buf, u + start, (size_t)len);
  buf[len] = 0;
}
static const jchar *GetStringCritical(void *env, JObj *s, jboolean *isCopy) { return GetStringChars(env, s, isCopy); }
static void ReleaseStringCritical(void *env, JObj *s, const jchar *c) { ReleaseStringChars(env, s, c); }

/* arrays */
static jsize GetArrayLength(void *env, JObj *a) { (void)env; return (valid(a) && a->kind == JO_ARRAY) ? a->length : 0; }
static JObj *NewObjectArray(void *env, jsize len, JObj *c, JObj *init) {
  (void)env; (void)c;
  JObj *a = jni_new_array('L', len);
  if (a) for (jsize i = 0; i < len; i++) ((JObj **)a->elems)[i] = init;
  return a;
}
static JObj *GetObjectArrayElement(void *env, JObj *a, jsize i) {
  (void)env;
  if (!valid(a) || a->kind != JO_ARRAY || a->elem != 'L' || i < 0 || i >= a->length) return NULL;
  return retain(((JObj **)a->elems)[i]);
}
static void SetObjectArrayElement(void *env, JObj *a, jsize i, JObj *v) {
  (void)env;
  if (!valid(a) || a->kind != JO_ARRAY || a->elem != 'L' || i < 0 || i >= a->length) return;
  ((JObj **)a->elems)[i] = retain(v);
}

#define ARRAY_FAMILY(Name, jtype, ch)                                                              \
  static JObj *New##Name##Array(void *env, jsize len) { (void)env; return jni_new_array(ch, len); } \
  static jtype *Get##Name##ArrayElements(void *env, JObj *a, jboolean *isCopy) {                    \
    (void)env; if (isCopy) *isCopy = 0; return (valid(a) && a->kind == JO_ARRAY) ? (jtype *)a->elems : NULL; } \
  static void Release##Name##ArrayElements(void *env, JObj *a, jtype *e, jint mode) { (void)env; (void)a; (void)e; (void)mode; } \
  static void Get##Name##ArrayRegion(void *env, JObj *a, jsize st, jsize len, jtype *buf) {         \
    (void)env; if (valid(a) && a->kind == JO_ARRAY && st >= 0 && len > 0 && st + len <= a->length) \
      memcpy(buf, (jtype *)a->elems + st, (size_t)len * sizeof(jtype)); }                           \
  static void Set##Name##ArrayRegion(void *env, JObj *a, jsize st, jsize len, const jtype *buf) {   \
    (void)env; if (valid(a) && a->kind == JO_ARRAY && st >= 0 && len > 0 && st + len <= a->length) \
      memcpy((jtype *)a->elems + st, buf, (size_t)len * sizeof(jtype)); }

ARRAY_FAMILY(Boolean, jboolean, 'Z')
ARRAY_FAMILY(Byte, jbyte, 'B')
ARRAY_FAMILY(Char, jchar, 'C')
ARRAY_FAMILY(Short, jshort, 'S')
ARRAY_FAMILY(Int, jint, 'I')
ARRAY_FAMILY(Long, jlong, 'J')
ARRAY_FAMILY(Float, jfloat, 'F')
ARRAY_FAMILY(Double, jdouble, 'D')

static void *GetPrimitiveArrayCritical(void *env, JObj *a, jboolean *isCopy) {
  (void)env; if (isCopy) *isCopy = 0;
  return (valid(a) && a->kind == JO_ARRAY) ? a->elems : NULL;
}
static void ReleasePrimitiveArrayCritical(void *env, JObj *a, void *e, jint mode) { (void)env; (void)a; (void)e; (void)mode; }

static jint RegisterNatives(void *env, JObj *c, const void *methods, jint n) {
  (void)env; (void)methods;
  LOGT("jni", "RegisterNatives(%s, %d)", valid(c) ? c->cls : "?", n);
  return 0;
}
static jint UnregisterNatives(void *env, JObj *c) { (void)env; (void)c; return 0; }
static jint MonitorEnter(void *env, JObj *o) { (void)env; (void)o; return 0; }
static jint MonitorExit(void *env, JObj *o) { (void)env; (void)o; return 0; }
static jint GetJavaVM(void *env, void **vm) { (void)env; *vm = jni_vm(); return 0; }
static JObj *NewWeakGlobalRef(void *env, JObj *o) { (void)env; return retain(o); }
static void DeleteWeakGlobalRef(void *env, JObj *o) { (void)env; jni_release(o); }
static jboolean ExceptionCheck(void *env) { (void)env; return 0; }
static JObj *NewDirectByteBuffer(void *env, void *addr, jlong cap) {
  (void)env;
  JObj *o = jni_new_object("java/nio/ByteBuffer");
  o->native = addr;
  o->ival[0] = cap;
  return o;
}
static void *GetDirectBufferAddress(void *env, JObj *o) { (void)env; return valid(o) ? o->native : NULL; }
static jlong GetDirectBufferCapacity(void *env, JObj *o) { (void)env; return valid(o) ? o->ival[0] : -1; }
static int GetObjectRefType(void *env, JObj *o) { (void)env; return valid(o) ? 1 : 0; }

/* ---- JavaVM ------------------------------------------------------------------------ */

static jint DestroyJavaVM(void *vm) { (void)vm; return 0; }
static jint AttachCurrentThread(void *vm, void **penv, void *args) { (void)vm; (void)args; if (penv) *penv = jni_env(); return 0; }
static jint DetachCurrentThread(void *vm) { (void)vm; return 0; }
static jint GetEnv(void *vm, void **penv, jint version) { (void)vm; (void)version; if (penv) *penv = jni_env(); return 0; }

/* ---- tables --------------------------------------------------------------------------- */

#define REG(n) g_env_table[JNI_##n] = (void *)(n)
#define REG_CALLS(N) REG(Call##N##Method); REG(Call##N##MethodV); REG(Call##N##MethodA); \
  REG(CallNonvirtual##N##Method); REG(CallNonvirtual##N##MethodV); REG(CallNonvirtual##N##MethodA); \
  REG(CallStatic##N##Method); REG(CallStatic##N##MethodV); REG(CallStatic##N##MethodA)
#define REG_FIELDS(N) REG(Get##N##Field); REG(Set##N##Field); REG(GetStatic##N##Field); REG(SetStatic##N##Field)
#define REG_ARRAYS(N) REG(New##N##Array); REG(Get##N##ArrayElements); REG(Release##N##ArrayElements); \
  REG(Get##N##ArrayRegion); REG(Set##N##ArrayRegion)

void jni_init(void) {
  mutexInit(&g_mu);
  for (int i = 0; i < JNI_SLOT_COUNT; i++) g_env_table[i] = (void *)jni_trap;
  for (int i = 0; i < 4; i++) g_env_table[i] = NULL;

  REG(GetVersion); REG(DefineClass); REG(FindClass); REG(GetSuperclass); REG(IsAssignableFrom);
  REG(Throw); REG(ThrowNew); REG(ExceptionOccurred); REG(ExceptionDescribe); REG(ExceptionClear);
  REG(FatalError); REG(PushLocalFrame); REG(PopLocalFrame); REG(NewGlobalRef); REG(DeleteGlobalRef);
  REG(DeleteLocalRef); REG(IsSameObject); REG(NewLocalRef); REG(EnsureLocalCapacity); REG(AllocObject);
  REG(NewObject); REG(NewObjectV); REG(NewObjectA); REG(GetObjectClass); REG(IsInstanceOf);
  REG(GetMethodID); REG(GetStaticMethodID); REG(GetFieldID); REG(GetStaticFieldID);
  REG_CALLS(Object); REG_CALLS(Boolean); REG_CALLS(Byte); REG_CALLS(Char); REG_CALLS(Short);
  REG_CALLS(Int); REG_CALLS(Long); REG_CALLS(Float); REG_CALLS(Double); REG_CALLS(Void);
  REG_FIELDS(Object); REG_FIELDS(Boolean); REG_FIELDS(Byte); REG_FIELDS(Char); REG_FIELDS(Short);
  REG_FIELDS(Int); REG_FIELDS(Long); REG_FIELDS(Float); REG_FIELDS(Double);
  REG(NewString); REG(GetStringLength); REG(GetStringChars); REG(ReleaseStringChars);
  REG(NewStringUTF); REG(GetStringUTFLength); REG(GetStringUTFChars); REG(ReleaseStringUTFChars);
  REG(GetStringRegion); REG(GetStringUTFRegion); REG(GetStringCritical); REG(ReleaseStringCritical);
  REG(GetArrayLength); REG(NewObjectArray); REG(GetObjectArrayElement); REG(SetObjectArrayElement);
  REG_ARRAYS(Boolean); REG_ARRAYS(Byte); REG_ARRAYS(Char); REG_ARRAYS(Short);
  REG_ARRAYS(Int); REG_ARRAYS(Long); REG_ARRAYS(Float); REG_ARRAYS(Double);
  REG(GetPrimitiveArrayCritical); REG(ReleasePrimitiveArrayCritical);
  REG(RegisterNatives); REG(UnregisterNatives); REG(MonitorEnter); REG(MonitorExit); REG(GetJavaVM);
  REG(NewWeakGlobalRef); REG(DeleteWeakGlobalRef); REG(ExceptionCheck);
  REG(NewDirectByteBuffer); REG(GetDirectBufferAddress); REG(GetDirectBufferCapacity); REG(GetObjectRefType);

  g_vm_table[JVM_DestroyJavaVM] = (void *)DestroyJavaVM;
  g_vm_table[JVM_AttachCurrentThread] = (void *)AttachCurrentThread;
  g_vm_table[JVM_DetachCurrentThread] = (void *)DetachCurrentThread;
  g_vm_table[JVM_GetEnv] = (void *)GetEnv;
  g_vm_table[JVM_AttachCurrentThreadAsDaemon] = (void *)AttachCurrentThread;
}
