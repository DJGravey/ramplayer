/* app.h - player state, playback clock, and input handling.
 *
 * Deliberately free of any windowing library: main.c translates SDL events
 * into the calls below, so the player logic can be exercised without a window.
 */
#ifndef RP_APP_H
#define RP_APP_H

#include "cache.h"
#include "color.h"
#include "draw.h"
#include "sequence.h"
#include "ui.h"

typedef enum {
    KEY_NONE = 0,
    KEY_QUIT,
    KEY_SPACE,
    KEY_PLAY_BACK,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGE_BACK,
    KEY_PAGE_FWD,
    KEY_HOME,
    KEY_END,
    KEY_FIT,
    KEY_ONE_TO_ONE,
    KEY_FILTER,
    KEY_HELP
} AppKey;

struct App {
    /* Immutable once the player is running; the loader threads read frame
     * paths out of it without holding a lock. */
    const Sequence *seq;
    Cache          *cache;

    int    current;   /* index into seq->frames */
    int    play_dir;  /* 0 paused, +1 forwards, -1 backwards */
    int    last_dir;  /* most recent direction, drives cache priority */
    double fps;
    double next_due;  /* when the next frame should be shown */
    int    stalled;   /* holding because the next frame is not resident */

    int        fit;   /* 1 = fit to window, 0 = actual size */
    DrawFilter filter;

    Image *shown;       /* image currently on screen; holds a reference */
    int    shown_frame; /* the frame `shown` is; the nearest resident one when
                           `current` itself is not in RAM yet */

    int scrubbing;
    int hover_button;
    int press_button;
    int show_help;

    Layout layout;
    int    need_redraw;
    int    quit;

    int src_w, src_h; /* source resolution, once known */
};

void app_init(App *a, const Sequence *seq, Cache *cache, double fps);
void app_shutdown(App *a);

void app_set_frame(App *a, int frame);
void app_step(App *a, int delta);
void app_toggle_play(App *a, int dir);
void app_pause(App *a);

/* Advances the playhead if the next frame is due and resident. Returns how
 * long the caller may sleep before calling again, in seconds. */
double app_tick(App *a);

/* Takes a reference on the image for the current frame, if it is resident,
 * else on the resident frame nearest to it along the timeline, so a scrub
 * into frames still loading shows the closest thing there is rather than the
 * last frame that happened to be on screen. Only if nothing at all is
 * resident does the previous image stay. */
void app_update_shown(App *a);

void app_mouse_down(App *a, int x, int y, int button);
void app_mouse_up(App *a, int x, int y, int button);
void app_mouse_move(App *a, int x, int y, int left_held);
void app_key(App *a, AppKey key);
void app_resize(App *a, int w, int h);

#endif /* RP_APP_H */
