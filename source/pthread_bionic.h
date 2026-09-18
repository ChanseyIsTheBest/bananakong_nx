#ifndef BK_PTHREAD_BIONIC_H
#define BK_PTHREAD_BIONIC_H

#include <stddef.h>
#include <stdint.h>

/* Bionic pthread ABI on top of libnx primitives.
 *
 * The game allocates bionic-sized objects (mutex 40 bytes, cond 48, rwlock 56,
 * attr 56) and may initialise them statically. The first 4 bytes of each hold
 * a tagged index into a table of real objects, created on first use; the
 * static initialisers (0, 0x4000 recursive, 0x8000 errorcheck) survive until
 * then. Only 32-bit atomics touch that storage (it is 4-byte aligned). */

void threads_init(void);
void threads_pause_all(void);   /* suspend every game-created thread (exit path) */

int  bn_pthread_create(void *out, const void *attr, void *(*fn)(void *), void *arg);
int  bn_pthread_attr_init(void *attr);
int  bn_pthread_attr_destroy(void *attr);
int  bn_pthread_attr_setstacksize(void *attr, size_t size);
int  bn_pthread_attr_getstacksize(const void *attr, size_t *size);
int  bn_pthread_attr_setdetachstate(void *attr, int state);
int  bn_pthread_getattr_np(unsigned long thread, void *attr);
int  bn_pthread_setname_np(unsigned long thread, const char *name);

int  bn_pthread_mutexattr_init(void *attr);
int  bn_pthread_mutexattr_destroy(void *attr);
int  bn_pthread_mutexattr_settype(void *attr, int type);
int  bn_pthread_mutex_init(void *m, const void *attr);
int  bn_pthread_mutex_destroy(void *m);
int  bn_pthread_mutex_lock(void *m);
int  bn_pthread_mutex_trylock(void *m);
int  bn_pthread_mutex_unlock(void *m);

int  bn_pthread_cond_init(void *c, const void *attr);
int  bn_pthread_cond_destroy(void *c);
int  bn_pthread_cond_wait(void *c, void *m);
int  bn_pthread_cond_signal(void *c);
int  bn_pthread_cond_broadcast(void *c);

int  bn_pthread_rwlock_rdlock(void *rw);
int  bn_pthread_rwlock_wrlock(void *rw);
int  bn_pthread_rwlock_unlock(void *rw);

int  bn_pthread_once(int *once, void (*fn)(void));
int  bn_pthread_key_create(int *key, void (*dtor)(void *));
int  bn_pthread_key_delete(int key);
void *bn_pthread_getspecific(int key);
int  bn_pthread_setspecific(int key, const void *value);

unsigned long bn_pthread_self(void);
int  bn_pthread_join(unsigned long thread, void **ret);
int  bn_pthread_detach(unsigned long thread);

#endif
