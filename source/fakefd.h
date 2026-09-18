#ifndef BK_FAKEFD_H
#define BK_FAKEFD_H

/* In-process pipes for the native_app_glue command pipe (and anything else
 * the engine pipe()s). Descriptors start at FAKE_FD_BASE so they can never
 * collide with newlib's. */
#define FAKE_FD_BASE 0x40000000
#define MAX_FAKE_FDS 128

int  fakefd_is_fake(int fd);
int  fakefd_pipe(int fds[2]);
long fakefd_write(int fd, const void *buf, unsigned long n);
long fakefd_read(int fd, void *buf, unsigned long n);
int  fakefd_close(int fd);
int  fakefd_poll_state(int fd, int *readable, int *writable);

/* Called (outside the fakefd lock) whenever a pipe gains data or loses a
 * writer, so ALooper_pollOnce can wake. */
void fakefd_set_notify(void (*fn)(void));

/* O_NONBLOCK, as set through fcntl(F_SETFL): reads of an empty pipe and
 * writes to a full one fail with EAGAIN instead of blocking. */
void fakefd_set_nonblock(int fd, int nonblock);
int  fakefd_get_nonblock(int fd);

#endif
