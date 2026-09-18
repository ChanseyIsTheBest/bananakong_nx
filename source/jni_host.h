#ifndef BK_JNI_HOST_H
#define BK_JNI_HOST_H

#include <stdint.h>

/* A minimal JVM for the game's JNI traffic. Objects are JObj; method and
 * field IDs are interned JMethod/JField records whose behaviour comes from
 * jni_defold.c. The function tables are laid out by jni_slots.h. */

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

typedef struct JObj JObj;
typedef union { jboolean z; jbyte b; jchar c; jshort s; jint i; jlong j; jfloat f; jdouble d; JObj *l; } jvalue;

enum { JO_CLASS = 1, JO_OBJECT, JO_STRING, JO_ARRAY };

struct JObj {
  uint32_t magic;
  uint8_t  kind;
  uint8_t  immortal;
  char     elem;          /* array element type: Z B C S I J F D L */
  uint8_t  queued;        /* sitting in the release quarantine */
  int32_t  refs;
  const char *cls;        /* class name, slash-separated (interned for classes) */
  char    *utf;           /* JO_STRING payload */
  int32_t  length;        /* JO_ARRAY length */
  void    *elems;         /* JO_ARRAY storage */
  void    *native;        /* behaviour-specific (FILE*, buffer address, ...) */
  int64_t  ival[4];
  JObj    *ref[4];
  char    *sval;
};

typedef struct JMethod JMethod;
typedef jvalue (*JHandler)(JMethod *m, JObj *self, const jvalue *args);

struct JMethod {
  uint32_t magic;
  const char *cls, *name, *sig;
  int  is_static;
  char ret;
  int  nargs;
  char args[32];
  int  calls;
  JHandler fn;
  JMethod *next;
};

typedef struct JField {
  uint32_t magic;
  const char *cls, *name, *sig;
  int is_static;
  struct JField *next;
} JField;

void  jni_init(void);
void *jni_env(void);   /* JNIEnv*  */
void *jni_vm(void);    /* JavaVM*  */

JObj *jni_class(const char *name);
JObj *jni_new_object(const char *cls);
JObj *jni_new_string(const char *utf);
JObj *jni_new_array(char elem, int32_t len);
const char *jni_str(const JObj *s);
void  jni_release(JObj *o);

/* jni_defold.c */
JHandler jni_defold_method(const char *cls, const char *name, const char *sig);
jvalue   jni_defold_field(const char *cls, const char *name, const char *sig, JObj *self);
void     jni_defold_construct(JObj *obj, const char *cls, const char *sig, const jvalue *args);
void     jni_defold_init(void *so_mod);
JObj    *jni_activity(void);
void     jni_pump(void);

#endif
