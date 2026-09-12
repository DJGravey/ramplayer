#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double rp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void *rp_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) rp_fatal("out of memory (%zu bytes)", n);
    return p;
}

void *rp_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) rp_fatal("out of memory (%zu x %zu bytes)", count, size);
    return p;
}

char *rp_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = rp_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void rp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ramplayer: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void rp_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ramplayer: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

const char *rp_human_bytes(char *buf, size_t bufsz, uint64_t bytes)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < RP_ARRAY_LEN(unit) - 1) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, bufsz, "%.0f %s", v, unit[u]);
    else
        snprintf(buf, bufsz, "%.*f %s", v < 10.0 ? 2 : (v < 100.0 ? 1 : 0), v, unit[u]);
    return buf;
}

int rp_parse_bytes(const char *s, uint64_t *out)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v < 0.0) return 0;

    while (*end == ' ') end++;
    uint64_t mult = 1;
    switch (toupper((unsigned char)*end)) {
    case 'K': mult = 1024ull; end++; break;
    case 'M': mult = 1024ull * 1024; end++; break;
    case 'G': mult = 1024ull * 1024 * 1024; end++; break;
    case 'T': mult = 1024ull * 1024 * 1024 * 1024; end++; break;
    case '\0': break;
    default: return 0;
    }
    if (toupper((unsigned char)*end) == 'B') end++;
    while (*end == ' ') end++;
    if (*end != '\0') return 0;

    *out = (uint64_t)(v * (double)mult);
    return 1;
}
