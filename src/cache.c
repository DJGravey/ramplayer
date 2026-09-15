#include "cache.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "reader.h"
#include "util.h"

typedef struct {
    _Atomic unsigned char state; /* read without the lock for display only */
    Image                *img;   /* owned by the cache while state == READY */
} Entry;

struct Cache {
    const Sequence *seq;
    const ColorLUT *lut;
    int             count;

    RpMutex mu;
    RpCond  cv;

    Entry *entries;

    /* The resident frames as a compact list, so looking for an eviction
     * victim costs one pass over what is actually in memory instead of one
     * pass over the whole sequence. */
    int *ready_list;
    int *ready_pos; /* frame -> index in ready_list, or -1 */
    int  ready_count;

    size_t bytes_used;
    size_t bytes_limit;
    size_t est_bytes; /* working estimate of one frame, for admission tests */

    int focus;
    int dir;

    int n_loading;
    int n_failed;

    int        stop;
    atomic_int abort_flag; /* polled inside the decoder so quitting is prompt */

    RpThread *threads;
    int       n_threads;

    void (*wakeup)(void *);
    void  *wakeup_ud;

    char last_error[256];
    int  error_count;
};

/* ---- priority ------------------------------------------------------------
 * 0 is the frame under the playhead, 1 the next one in the direction of play,
 * and so on around the loop. Higher is less valuable. */
static inline int prio_of(const Cache *c, int f)
{
    return rp_wrap((f - c->focus) * c->dir, c->count);
}

/* ---- resident set bookkeeping ------------------------------------------- */

static void ready_add(Cache *c, int f)
{
    c->ready_pos[f] = c->ready_count;
    c->ready_list[c->ready_count++] = f;
}

static void ready_remove(Cache *c, int f)
{
    int pos = c->ready_pos[f];
    if (pos < 0) return;
    int last = c->ready_list[--c->ready_count];
    c->ready_list[pos] = last;
    c->ready_pos[last] = pos;
    c->ready_pos[f] = -1;
}

/* The resident frame furthest from the playhead. The frame being displayed is
 * never a candidate. */
static int find_victim_locked(const Cache *c, int *out_prio)
{
    int best = -1, best_prio = -1;
    for (int i = 0; i < c->ready_count; i++) {
        int f = c->ready_list[i];
        if (f == c->focus) continue;
        int p = prio_of(c, f);
        if (p > best_prio) {
            best_prio = p;
            best = f;
        }
    }
    *out_prio = best_prio;
    return best;
}

static void evict_locked(Cache *c, int f)
{
    Entry *e = &c->entries[f];
    if (!e->img) return;
    ready_remove(c, f);
    c->bytes_used -= e->img->bytes;
    image_unref(e->img);
    e->img = NULL;
    atomic_store_explicit(&e->state, CACHE_EMPTY, memory_order_relaxed);
}

/* ---- scheduling --------------------------------------------------------- */

/* Picks the most valuable frame that is worth fetching right now, or -1 if
 * there is nothing useful to do. Candidates are visited in priority order, so
 * the moment one fails the admission test every later one would too. */
static int pick_locked(Cache *c)
{
    int n = c->count;
    if (n <= 0) return -1;

    /* Look no further ahead than the budget could hold, plus enough slack to
     * keep every thread busy. The division stays in size_t and is clamped
     * before narrowing, so a very large budget cannot overflow the counter and
     * leave the scan doing nothing. */
    size_t est = c->est_bytes ? c->est_bytes : 1;
    size_t cap_frames = c->bytes_limit / est;
    int max_scan = (cap_frames >= (size_t)n) ? n
                                             : (int)cap_frames + c->n_threads + 4;
    if (max_scan > n) max_scan = n;

    int victim_prio = -1, have_victim = 0;

    for (int k = 0; k < max_scan; k++) {
        int f = rp_wrap(c->focus + c->dir * k, n);
        if (atomic_load_explicit(&c->entries[f].state, memory_order_relaxed) != CACHE_EMPTY)
            continue;

        if (c->bytes_used + est <= c->bytes_limit) return f;
        if (c->ready_count == 0 && c->n_loading == 0) return f; /* one frame always fits */

        if (!have_victim) {
            find_victim_locked(c, &victim_prio);
            have_victim = 1;
        }
        return (victim_prio > k) ? f : -1;
    }
    return -1;
}

static void insert_locked(Cache *c, int f, Image *im)
{
    int k = prio_of(c, f);

    while (c->bytes_used + im->bytes > c->bytes_limit) {
        int vprio;
        int v = find_victim_locked(c, &vprio);
        if (v < 0 || vprio <= k) break;
        evict_locked(c, v);
    }

    if (c->bytes_used + im->bytes > c->bytes_limit && c->ready_count > 0) {
        /* Everything resident is nearer the playhead than this frame, so the
         * budget is better spent where it already is. Leaving the entry EMPTY
         * is safe: pick_locked() applies the same test and will not
         * immediately ask for it again. */
        image_unref(im);
        atomic_store_explicit(&c->entries[f].state, CACHE_EMPTY, memory_order_relaxed);
        return;
    }

    c->entries[f].img = im;
    c->bytes_used += im->bytes;
    ready_add(c, f);
    atomic_store_explicit(&c->entries[f].state, CACHE_READY, memory_order_release);

    if (im->bytes > c->est_bytes) c->est_bytes = im->bytes;
}

/* ---- loader threads ----------------------------------------------------- */

static void worker_main(void *arg)
{
    Cache *c = arg;
    DecodeScratch *scratch = decode_scratch_create();
    char err[256];

    for (;;) {
        rp_mutex_lock(&c->mu);
        int f = -1;
        for (;;) {
            if (c->stop) break;
            f = pick_locked(c);
            if (f >= 0) break;
            rp_cond_wait(&c->cv, &c->mu);
        }
        if (c->stop) {
            rp_mutex_unlock(&c->mu);
            break;
        }
        atomic_store_explicit(&c->entries[f].state, CACHE_LOADING, memory_order_relaxed);
        c->n_loading++;
        const char *path = c->seq->frames[f].path; /* sequence is immutable */
        rp_mutex_unlock(&c->mu);

        Image *im = reader_load(path, c->lut, scratch, &c->abort_flag, err, sizeof err);

        rp_mutex_lock(&c->mu);
        c->n_loading--;
        if (im) {
            insert_locked(c, f, im);
        } else if (c->stop) {
            /* Cancelled on the way out; not a real failure. */
            atomic_store_explicit(&c->entries[f].state, CACHE_EMPTY, memory_order_relaxed);
        } else {
            atomic_store_explicit(&c->entries[f].state, CACHE_FAILED, memory_order_relaxed);
            c->n_failed++;
            if (c->error_count++ == 0)
                snprintf(c->last_error, sizeof c->last_error, "%s", err);
        }
        void (*wakeup)(void *) = c->wakeup;
        void *wakeup_ud = c->wakeup_ud;
        rp_cond_broadcast(&c->cv);
        rp_mutex_unlock(&c->mu);

        /* Called outside the lock: it pushes an event into the windowing
         * layer, which takes locks of its own. */
        if (wakeup) wakeup(wakeup_ud);
    }

    decode_scratch_destroy(scratch);
}

/* ---- public API --------------------------------------------------------- */

Cache *cache_create(const Sequence *seq, const ColorLUT *lut,
                    size_t byte_limit, int n_workers, size_t est_frame_bytes)
{
    if (!seq || seq->count <= 0) return NULL;
    if (n_workers < 1) n_workers = 1;

    size_t est = est_frame_bytes ? est_frame_bytes : (size_t)1920 * 1080 * 4;

    /* Playback needs the frame on screen plus the one after it, so a budget
     * too small for two frames could never advance. Raise it rather than
     * stalling, and say so. */
    if (byte_limit < est * 2) {
        char want[32], got[32];
        rp_human_bytes(got, sizeof got, byte_limit);
        rp_human_bytes(want, sizeof want, est * 2);
        rp_log("budget %s holds less than two %dx%d frames; using %s instead",
               got, seq->width, seq->height, want);
        byte_limit = est * 2;
    }

    Cache *c = rp_xcalloc(1, sizeof *c);
    c->seq         = seq;
    c->lut         = lut;
    c->count       = seq->count;
    c->bytes_limit = byte_limit;
    c->est_bytes   = est;
    c->dir         = 1;
    c->n_threads   = n_workers;

    c->entries    = rp_xcalloc((size_t)c->count, sizeof *c->entries);
    c->ready_list = rp_xmalloc((size_t)c->count * sizeof *c->ready_list);
    c->ready_pos  = rp_xmalloc((size_t)c->count * sizeof *c->ready_pos);
    for (int i = 0; i < c->count; i++) {
        atomic_init(&c->entries[i].state, CACHE_EMPTY);
        c->ready_pos[i] = -1;
    }
    atomic_init(&c->abort_flag, 0);

    rp_mutex_init(&c->mu);
    rp_cond_init(&c->cv);

    c->threads = rp_xcalloc((size_t)n_workers, sizeof *c->threads);

    /* Spawn with the lock held. Workers take it as the first thing they do, so
     * this publishes the finished cache to them and lets the final thread
     * count be written before anybody can read it. */
    rp_mutex_lock(&c->mu);
    int started = 0;
    for (int i = 0; i < n_workers; i++) {
        if (!rp_thread_create(&c->threads[i], worker_main, c)) break;
        started++;
    }
    c->n_threads = started;
    if (started == 0) c->stop = 1;
    rp_mutex_unlock(&c->mu);

    if (started == 0) {
        rp_log("could not start any loader threads");
        cache_destroy(c);
        return NULL;
    }
    return c;
}

void cache_destroy(Cache *c)
{
    if (!c) return;

    atomic_store(&c->abort_flag, 1);
    rp_mutex_lock(&c->mu);
    c->stop = 1;
    rp_cond_broadcast(&c->cv);
    rp_mutex_unlock(&c->mu);

    for (int i = 0; i < c->n_threads; i++) rp_thread_join(&c->threads[i]);

    for (int i = 0; i < c->count; i++) image_unref(c->entries[i].img);

    rp_cond_destroy(&c->cv);
    rp_mutex_destroy(&c->mu);
    free(c->threads);
    free(c->entries);
    free(c->ready_list);
    free(c->ready_pos);
    free(c);
}

void cache_set_focus(Cache *c, int frame, int direction)
{
    if (!c) return;
    if (direction == 0) direction = 1;
    frame = RP_CLAMP(frame, 0, c->count - 1);

    rp_mutex_lock(&c->mu);
    int changed = (c->focus != frame) || (c->dir != direction);
    c->focus = frame;
    c->dir   = direction;
    if (changed) rp_cond_broadcast(&c->cv);
    rp_mutex_unlock(&c->mu);
}

Image *cache_acquire(Cache *c, int frame)
{
    if (!c || frame < 0 || frame >= c->count) return NULL;

    rp_mutex_lock(&c->mu);
    Image *im = NULL;
    if (atomic_load_explicit(&c->entries[frame].state, memory_order_relaxed) == CACHE_READY)
        im = image_ref(c->entries[frame].img);
    rp_mutex_unlock(&c->mu);
    return im;
}

int cache_frame_state(const Cache *c, int frame)
{
    if (!c || frame < 0 || frame >= c->count) return CACHE_EMPTY;
    return (int)atomic_load_explicit(&c->entries[frame].state, memory_order_relaxed);
}

void cache_get_stats(Cache *c, CacheStats *out)
{
    memset(out, 0, sizeof *out);
    if (!c) return;

    rp_mutex_lock(&c->mu);
    out->ready       = c->ready_count;
    out->loading     = c->n_loading;
    out->failed      = c->n_failed;
    out->bytes_used  = c->bytes_used;
    out->bytes_limit = c->bytes_limit;
    out->capacity_frames = c->est_bytes ? (int)(c->bytes_limit / c->est_bytes) : 0;
    rp_mutex_unlock(&c->mu);
}

void cache_set_wakeup(Cache *c, void (*fn)(void *), void *userdata)
{
    if (!c) return;
    rp_mutex_lock(&c->mu);
    c->wakeup    = fn;
    c->wakeup_ud = userdata;
    rp_mutex_unlock(&c->mu);
}

const char *cache_last_error(Cache *c)
{
    if (!c) return NULL;
    return c->error_count ? c->last_error : NULL;
}
