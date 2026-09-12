/* util.h - small helpers shared across ramplayer. */
#ifndef RP_UTIL_H
#define RP_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define RP_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define RP_MAX(a, b)       ((a) > (b) ? (a) : (b))
#define RP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define RP_ARRAY_LEN(a)    ((int)(sizeof(a) / sizeof((a)[0])))

/* Monotonic clock, in seconds. */
double rp_now(void);

/* Allocation wrappers that abort on failure; use rp_try_* where a failed
 * allocation should be handled rather than fatal (frame pixel buffers). */
void *rp_xmalloc(size_t n);
void *rp_xcalloc(size_t count, size_t size);
char *rp_strdup(const char *s);

void rp_log(const char *fmt, ...);
void rp_fatal(const char *fmt, ...);

/* Formats a byte count as e.g. "1.4 GB" into buf, which is returned. */
const char *rp_human_bytes(char *buf, size_t bufsz, uint64_t bytes);

/* Parses sizes such as "1G", "512M", "1536MB", "2048". Returns 0 on failure. */
int rp_parse_bytes(const char *s, uint64_t *out);

/* Positive modulo: result is always in [0, m). */
static inline int rp_wrap(int v, int m)
{
    if (m <= 0) return 0;
    v %= m;
    return v < 0 ? v + m : v;
}

#endif /* RP_UTIL_H */
