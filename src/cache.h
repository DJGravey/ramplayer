/* cache.h - the RAM frame store.
 *
 * A pool of loader threads keeps the frames nearest the playhead resident in
 * memory, up to a fixed byte budget. Rather than draining a queue that goes
 * stale the moment the user scrubs somewhere else, each worker asks the cache
 * which frame is currently the most valuable one to fetch, so a jump across the
 * timeline redirects every thread immediately.
 *
 * Value is simply the distance from the playhead measured in the direction of
 * play, wrapping at the ends because playback loops. A frame is only fetched
 * if there is room for it, or if something resident is further from the
 * playhead than it is; that admission rule is what stops the threads from
 * thrashing a full cache.
 *
 * The Sequence passed to cache_create() must outlive the cache: loader threads
 * read paths out of it without holding the lock.
 */
#ifndef RP_CACHE_H
#define RP_CACHE_H

#include <stddef.h>

#include "color.h"
#include "image.h"
#include "sequence.h"

typedef enum {
    CACHE_EMPTY   = 0,
    CACHE_LOADING = 1,
    CACHE_READY   = 2,
    CACHE_FAILED  = 3
} CacheFrameState;

typedef struct {
    int    ready;
    int    loading;
    int    failed;
    size_t bytes_used;
    size_t bytes_limit;
    int    capacity_frames; /* how many frames the budget is expected to hold */
    int    discarded;       /* decoded frames dropped on arrival because nothing
                               resident was worth less by then; a count that
                               grows during ordinary playback means loads are
                               being admitted that cannot land */
    int    aborted;         /* decodes abandoned part way because the playhead
                               jumped out of reach of the frame; the thread
                               went to a frame that mattered instead */
} CacheStats;

typedef struct Cache Cache;

Cache *cache_create(const Sequence *seq, const ColorLUT *lut,
                    size_t byte_limit, int n_workers, size_t est_frame_bytes);
void   cache_destroy(Cache *c);

/* Tells the loaders where the playhead is and which way it is moving. A decode
 * in progress whose frame is now out of the loaders' reach in both directions
 * is abandoned, so a jump across the timeline frees the threads for the frames
 * around the new position instead of waiting for the old ones to finish. */
void cache_set_focus(Cache *c, int frame, int direction);

/* Returns the frame with an extra reference, or NULL if it is not resident.
 * Never blocks on I/O. The caller must image_unref() the result; holding a
 * reference keeps the pixels alive even if the frame is evicted meanwhile. */
Image *cache_acquire(Cache *c, int frame);

/* Advisory, lock free; for drawing the cache strip on the timeline. */
int cache_frame_state(const Cache *c, int frame);

void cache_get_stats(Cache *c, CacheStats *out);

/* Called from loader threads whenever a frame lands, so the main loop can wake
 * up and redraw. Must be safe to call from any thread. */
void cache_set_wakeup(Cache *c, void (*fn)(void *), void *userdata);

/* First decode error seen, or NULL. Also reports how many frames failed. */
const char *cache_last_error(Cache *c);

#endif /* RP_CACHE_H */
