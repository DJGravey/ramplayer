/* platform.h - the few operating system services ramplayer needs.
 *
 * Threads, a monotonic clock, a short sleep, directory listing and a file
 * kind test. platform.c holds a POSIX backend and a Windows backend; nothing
 * else in the program includes an operating system header.
 *
 * On Windows the three synchronisation types are declared here as a single
 * pointer-sized word, which is what SRWLOCK, CONDITION_VARIABLE and HANDLE
 * are, so callers can embed them by value without this header pulling in
 * windows.h. platform.c checks the sizes with a static assertion.
 */
#ifndef RP_PLATFORM_H
#define RP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
typedef struct { void *h; } RpMutex;  /* SRWLOCK */
typedef struct { void *h; } RpCond;   /* CONDITION_VARIABLE */
typedef struct { void *h; } RpThread; /* HANDLE */
#else
#include <pthread.h>
typedef struct { pthread_mutex_t m; } RpMutex;
typedef struct { pthread_cond_t c; } RpCond;
typedef struct { pthread_t t; } RpThread;
#endif

/* ---- threads ------------------------------------------------------------ */

void rp_mutex_init(RpMutex *m);
void rp_mutex_destroy(RpMutex *m);
void rp_mutex_lock(RpMutex *m);
void rp_mutex_unlock(RpMutex *m);

void rp_cond_init(RpCond *c);
void rp_cond_destroy(RpCond *c);
/* Atomically releases m, waits for a broadcast, and re-takes m. Spurious
 * wake-ups are possible, as with pthreads; callers loop on their predicate. */
void rp_cond_wait(RpCond *c, RpMutex *m);
void rp_cond_broadcast(RpCond *c);

/* Starts fn(arg) on a new thread. Returns 1 on success, 0 if the thread could
 * not be started, in which case t is left untouched and must not be joined. */
int  rp_thread_create(RpThread *t, void (*fn)(void *), void *arg);
void rp_thread_join(RpThread *t);

/* ---- time --------------------------------------------------------------- */

/* The monotonic clock, rp_now(), is declared in util.h and implemented here. */

void rp_sleep_ms(int ms);

/* ---- files -------------------------------------------------------------- */

/* Paths are UTF-8 on every platform, which is what SDL hands main() and what
 * OpenEXRCore expects. */

typedef struct RpDir RpDir;

/* Opens a directory for listing, or returns NULL if it cannot be read. */
RpDir *rp_dir_open(const char *path);
/* The next entry name, or NULL when the listing is exhausted. "." and ".." are
 * never returned. The string is valid until the next call on the same RpDir. */
const char *rp_dir_next(RpDir *d);
void rp_dir_close(RpDir *d);

int rp_is_dir(const char *path);
int rp_is_file(const char *path);

/* Whole-file reading, front to back. */
typedef struct RpFile RpFile;

/* Opens for sequential reading, or returns NULL. */
RpFile *rp_file_open(const char *path);
/* The file's size in bytes, or -1. */
int64_t rp_file_size(RpFile *f);
/* Reads exactly n bytes from the current position. Returns 1 on success, 0 if
 * the file ended early or the read failed. */
int  rp_file_read(RpFile *f, void *buf, size_t n);
void rp_file_close(RpFile *f);

/* Both separators count on Windows; only '/' elsewhere. */
int rp_is_path_sep(char c);

#endif /* RP_PLATFORM_H */
