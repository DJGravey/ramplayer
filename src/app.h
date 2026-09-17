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
#include "view.h"

typedef enum {
    KEY_NONE = 0,
    KEY_QUIT,
    KEY_SPACE,      /* pause, or play in the direction that last played */
    KEY_PLAY_FWD,   /* play forwards; not a toggle */
    KEY_PLAY_REV,   /* play backwards; not a toggle */
    KEY_PAUSE,      /* pause only; never resumes */
    KEY_LEFT,       /* pause and step back one frame */
    KEY_RIGHT,      /* pause and step forwards one frame */
    KEY_STEP5_BACK, /* the same, five frames */
    KEY_STEP5_FWD,
    KEY_PAGE_BACK,
    KEY_PAGE_FWD,
    KEY_HOME,
    KEY_END,
    KEY_FIT,        /* fit the frame to the window */
    KEY_ONE_TO_ONE, /* 100%, centred */
    KEY_GUIDE,      /* toggle the 1920x1080 frame guide */
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
    int    resume_dir; /* direction of the last play command; what space
                          resumes. Drags and steps do not change it. */
    double fps;
    double next_due;  /* when the next frame should be shown */
    int    stalled;   /* holding because the next frame is not resident */

    View       view;  /* fitted, or zoomed and panned */
    DrawFilter filter;
    int        show_guide; /* the 1920x1080 frame guide */

    Image *shown;       /* image currently on screen; holds a reference */
    int    shown_frame; /* the frame `shown` is; the nearest resident one when
                           `current` itself is not in RAM yet */

    int scrubbing;
    int panning;      /* a drag button held in the viewport */
    int pan_button;   /* which one */
    int pan_x, pan_y; /* where the pointer was at the last pan step */
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
/* Plays in `dir`, or pauses if already playing that way (the buttons). */
void app_toggle_play(App *a, int dir);
/* Plays in `dir`; a no-op if already playing that way (J and L). */
void app_play(App *a, int dir);
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

/* Mouse buttons are numbered as SDL numbers them: 1 left, 2 middle, 3 right.
 * `held` on a move is a mask of the buttons down, bit 1 left, bit 2 middle. */
#define APP_MOUSE_LEFT   1
#define APP_MOUSE_MIDDLE 2
#define APP_HELD_LEFT    1
#define APP_HELD_MIDDLE  2

void app_mouse_down(App *a, int x, int y, int button);
void app_mouse_up(App *a, int x, int y, int button);
void app_mouse_move(App *a, int x, int y, int held);
/* Wheel notches: positive zooms in about (x, y). Ignored outside the viewport. */
void app_wheel(App *a, int x, int y, int notches);
void app_key(App *a, AppKey key);
void app_resize(App *a, int w, int h);

#endif /* RP_APP_H */
