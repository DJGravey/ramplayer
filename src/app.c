#include "app.h"

#include <string.h>

#include "util.h"

void app_init(App *a, const Sequence *seq, Cache *cache, double fps)
{
    memset(a, 0, sizeof *a);
    a->seq         = seq;
    a->cache       = cache;
    a->fps         = (fps > 0.0) ? fps : 30.0;
    a->current     = 0;
    a->play_dir    = 0;
    a->last_dir    = 1;
    a->fit         = 1;
    a->filter      = DRAW_BILINEAR;
    a->shown_frame = -1;
    a->hover_button = -1;
    a->press_button = -1;
    a->need_redraw = 1;
    a->src_w = seq->width;
    a->src_h = seq->height;

    cache_set_focus(a->cache, a->current, a->last_dir);
}

void app_shutdown(App *a)
{
    image_unref(a->shown);
    a->shown = NULL;
}

/* Moves the playhead without touching playback state. */
static void goto_frame(App *a, int frame)
{
    frame = RP_CLAMP(frame, 0, a->seq->count - 1);
    if (frame != a->current) {
        a->current = frame;
        a->need_redraw = 1;
    }
    cache_set_focus(a->cache, a->current, a->last_dir);
}

void app_set_frame(App *a, int frame)
{
    goto_frame(a, frame);
}

void app_pause(App *a)
{
    if (a->play_dir != 0) {
        a->play_dir = 0;
        a->stalled = 0;
        a->need_redraw = 1;
        cache_set_focus(a->cache, a->current, a->last_dir);
    }
}

void app_step(App *a, int delta)
{
    app_pause(a);
    if (delta != 0) a->last_dir = (delta > 0) ? 1 : -1;
    goto_frame(a, rp_wrap(a->current + delta, a->seq->count));
}

void app_toggle_play(App *a, int dir)
{
    if (dir == 0) dir = 1;

    if (a->play_dir == dir) {
        app_pause(a);
        return;
    }
    a->play_dir = dir;
    a->last_dir = dir;
    a->stalled  = 0;
    a->next_due = rp_now() + 1.0 / a->fps;
    a->need_redraw = 1;
    cache_set_focus(a->cache, a->current, a->last_dir);
}

double app_tick(App *a)
{
    if (a->play_dir == 0) {
        a->stalled = 0;
        return 1.0;
    }

    double now = rp_now();
    double period = 1.0 / a->fps;
    if (now < a->next_due) return a->next_due - now;

    int next = rp_wrap(a->current + a->play_dir, a->seq->count);
    int state = cache_frame_state(a->cache, next);

    if (state != CACHE_READY && state != CACHE_FAILED) {
        /* Not in RAM yet. Hold both the playhead and the clock: resuming from
         * here plays at true speed rather than sprinting through the frames
         * that were missed while waiting. */
        if (!a->stalled) {
            a->stalled = 1;
            a->need_redraw = 1;
        }
        a->next_due = now;
        return 0.008;
    }

    if (a->stalled) {
        a->stalled = 0;
        a->need_redraw = 1;
    }
    goto_frame(a, next);

    a->next_due += period;
    /* If we have fallen far behind (the window was hidden, say) resync instead
     * of running a burst of catch-up frames. */
    if (a->next_due < now) a->next_due = now + period;
    return RP_MAX(0.0, a->next_due - rp_now());
}

void app_update_shown(App *a)
{
    if (a->shown && a->shown_frame == a->current) return;

    Image *im = cache_acquire(a->cache, a->current);
    if (!im) return; /* keep the previous frame on screen while this one loads */

    image_unref(a->shown);
    a->shown = im;
    a->shown_frame = a->current;
    a->need_redraw = 1;

    /* Tracked per frame rather than once, so a sequence whose frames differ
     * in size still reports what is actually on screen. */
    a->src_w = im->width;
    a->src_h = im->height;
}

/* ---- input -------------------------------------------------------------- */

static void activate_button(App *a, int id)
{
    switch (id) {
    case BTN_PREV:      app_step(a, -1);       break;
    case BTN_NEXT:      app_step(a, +1);       break;
    case BTN_PLAY_BACK: app_toggle_play(a, -1); break;
    case BTN_PLAY_FWD:  app_toggle_play(a, +1); break;
    default: break;
    }
}

void app_mouse_down(App *a, int x, int y, int button)
{
    if (button != 1) return;

    int b = ui_button_at(&a->layout, x, y);
    if (b >= 0) {
        a->press_button = b;
        a->need_redraw = 1;
        return;
    }

    if (rect_contains(a->layout.timeline, x, y)) {
        /* Pressing the timeline pauses and jumps straight to the frame under
         * the cursor; the drag that may follow keeps updating it. */
        app_pause(a);
        a->scrubbing = 1;
        goto_frame(a, ui_frame_at_x(&a->layout, x, a->seq->count));
        a->need_redraw = 1;
    }
}

void app_mouse_move(App *a, int x, int y, int left_held)
{
    int hb = ui_button_at(&a->layout, x, y);
    if (hb != a->hover_button) {
        a->hover_button = hb;
        a->need_redraw = 1;
    }

    if (!a->scrubbing) return;
    if (!left_held) {
        a->scrubbing = 0;
        return;
    }
    /* x is clamped inside ui_frame_at_x, so dragging past either end of the
     * timeline pins to the first or last frame. */
    goto_frame(a, ui_frame_at_x(&a->layout, x, a->seq->count));
}

void app_mouse_up(App *a, int x, int y, int button)
{
    if (button != 1) return;

    if (a->press_button >= 0) {
        if (ui_button_at(&a->layout, x, y) == a->press_button)
            activate_button(a, a->press_button);
        a->press_button = -1;
        a->need_redraw = 1;
    }
    a->scrubbing = 0;
}

void app_key(App *a, AppKey key)
{
    switch (key) {
    case KEY_QUIT:      a->quit = 1; break;
    case KEY_SPACE:
        if (a->play_dir != 0) app_pause(a);
        else                  app_toggle_play(a, 1);
        break;
    case KEY_PLAY_BACK: app_toggle_play(a, -1); break;
    case KEY_LEFT:      app_step(a, -1);  break;
    case KEY_RIGHT:     app_step(a, +1);  break;
    case KEY_PAGE_BACK: app_step(a, -10); break;
    case KEY_PAGE_FWD:  app_step(a, +10); break;
    case KEY_HOME:      app_pause(a); goto_frame(a, 0); break;
    case KEY_END:       app_pause(a); goto_frame(a, a->seq->count - 1); break;
    case KEY_FIT:       a->fit = 1; a->need_redraw = 1; break;
    case KEY_ONE_TO_ONE:a->fit = 0; a->need_redraw = 1; break;
    case KEY_FILTER:
        a->filter = (a->filter == DRAW_BILINEAR) ? DRAW_NEAREST : DRAW_BILINEAR;
        a->need_redraw = 1;
        break;
    case KEY_HELP:      a->show_help = !a->show_help; a->need_redraw = 1; break;
    default: break;
    }
}

void app_resize(App *a, int w, int h)
{
    ui_layout(&a->layout, w, h);
    a->need_redraw = 1;
}
