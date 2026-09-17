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

/* One per loader thread. `frame` is the frame the thread delivers next, -1
 * when idle, written and read under the cache lock. `abort` is the flag the
 * decoder polls: raised under the lock by cache_set_focus() when the frame
 * has fallen out of reach, and by cache_destroy() on the way out. A claim is
 * the run of frames the thread has marked LOADING for itself. */
typedef struct {
    Cache     *cache;
    Reader    *reader;
    int        index;
    int        frame;
    int        claim_from, claim_to;
    int        claimed; /* frames of the claim not yet delivered or released */
    atomic_int abort;
    int        just_aborted; /* the last decode was abandoned; this one is
                                allowed to finish, so a playhead that never
                                stops moving cannot starve the cache */
} Worker;

struct Cache {
    const Sequence *seq;
    const ColorLUT *lut;
    int             count;

    RpMutex mu;
    RpCond  cv;

    Entry *entries;
    int   *claim; /* per frame: the worker that holds it LOADING, or -1 */

    /* The resident frames as a compact list, so looking for an eviction
     * victim costs one pass over what is actually in memory instead of one
     * pass over the whole sequence. */
    int *ready_list;
    int *ready_pos; /* frame -> index in ready_list, or -1 */
    int  ready_count;

    size_t bytes_used;
    size_t bytes_limit;
    size_t est_bytes; /* working estimate of one frame, for admission tests */
    int    cap_frames; /* frames the budget holds at that estimate, 2..count */

    int focus;
    int dir;

    int reserve_frames; /* cache_set_reserve(); image sequences only */
    int reserve;        /* frames behind the playhead kept ahead of the far
                           frames ahead; from the groups for video */

    int n_loading;
    int n_failed;
    int n_discarded;
    int n_aborted;
    int n_delivered;

    int stop;

    RpThread *threads;
    Worker   *workers;
    int       n_workers; /* workers allocated, each with a reader */
    int       n_threads; /* of those, threads actually started */

    void (*wakeup)(void *);
    void  *wakeup_ud;

    char last_error[256];
    int  error_count;
};

/* ---- distance ------------------------------------------------------------
 * Two measures with one job each.
 *
 * prio_of() orders loading: 0 is the frame under the playhead, 1 the next one
 * in the direction of play, and so on around the loop. The loaders fill in
 * that order, so the resident region runs ahead of the playhead and no effort
 * goes to frames behind it.
 *
 * score_of() decides what to give up. Higher is less valuable. A frame counts
 * as ahead if it is within the budget's reach in the direction of play, that
 * is, if the loaders could fill that far; it scores its distance ahead, so
 * the nearest is kept longest. Every other frame is behind, scoring above all
 * of those, the farthest behind highest, so the frames just shown are the
 * last to be recycled: a reversal, or a scrub back over them, finds them still
 * in RAM while the loaders turn around. (The original rule scored every frame
 * by its distance ahead round the loop, which made the frame just shown the
 * first to go.)
 *
 * Splitting at the budget's reach rather than at half the loop matters when
 * the budget holds more than half the sequence: the region must still run
 * the whole budget ahead of the playhead, and going round the loop the far
 * end is genuinely within reach.
 *
 * The reserve carves one band out of that. With `reserve` frames protected
 * behind the playhead and `window = cap - reserve` ahead of it, the order
 * from most to least valuable is: ahead within the window, then the reserve
 * (nearest first), then ahead beyond the window but within the budget, then
 * everything else behind, farthest first. The loaders fill forwards only to
 * the window, never evict the reserve to go further, and a reversal finds the
 * reserve still there. With reserve 0 the scores are the plain ones. */
static inline int prio_of(const Cache *c, int f)
{
    return rp_wrap((f - c->focus) * c->dir, c->count);
}

static inline int score_of(const Cache *c, int f)
{
    int cap    = c->cap_frames;
    int window = RP_MAX(2, cap - c->reserve);
    int res    = cap - window;
    int ahead  = prio_of(c, f);
    if (ahead < window) return ahead;
    int behind = c->count - ahead;
    if (behind <= res) return window + behind - 1;
    if (ahead < cap) return res + ahead;
    return cap + behind;
}

/* The reserve for the current focus and direction. For video: the frames of
 * the current group already shown plus the whole group before it (the last
 * group before the first, since playback loops); the mirror image playing
 * backwards. For images: the fixed count. Never more than half the budget,
 * so the direction of play always has the other half. */
static void update_reserve_locked(Cache *c)
{
    const Sequence *seq = c->seq;
    int n = c->count;
    int r;
    if (seq->group_start) {
        int f = c->focus;
        if (c->dir >= 0) {
            int gs   = sequence_group_start(seq, f);
            int prev = gs > 0 ? gs - sequence_group_start(seq, gs - 1)
                              : n - sequence_group_start(seq, n - 1);
            r = (f - gs) + prev;
        } else {
            int ge   = sequence_group_end(seq, f);
            int next = ge < n ? sequence_group_end(seq, ge) - ge
                              : sequence_group_end(seq, 0);
            r = (ge - 1 - f) + next;
        }
    } else {
        r = c->reserve_frames;
    }
    c->reserve = RP_CLAMP(r, 0, c->cap_frames / 2);
}

/* Keeps cap_frames in step with the budget and the frame size estimate. At
 * least 2, so the frame after the one on show always counts as ahead. */
static void update_cap_locked(Cache *c)
{
    size_t est = c->est_bytes ? c->est_bytes : 1;
    size_t cap = c->bytes_limit / est;
    if (cap > (size_t)c->count) cap = (size_t)c->count;
    c->cap_frames = cap < 2 ? 2 : (int)cap;
    update_reserve_locked(c);
}

/* How far ahead of the playhead the loaders look: the budget's reach plus
 * enough slack to keep every thread busy, never past the sequence. cap_frames
 * is at most count, so the sum cannot overflow. */
static int reach_locked(const Cache *c)
{
    return RP_MIN(c->cap_frames + c->n_threads + 4, c->count);
}

/* A decode in progress is stale when its frame is at least the loaders' reach
 * from the playhead in both directions. Measured both ways rather than in the
 * direction of play, so that reversing over the frames just ahead lets them
 * finish and land, where they are admitted anyway; only a genuine jump
 * abandons work. */
static int stale_locked(const Cache *c, int f)
{
    int ahead = prio_of(c, f);
    return RP_MIN(ahead, c->count - ahead) >= reach_locked(c);
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

/* The resident frame least worth keeping: the farthest behind the playhead if
 * any is behind, else the farthest ahead. The frame being displayed is never a
 * candidate. */
static int find_victim_locked(const Cache *c, int *out_score)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < c->ready_count; i++) {
        int f = c->ready_list[i];
        if (f == c->focus) continue;
        int s = score_of(c, f);
        if (s > best_score) {
            best_score = s;
            best = f;
        }
    }
    *out_score = best_score;
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

/* How many resident frames are worth less than a candidate with this score,
 * so could be given up for it. The frame being displayed never counts. */
static int count_worse_locked(const Cache *c, int score)
{
    int worse = 0;
    for (int i = 0; i < c->ready_count; i++) {
        int f = c->ready_list[i];
        if (f != c->focus && score_of(c, f) > score) worse++;
    }
    return worse;
}

/* ---- scheduling --------------------------------------------------------- */

/* Picks the most valuable frame that is worth fetching right now, or -1 if
 * there is nothing useful to do. Candidates are visited in priority order, so
 * the moment one fails the admission test every later one would too.
 *
 * A frame is admitted only if there will be somewhere to put it: free room,
 * or resident frames worth less than it, beyond what the loads already in
 * flight will take when they land. Without that count, one freed slot would
 * start a load on every thread and all but one would be decoded for nothing. */
static int pick_locked(Cache *c)
{
    int n = c->count;
    if (n <= 0) return -1;

    /* Look no further ahead than the budget could hold, plus enough slack to
     * keep every thread busy. The division stays in size_t and is clamped
     * before narrowing, so a very large budget cannot overflow the counter and
     * leave the scan doing nothing. */
    size_t est = c->est_bytes ? c->est_bytes : 1;
    int max_scan = reach_locked(c);

    size_t free_bytes = c->bytes_limit > c->bytes_used ? c->bytes_limit - c->bytes_used : 0;
    size_t free_slots = free_bytes / est;
    size_t in_flight  = (size_t)c->n_loading;

    for (int k = 0; k < max_scan; k++) {
        int f = rp_wrap(c->focus + c->dir * k, n);
        if (atomic_load_explicit(&c->entries[f].state, memory_order_relaxed) != CACHE_EMPTY)
            continue;

        if (c->ready_count == 0 && c->n_loading == 0) return f; /* one frame always fits */
        if (free_slots > in_flight) return f;

        size_t worse = (size_t)count_worse_locked(c, score_of(c, f));
        return (free_slots + worse > in_flight) ? f : -1;
    }
    return -1;
}

static void insert_locked(Cache *c, int f, Image *im)
{
    int s = score_of(c, f);

    while (c->bytes_used + im->bytes > c->bytes_limit) {
        int vscore;
        int v = find_victim_locked(c, &vscore);
        if (v < 0 || vscore <= s) break;
        evict_locked(c, v);
    }

    if (c->bytes_used + im->bytes > c->bytes_limit && c->ready_count > 0) {
        /* Everything resident is nearer the playhead than this frame, so the
         * budget is better spent where it already is. Leaving the entry EMPTY
         * is safe: pick_locked() applies the same test and will not
         * immediately ask for it again. */
        image_unref(im);
        atomic_store_explicit(&c->entries[f].state, CACHE_EMPTY, memory_order_relaxed);
        c->n_discarded++;
        return;
    }

    c->entries[f].img = im;
    c->bytes_used += im->bytes;
    ready_add(c, f);
    atomic_store_explicit(&c->entries[f].state, CACHE_READY, memory_order_release);

    if (im->bytes > c->est_bytes) {
        c->est_bytes = im->bytes;
        update_cap_locked(c);
    }
}

/* ---- loader threads ----------------------------------------------------- */

static void record_failure_locked(Cache *c, int f, const char *err)
{
    atomic_store_explicit(&c->entries[f].state, CACHE_FAILED, memory_order_relaxed);
    c->n_failed++;
    if (c->error_count++ == 0)
        snprintf(c->last_error, sizeof c->last_error, "%s", err ? err : "unknown error");
}

/* Gives back the part of a worker's claim from `from` on: frames it marked
 * LOADING and never delivered. `to_state` is EMPTY when they may be asked
 * for again, FAILED when the reader broke and retrying would only repeat it. */
static void release_claim_locked(Cache *c, Worker *w, int from, int to_state, const char *err)
{
    for (int k = RP_MAX(from, w->claim_from); k < w->claim_to; k++) {
        if (c->claim[k] != w->index) continue;
        c->claim[k] = -1;
        c->n_loading--;
        w->claimed--;
        if (to_state == CACHE_FAILED) record_failure_locked(c, k, err);
        else atomic_store_explicit(&c->entries[k].state, CACHE_EMPTY, memory_order_relaxed);
    }
}

/* Would frame `k` of this worker's claim be kept if it landed now? Yes if it
 * is within the kept set (ahead within the window, or in the reserve), or if
 * there is room for it beyond what the other workers' claims will take. The
 * worker's own later claims do not count against it: they land after `k`,
 * and are released if they turn out not to be wanted. Mirrors the admission
 * test in pick_locked(), so a claim stops where a pick would have. */
static int wanted_locked(const Cache *c, const Worker *w, int k)
{
    if (score_of(c, k) < c->cap_frames) return 1;
    size_t est = c->est_bytes ? c->est_bytes : 1;
    size_t free_bytes = c->bytes_limit > c->bytes_used ? c->bytes_limit - c->bytes_used : 0;
    size_t free_slots = free_bytes / est;
    size_t others = (size_t)(c->n_loading - w->claimed);
    return free_slots > others;
}

/* Is any frame of the claim after `after` still worth decoding? */
static int claim_wanted_locked(const Cache *c, const Worker *w, int after)
{
    for (int k = RP_MAX(after + 1, w->claim_from); k < w->claim_to; k++)
        if (c->claim[k] == w->index && wanted_locked(c, w, k)) return 1;
    return 0;
}

/* The reader asks whether to convert a frame of the range: only the ones this
 * worker claimed. Runs on the loader thread. */
static int frame_claimed(void *ud, int f)
{
    Worker *w = ud;
    Cache  *c = w->cache;
    rp_mutex_lock(&c->mu);
    int mine = c->claim[f] == w->index;
    rp_mutex_unlock(&c->mu);
    return mine;
}

/* The reader hands over one frame of the range. Runs on the loader thread. */
static int deliver_frame(void *ud, int f, Image *im, const char *err)
{
    Worker *w = ud;
    Cache  *c = w->cache;

    rp_mutex_lock(&c->mu);
    if (im) c->n_delivered++;
    if (c->claim[f] == w->index) {
        c->claim[f] = -1;
        c->n_loading--;
        w->claimed--;
        if (im) {
            /* A frame that finished despite a soft cancel is admitted like
             * any other; insert_locked() drops it if nothing resident is
             * worth less. */
            insert_locked(c, f, im);
        } else {
            record_failure_locked(c, f, err);
        }
    } else {
        /* Decoded on the way to a frame that was wanted, but this one was
         * not asked for: already resident, or never claimed. */
        image_unref(im);
    }
    w->frame = f + 1;

    int go_on = !c->stop && claim_wanted_locked(c, w, f);
    if (!go_on) release_claim_locked(c, w, f + 1, CACHE_EMPTY, NULL);

    void (*wakeup)(void *) = c->wakeup;
    void *wakeup_ud = c->wakeup_ud;
    rp_cond_broadcast(&c->cv);
    rp_mutex_unlock(&c->mu);

    /* Outside the lock: it pushes an event into the windowing layer, which
     * takes locks of its own. */
    if (wakeup) wakeup(wakeup_ud);
    return go_on;
}

/* Where a claim for the pick `f` starts: the earliest empty frame before it
 * in its group, so a backwards pick takes its group's earlier frames in the
 * same claim. */
static int claim_from_locked(const Cache *c, int f)
{
    int gs = sequence_group_start(c->seq, f);
    int from = f;
    while (from > gs &&
           atomic_load_explicit(&c->entries[from - 1].state, memory_order_relaxed) == CACHE_EMPTY)
        from--;
    return from;
}

/* How many pictures this reader decodes before it can deliver `from`: none
 * when it sits exactly there, the frames in between when it is earlier in
 * the same group, and the whole run from the keyframe plus a seek when it
 * would have to seek, which costs more than any position in the group. */
static int position_cost(const Reader *r, const Sequence *seq, int from)
{
    int gs  = sequence_group_start(seq, from);
    int pos = reader_position(r);
    if (pos >= 0 && pos <= from && gs <= pos) return from - pos;
    return from - gs + 1;
}

/* Should this worker leave the pick to another? Yes when an idle worker's
 * reader would reach the range more cheaply than this one's, so that worker
 * decodes on instead of this one seeking to the keyframe, or decoding
 * through frames it already delivered, to get there. The idle worker was
 * woken by the same broadcast and takes the pick next. An idle worker's
 * reader is not in use, so reading its position from here is safe. */
static int leave_to_cheaper_locked(const Cache *c, const Worker *w, int from)
{
    int mine = position_cost(w->reader, c->seq, from);
    if (mine == 0) return 0;
    for (int i = 0; i < c->n_threads; i++) {
        const Worker *o = &c->workers[i];
        if (o != w && o->frame < 0 && position_cost(o->reader, c->seq, from) < mine) return 1;
    }
    return 0;
}

static void worker_main(void *arg)
{
    Worker *w = arg;
    Cache  *c = w->cache;
    char err[256];

    for (;;) {
        rp_mutex_lock(&c->mu);
        int f = -1, from = -1;
        for (;;) {
            if (c->stop) break;
            f = pick_locked(c);
            if (f >= 0) {
                from = claim_from_locked(c, f);
                if (!leave_to_cheaper_locked(c, w, from)) break;
            }
            /* Going idle means nothing near the playhead is wanted, so the
             * exemption below has done its job; a later decode may be
             * abandoned again. */
            w->just_aborted = 0;
            rp_cond_wait(&c->cv, &c->mu);
        }
        if (c->stop) {
            rp_mutex_unlock(&c->mu);
            break;
        }

        /* Claim the pick's group, from `from` to the end of the group.
         * Frames already resident, loading or failed are left alone; the
         * reader decodes past them without converting them. */
        int ge = sequence_group_end(c->seq, f);
        for (int k = from; k < ge; k++) {
            if (atomic_load_explicit(&c->entries[k].state, memory_order_relaxed) != CACHE_EMPTY) continue;
            atomic_store_explicit(&c->entries[k].state, CACHE_LOADING, memory_order_relaxed);
            c->claim[k] = w->index;
            c->n_loading++;
            w->claimed++;
        }
        w->claim_from = from;
        w->claim_to   = ge;
        w->frame      = from;
        atomic_store_explicit(&w->abort, 0, memory_order_relaxed);
        rp_mutex_unlock(&c->mu);

        int ok = reader_load_range(w->reader, from, ge, deliver_frame, frame_claimed, w,
                                   &w->abort, err, sizeof err);

        rp_mutex_lock(&c->mu);
        if (ok) {
            /* The range ended, or the callback stopped it; either way what
             * is left of the claim was not wanted. */
            release_claim_locked(c, w, w->claim_from, CACHE_EMPTY, NULL);
            w->just_aborted = 0;
        } else if (c->stop) {
            /* Cancelled on the way out; not a real failure. */
            release_claim_locked(c, w, w->claim_from, CACHE_EMPTY, NULL);
        } else if (strcmp(err, READER_ERR_CANCELLED) == 0) {
            /* The playhead left this group behind and the decoder stopped;
             * the loop picks a frame that matters now. Judged by what the
             * decoder reports, not by the flag: a real failure after a
             * cancel it chose to ignore is still a failure. */
            release_claim_locked(c, w, w->claim_from, CACHE_EMPTY, NULL);
            c->n_aborted++;
            w->just_aborted = 1;
        } else {
            /* The reader itself broke on this group: a seek or a read
             * failed. Asking again would only repeat it. */
            release_claim_locked(c, w, w->claim_from, CACHE_FAILED, err);
            w->just_aborted = 0;
        }
        w->frame = -1;
        void (*wakeup)(void *) = c->wakeup;
        void *wakeup_ud = c->wakeup_ud;
        rp_cond_broadcast(&c->cv);
        rp_mutex_unlock(&c->mu);

        if (wakeup) wakeup(wakeup_ud);
    }
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
    update_cap_locked(c);

    c->entries    = rp_xcalloc((size_t)c->count, sizeof *c->entries);
    c->claim      = rp_xmalloc((size_t)c->count * sizeof *c->claim);
    c->ready_list = rp_xmalloc((size_t)c->count * sizeof *c->ready_list);
    c->ready_pos  = rp_xmalloc((size_t)c->count * sizeof *c->ready_pos);
    for (int i = 0; i < c->count; i++) {
        atomic_init(&c->entries[i].state, CACHE_EMPTY);
        c->ready_pos[i] = -1;
        c->claim[i] = -1;
    }

    rp_mutex_init(&c->mu);
    rp_cond_init(&c->cv);

    c->threads   = rp_xcalloc((size_t)n_workers, sizeof *c->threads);
    c->workers   = rp_xcalloc((size_t)n_workers, sizeof *c->workers);
    c->n_workers = n_workers;
    for (int i = 0; i < n_workers; i++) {
        c->workers[i].cache = c;
        c->workers[i].index = i;
        c->workers[i].frame = -1;
        atomic_init(&c->workers[i].abort, 0);
    }

    /* Each worker gets its own reader, opened here so a sequence that cannot
     * be decoded at all is reported now rather than by silent idle threads. */
    for (int i = 0; i < n_workers; i++) {
        char err[256];
        c->workers[i].reader = reader_open(seq, lut, err, sizeof err);
        if (!c->workers[i].reader) {
            rp_log("cannot open '%s': %s", seq->display, err);
            c->stop = 1;
            c->n_threads = 0;
            cache_destroy(c);
            return NULL;
        }
    }

    /* Spawn with the lock held. Workers take it as the first thing they do, so
     * this publishes the finished cache to them and lets the final thread
     * count be written before anybody can read it. */
    rp_mutex_lock(&c->mu);
    int started = 0;
    for (int i = 0; i < n_workers; i++) {
        if (!rp_thread_create(&c->threads[i], worker_main, &c->workers[i])) break;
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

    rp_mutex_lock(&c->mu);
    c->stop = 1;
    /* Under the lock, so a worker cannot start a new decode (and reset its
     * flag) between the flag being raised and it seeing stop. */
    for (int i = 0; i < c->n_threads; i++)
        atomic_store_explicit(&c->workers[i].abort, READER_CANCEL_HARD, memory_order_relaxed);
    rp_cond_broadcast(&c->cv);
    rp_mutex_unlock(&c->mu);

    for (int i = 0; i < c->n_threads; i++) rp_thread_join(&c->threads[i]);

    for (int i = 0; i < c->count; i++) image_unref(c->entries[i].img);
    for (int i = 0; i < c->n_workers; i++) reader_close(c->workers[i].reader);

    rp_cond_destroy(&c->cv);
    rp_mutex_destroy(&c->mu);
    free(c->threads);
    free(c->workers);
    free(c->entries);
    free(c->claim);
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
    if (changed) {
        update_reserve_locked(c);
        /* Decodes the playhead has left out of reach are abandoned so the
         * thread can take a frame near the new position: a soft cancel, so
         * one past half way finishes instead, and never two in a row on one
         * thread. n_loading stays as it is: the thread is busy until it
         * returns. */
        for (int i = 0; i < c->n_threads; i++) {
            Worker *w = &c->workers[i];
            if (w->frame >= 0 && !w->just_aborted && stale_locked(c, w->frame))
                atomic_store_explicit(&w->abort, READER_CANCEL_SOFT, memory_order_relaxed);
        }
        rp_cond_broadcast(&c->cv);
    }
    rp_mutex_unlock(&c->mu);
}

void cache_set_reserve(Cache *c, int frames)
{
    if (!c) return;
    rp_mutex_lock(&c->mu);
    c->reserve_frames = frames > 0 ? frames : 0;
    update_reserve_locked(c);
    rp_cond_broadcast(&c->cv);
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
    out->discarded   = c->n_discarded;
    out->aborted     = c->n_aborted;
    out->delivered   = c->n_delivered;
    for (int i = 0; i < c->n_workers; i++) {
        int d = 0;
        if (reader_video_stats(c->workers[i].reader, NULL, &d)) out->decoded += d;
    }
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
