/* platform.c - POSIX and Windows backends for platform.h. */
#include "platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared between the backends ---------------------------------------- */

/* What a new thread is given: the caller's function and argument, on the
 * heap so it outlives rp_thread_create(). The trampoline frees it. */
typedef struct {
    void (*fn)(void *);
    void *arg;
} ThreadStart;

static ThreadStart *thread_start_new(void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->fn  = fn;
    s->arg = arg;
    return s;
}

static void thread_start_run(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
}

static int is_dot_entry(const char *name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

#ifdef _WIN32

/* ======================================================================== */
/* Windows                                                                  */
/* ======================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "RpMutex must hold an SRWLOCK");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "RpCond must hold a CONDITION_VARIABLE");
_Static_assert(sizeof(HANDLE) == sizeof(void *), "RpThread must hold a HANDLE");

/* ---- threads ------------------------------------------------------------ */

void rp_mutex_init(RpMutex *m)    { InitializeSRWLock((SRWLOCK *)&m->h); }
void rp_mutex_destroy(RpMutex *m) { (void)m; /* SRW locks need no teardown */ }
void rp_mutex_lock(RpMutex *m)    { AcquireSRWLockExclusive((SRWLOCK *)&m->h); }
void rp_mutex_unlock(RpMutex *m)  { ReleaseSRWLockExclusive((SRWLOCK *)&m->h); }

void rp_cond_init(RpCond *c)    { InitializeConditionVariable((CONDITION_VARIABLE *)&c->h); }
void rp_cond_destroy(RpCond *c) { (void)c; }

void rp_cond_wait(RpCond *c, RpMutex *m)
{
    SleepConditionVariableSRW((CONDITION_VARIABLE *)&c->h, (SRWLOCK *)&m->h, INFINITE, 0);
}

void rp_cond_broadcast(RpCond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)&c->h); }

static unsigned __stdcall thread_trampoline(void *p)
{
    thread_start_run(p);
    return 0;
}

int rp_thread_create(RpThread *t, void (*fn)(void *), void *arg)
{
    ThreadStart *s = thread_start_new(fn, arg);
    if (!s) return 0;
    uintptr_t h = _beginthreadex(NULL, 0, thread_trampoline, s, 0, NULL);
    if (h == 0) {
        free(s);
        return 0;
    }
    t->h = (void *)h;
    return 1;
}

void rp_thread_join(RpThread *t)
{
    WaitForSingleObject((HANDLE)t->h, INFINITE);
    CloseHandle((HANDLE)t->h);
    t->h = NULL;
}

/* ---- time --------------------------------------------------------------- */

double rp_now(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

void rp_sleep_ms(int ms) { Sleep(ms > 0 ? (DWORD)ms : 0); }

/* ---- files -------------------------------------------------------------- */

/* Paths are UTF-8 throughout the program: SDL2main hands main() UTF-8
 * arguments and OpenEXRCore opens files as UTF-8, so this backend talks to
 * the wide-character file APIs and converts at the boundary. */

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

struct RpDir {
    HANDLE           h;         /* INVALID_HANDLE_VALUE for an empty listing */
    WIN32_FIND_DATAW fd;
    int              pending;   /* the entry in fd has not been handed out yet */
    int              exhausted;
    char             name[MAX_PATH * 3 + 1]; /* UTF-8 of fd.cFileName */
};

RpDir *rp_dir_open(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return NULL;
    size_t n = wcslen(wpath);
    wchar_t *pattern = malloc((n + 3) * sizeof *pattern);
    if (!pattern) {
        free(wpath);
        return NULL;
    }
    memcpy(pattern, wpath, n * sizeof *pattern);
    free(wpath);
    if (n > 0 && pattern[n - 1] != L'/' && pattern[n - 1] != L'\\') pattern[n++] = L'/';
    pattern[n++] = L'*';
    pattern[n]   = L'\0';

    RpDir *d = calloc(1, sizeof *d);
    if (!d) {
        free(pattern);
        return NULL;
    }
    d->h = FindFirstFileW(pattern, &d->fd);
    free(pattern);
    if (d->h == INVALID_HANDLE_VALUE) {
        /* A directory with nothing in it at all (a bare volume root has no
         * "." or "..") is an empty listing, not a failure to open. */
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            d->exhausted = 1;
            return d;
        }
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}

const char *rp_dir_next(RpDir *d)
{
    for (;;) {
        if (d->exhausted) return NULL;
        if (!d->pending && !FindNextFileW(d->h, &d->fd)) {
            d->exhausted = 1;
            return NULL;
        }
        d->pending = 0;
        int n = WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1,
                                    d->name, (int)sizeof d->name, NULL, NULL);
        if (n <= 0) continue; /* unrepresentable name: skip it */
        if (!is_dot_entry(d->name)) return d->name;
    }
}

void rp_dir_close(RpDir *d)
{
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
}

static DWORD attributes_of(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w) return INVALID_FILE_ATTRIBUTES;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

int rp_is_dir(const char *path)
{
    DWORD a = attributes_of(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int rp_is_file(const char *path)
{
    DWORD a = attributes_of(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int rp_is_path_sep(char c) { return c == '/' || c == '\\'; }

struct RpFile {
    HANDLE h;
};

RpFile *rp_file_open(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w) return NULL;
    /* Share everything: a frame still being written by a renderer, or held
     * open by another viewer, must still open here as it does elsewhere. */
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    RpFile *f = malloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

int64_t rp_file_size(RpFile *f)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz)) return -1;
    return (int64_t)sz.QuadPart;
}

/* Reads in 1 MB requests: the SMB redirector pipelines requests of that size
 * well, where one request for a whole 12 MB file measured a third slower. */
int rp_file_read(RpFile *f, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        DWORD want = n > (1u << 20) ? (1u << 20) : (DWORD)n;
        DWORD got  = 0;
        if (!ReadFile(f->h, p, want, &got, NULL) || got == 0) return 0;
        p += got;
        n -= got;
    }
    return 1;
}

void rp_file_close(RpFile *f)
{
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

#else

/* ======================================================================== */
/* POSIX                                                                    */
/* ======================================================================== */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- threads ------------------------------------------------------------ */

void rp_mutex_init(RpMutex *m)    { pthread_mutex_init(&m->m, NULL); }
void rp_mutex_destroy(RpMutex *m) { pthread_mutex_destroy(&m->m); }
void rp_mutex_lock(RpMutex *m)    { pthread_mutex_lock(&m->m); }
void rp_mutex_unlock(RpMutex *m)  { pthread_mutex_unlock(&m->m); }

void rp_cond_init(RpCond *c)              { pthread_cond_init(&c->c, NULL); }
void rp_cond_destroy(RpCond *c)           { pthread_cond_destroy(&c->c); }
void rp_cond_wait(RpCond *c, RpMutex *m)  { pthread_cond_wait(&c->c, &m->m); }
void rp_cond_broadcast(RpCond *c)         { pthread_cond_broadcast(&c->c); }

static void *thread_trampoline(void *p)
{
    thread_start_run(p);
    return NULL;
}

int rp_thread_create(RpThread *t, void (*fn)(void *), void *arg)
{
    ThreadStart *s = thread_start_new(fn, arg);
    if (!s) return 0;
    if (pthread_create(&t->t, NULL, thread_trampoline, s) != 0) {
        free(s);
        return 0;
    }
    return 1;
}

void rp_thread_join(RpThread *t) { pthread_join(t->t, NULL); }

/* ---- time --------------------------------------------------------------- */

double rp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void rp_sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- files -------------------------------------------------------------- */

struct RpDir {
    DIR *d;
};

RpDir *rp_dir_open(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    RpDir *d = malloc(sizeof *d);
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->d = dir;
    return d;
}

const char *rp_dir_next(RpDir *d)
{
    struct dirent *de;
    while ((de = readdir(d->d)) != NULL)
        if (!is_dot_entry(de->d_name)) return de->d_name;
    return NULL;
}

void rp_dir_close(RpDir *d)
{
    if (!d) return;
    closedir(d->d);
    free(d);
}

int rp_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int rp_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int rp_is_path_sep(char c) { return c == '/'; }

struct RpFile {
    int fd;
};

RpFile *rp_file_open(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    RpFile *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    return f;
}

int64_t rp_file_size(RpFile *f)
{
    struct stat st;
    if (fstat(f->fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int rp_file_read(RpFile *f, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t got = read(f->fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return 0;
        p += got;
        n -= (size_t)got;
    }
    return 1;
}

void rp_file_close(RpFile *f)
{
    if (!f) return;
    close(f->fd);
    free(f);
}

#endif
