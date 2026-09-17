/* test_player.c - exercises the player's specified behaviour without a window.
 *
 * Everything here drives the same entry points main.c calls when SDL delivers
 * an event, so the timeline mapping, the transport buttons and the playback
 * loop are tested as the user drives them.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "cache.h"
#include "color.h"
#include "font.h"
#include "platform.h"
#include "reader.h"
#include "sequence.h"
#include "ui.h"
#include "util.h"

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
               printf(__VA_ARGS__); printf("\n"); }                           \
    } while (0)

#define WIN_W 1200
#define WIN_H 800

static const int MOUSE_LEFT = 1;

static void click(App *a, int x, int y)
{
    app_mouse_down(a, x, y, MOUSE_LEFT);
    app_mouse_up(a, x, y, MOUSE_LEFT);
}

static int btn_cx(const App *a, ButtonId b) { return a->layout.buttons[b].x + a->layout.buttons[b].w / 2; }
static int btn_cy(const App *a, ButtonId b) { return a->layout.buttons[b].y + a->layout.buttons[b].h / 2; }

/* ---- tests -------------------------------------------------------------- */

static void test_timeline_mapping(App *a)
{
    printf("timeline mapping\n");
    const Layout *L = &a->layout;
    int n = a->seq->count;
    Rect t = L->track;

    CHECK(ui_frame_at_x(L, t.x, n) == 0, "leftmost pixel must be the first frame");
    CHECK(ui_frame_at_x(L, t.x + t.w - 1, n) == n - 1, "rightmost pixel must be the last frame");

    /* Clicks outside the track clamp rather than running off the sequence. */
    CHECK(ui_frame_at_x(L, t.x - 500, n) == 0, "left of the track clamps to the first frame");
    CHECK(ui_frame_at_x(L, t.x + t.w + 500, n) == n - 1, "right of the track clamps to the last frame");

    /* Halfway along is the middle of the sequence. */
    int mid = ui_frame_at_x(L, t.x + (t.w - 1) / 2, n);
    CHECK(abs(mid - (n - 1) / 2) <= 1, "midpoint maps near the middle frame (got %d of %d)", mid, n);

    /* With more pixels than frames the mapping must round trip exactly. */
    int roundtrip_ok = 1;
    for (int f = 0; f < n; f++)
        if (ui_frame_at_x(L, ui_x_for_frame(L, f, n), n) != f) roundtrip_ok = 0;
    CHECK(roundtrip_ok, "frame -> x -> frame must round trip for every frame");

    /* And it must increase monotonically across the track. */
    int mono_ok = 1, prev = -1;
    for (int x = t.x; x < t.x + t.w; x++) {
        int f = ui_frame_at_x(L, x, n);
        if (f < prev) mono_ok = 0;
        prev = f;
    }
    CHECK(mono_ok, "frame under the cursor must never go backwards moving right");
}

static void test_timeline_click_and_drag(App *a)
{
    printf("timeline press, drag and release\n");
    const Layout *L = &a->layout;
    int n = a->seq->count;
    int ty = L->track.y + L->track.h / 2;

    /* Spec: pressing the left button on the timeline pauses playback. */
    app_toggle_play(a, +1);
    CHECK(a->play_dir == +1, "play forwards should start playback");
    app_mouse_down(a, ui_x_for_frame(L, 40, n), ty, MOUSE_LEFT);
    CHECK(a->play_dir == 0, "pressing the timeline must pause playback");
    CHECK(a->current == 40, "pressing the timeline must jump to that frame (got %d)", a->current);

    /* Spec: the frame keeps updating while the button stays down. */
    app_mouse_move(a, ui_x_for_frame(L, 70, n), ty, 1);
    CHECK(a->current == 70, "dragging must keep updating the frame (got %d)", a->current);
    app_mouse_move(a, ui_x_for_frame(L, 12, n), ty, 1);
    CHECK(a->current == 12, "dragging backwards must work too (got %d)", a->current);

    /* Dragging beyond the ends pins to the first and last frames. */
    app_mouse_move(a, L->track.x - 2000, ty, 1);
    CHECK(a->current == 0, "dragging off the left end pins to the first frame");
    app_mouse_move(a, L->track.x + L->track.w + 2000, ty, 1);
    CHECK(a->current == n - 1, "dragging off the right end pins to the last frame");

    /* After release, moving the mouse must no longer scrub. */
    app_mouse_up(a, ui_x_for_frame(L, n - 1, n), ty, MOUSE_LEFT);
    app_mouse_move(a, ui_x_for_frame(L, 5, n), ty, 0);
    CHECK(a->current == n - 1, "releasing must stop scrubbing (got %d)", a->current);

    /* A plain click still selects the clicked frame. */
    click(a, ui_x_for_frame(L, 33, n), ty);
    CHECK(a->current == 33, "clicking the timeline selects that frame (got %d)", a->current);
}

static void test_transport_buttons(App *a)
{
    printf("transport buttons\n");
    app_pause(a);
    app_set_frame(a, 20);

    click(a, btn_cx(a, BTN_PLAY_FWD), btn_cy(a, BTN_PLAY_FWD));
    CHECK(a->play_dir == +1, "play forwards button starts forward playback");

    /* Spec: clicking the same button again pauses. */
    click(a, btn_cx(a, BTN_PLAY_FWD), btn_cy(a, BTN_PLAY_FWD));
    CHECK(a->play_dir == 0, "play forwards button pauses when clicked again");

    click(a, btn_cx(a, BTN_PLAY_BACK), btn_cy(a, BTN_PLAY_BACK));
    CHECK(a->play_dir == -1, "play backwards button starts reverse playback");
    click(a, btn_cx(a, BTN_PLAY_BACK), btn_cy(a, BTN_PLAY_BACK));
    CHECK(a->play_dir == 0, "play backwards button pauses when clicked again");

    /* The opposite button changes direction rather than pausing. */
    click(a, btn_cx(a, BTN_PLAY_FWD), btn_cy(a, BTN_PLAY_FWD));
    click(a, btn_cx(a, BTN_PLAY_BACK), btn_cy(a, BTN_PLAY_BACK));
    CHECK(a->play_dir == -1, "the other play button switches direction");

    /* Spec: step buttons pause first, then move one frame. */
    int before = a->current;
    click(a, btn_cx(a, BTN_NEXT), btn_cy(a, BTN_NEXT));
    CHECK(a->play_dir == 0, "next frame button pauses playback");
    CHECK(a->current == before + 1, "next frame button advances one frame (%d -> %d)", before, a->current);

    click(a, btn_cx(a, BTN_PLAY_FWD), btn_cy(a, BTN_PLAY_FWD));
    before = a->current;
    click(a, btn_cx(a, BTN_PREV), btn_cy(a, BTN_PREV));
    CHECK(a->play_dir == 0, "previous frame button pauses playback");
    CHECK(a->current == before - 1, "previous frame button steps back one frame (%d -> %d)", before, a->current);

    /* A press that drifts off the button before release must not fire it. */
    app_set_frame(a, 20);
    app_mouse_down(a, btn_cx(a, BTN_NEXT), btn_cy(a, BTN_NEXT), MOUSE_LEFT);
    app_mouse_up(a, a->layout.viewport.x + 5, a->layout.viewport.y + 5, MOUSE_LEFT);
    CHECK(a->current == 20, "releasing away from a button must not activate it");
}

static void test_step_wrapping(App *a)
{
    printf("stepping past the ends\n");
    int n = a->seq->count;

    app_set_frame(a, n - 1);
    app_step(a, +1);
    CHECK(a->current == 0, "stepping past the last frame wraps to the first (got %d)", a->current);

    app_set_frame(a, 0);
    app_step(a, -1);
    CHECK(a->current == n - 1, "stepping before the first frame wraps to the last (got %d)", a->current);
}

/* Waits until the whole sequence is resident so playback cannot stall. */
static int wait_for_cache(Cache *c, int want, double timeout)
{
    double t0 = rp_now();
    CacheStats st;
    for (;;) {
        cache_get_stats(c, &st);
        if (st.ready >= want) return 1;
        if (rp_now() - t0 > timeout) {
            printf("  (cache filled %d/%d in %.1fs)\n", st.ready, want, timeout);
            return st.ready > want / 2;
        }
        rp_sleep_ms(20);
    }
}

/* Drives app_tick() until the playhead moves, with the clock forced due. */
static int advance_one(App *a)
{
    int start = a->current;
    for (int i = 0; i < 400; i++) {
        a->next_due = rp_now() - 1.0;
        app_tick(a);
        if (a->current != start) return 1;
        rp_sleep_ms(5);
    }
    return 0;
}

static void test_playback_loops(App *a)
{
    printf("playback advance and looping\n");
    int n = a->seq->count;

    app_pause(a);
    app_set_frame(a, 10);
    app_toggle_play(a, +1);
    CHECK(advance_one(a), "forward playback advances the frame");
    CHECK(a->current == 11, "forward playback moves to the next frame (got %d)", a->current);

    /* Spec: forward playback loops round to the first frame. */
    app_set_frame(a, n - 1);
    CHECK(advance_one(a), "forward playback advances at the last frame");
    CHECK(a->current == 0, "forward playback loops to the first frame (got %d)", a->current);

    app_pause(a);
    app_set_frame(a, 10);
    app_toggle_play(a, -1);
    CHECK(advance_one(a), "reverse playback advances the frame");
    CHECK(a->current == 9, "reverse playback moves to the previous frame (got %d)", a->current);

    /* Spec: backward playback loops round to the last frame. */
    app_set_frame(a, 0);
    CHECK(advance_one(a), "reverse playback advances at the first frame");
    CHECK(a->current == n - 1, "reverse playback loops to the last frame (got %d)", a->current);

    app_pause(a);
    int held = a->current;
    a->next_due = rp_now() - 1.0;
    app_tick(a);
    CHECK(a->current == held, "a paused player must not advance");
}

static void test_keyboard(App *a)
{
    printf("keyboard shortcuts\n");
    int n = a->seq->count;
    app_pause(a);
    app_set_frame(a, 50);

    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == +1, "space starts playback");
    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == 0, "space pauses again");

    app_key(a, KEY_RIGHT);
    CHECK(a->current == 51, "right steps forward (got %d)", a->current);
    app_key(a, KEY_LEFT);
    CHECK(a->current == 50, "left steps back (got %d)", a->current);

    app_key(a, KEY_HOME);
    CHECK(a->current == 0, "home goes to the first frame");
    app_key(a, KEY_END);
    CHECK(a->current == n - 1, "end goes to the last frame");

    /* J, K and L are commands, not toggles. */
    app_key(a, KEY_PLAY_FWD);
    CHECK(a->play_dir == +1, "L plays forwards");
    app_key(a, KEY_PLAY_FWD);
    CHECK(a->play_dir == +1, "L again keeps playing forwards, it is not a toggle");
    app_key(a, KEY_PLAY_REV);
    CHECK(a->play_dir == -1, "J plays backwards");
    app_key(a, KEY_PLAY_REV);
    CHECK(a->play_dir == -1, "J again keeps playing backwards");
    app_key(a, KEY_PAUSE);
    CHECK(a->play_dir == 0, "K pauses");
    app_key(a, KEY_PAUSE);
    CHECK(a->play_dir == 0, "K again stays paused, it never resumes");

    /* Space resumes the direction of the last play command, and a scrub in
     * between does not change that even though it turns the cache round. */
    app_key(a, KEY_PLAY_REV);
    app_key(a, KEY_PAUSE);
    {
        const Layout *L = &a->layout;
        int ty = L->track.y + L->track.h / 2;
        app_mouse_down(a, ui_x_for_frame(L, 10, n), ty, MOUSE_LEFT);
        app_mouse_move(a, ui_x_for_frame(L, 20, n), ty, 1);
        app_mouse_up(a, ui_x_for_frame(L, 20, n), ty, MOUSE_LEFT);
    }
    CHECK(a->last_dir == +1, "the rightward drag turned the cache forwards");
    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == -1, "space after J, K and a drag plays backwards (got %d)", a->play_dir);
    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == 0, "space pauses backwards playback");
    app_key(a, KEY_PLAY_FWD);
    app_key(a, KEY_PAUSE);
    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == +1, "space after L and K plays forwards (got %d)", a->play_dir);
    app_key(a, KEY_PAUSE);

    /* The play buttons count as play commands too. */
    click(a, btn_cx(a, BTN_PLAY_BACK), btn_cy(a, BTN_PLAY_BACK));
    click(a, btn_cx(a, BTN_PLAY_BACK), btn_cy(a, BTN_PLAY_BACK));
    app_key(a, KEY_SPACE);
    CHECK(a->play_dir == -1, "space after the reverse button plays backwards (got %d)", a->play_dir);

    /* I and O pause and step; with shift, five frames, wrapping like the rest. */
    app_set_frame(a, 50);
    app_key(a, KEY_PLAY_FWD);
    app_key(a, KEY_STEP5_FWD);
    CHECK(a->play_dir == 0, "shift+O pauses");
    CHECK(a->current == 55, "shift+O steps five forwards (got %d)", a->current);
    app_key(a, KEY_STEP5_BACK);
    CHECK(a->current == 50, "shift+I steps five back (got %d)", a->current);
    app_set_frame(a, 2);
    app_key(a, KEY_STEP5_BACK);
    CHECK(a->current == n - 3, "shift+I wraps at the start (got %d)", a->current);
    app_key(a, KEY_STEP5_FWD);
    CHECK(a->current == 2, "shift+O wraps at the end (got %d)", a->current);

    /* The view keys. */
    app_key(a, KEY_ONE_TO_ONE);
    CHECK(!a->view.fit, "0 leaves fit mode");
    {
        Rect vp = a->layout.viewport;
        Rect got = view_rect(&a->view, vp, a->src_w, a->src_h);
        Rect want = rect_center(vp, a->src_w, a->src_h);
        CHECK(got.x == want.x && got.y == want.y && got.w == want.w && got.h == want.h,
              "0 shows the frame at 100%%, centred (%d,%d %dx%d vs %d,%d %dx%d)",
              got.x, got.y, got.w, got.h, want.x, want.y, want.w, want.h);
    }
    app_key(a, KEY_FIT);
    CHECK(a->view.fit, "backspace fits the frame to the window again");

    CHECK(!a->show_guide, "the frame guide starts off");
    app_key(a, KEY_GUIDE);
    CHECK(a->show_guide, "F shows the frame guide");
    app_key(a, KEY_GUIDE);
    CHECK(!a->show_guide, "F again hides it");

    /* A fresh player resumes forwards. */
    App fresh;
    app_init(&fresh, a->seq, a->cache, 24.0);
    app_resize(&fresh, WIN_W, WIN_H);
    app_key(&fresh, KEY_SPACE);
    CHECK(fresh.play_dir == +1, "space on a fresh player plays forwards");
    app_shutdown(&fresh);
}

/* The source pixel under screen point (mx, my) for the given view. */
static double source_x_under(const View *v, Rect vp, int iw, int ih, int mx)
{
    Rect r = view_rect(v, vp, iw, ih);
    return (mx - r.x) / view_scale(v, vp, iw, ih);
}

static double source_y_under(const View *v, Rect vp, int iw, int ih, int my)
{
    Rect r = view_rect(v, vp, iw, ih);
    return (my - r.y) / view_scale(v, vp, iw, ih);
}

static int rect_eq(Rect a, Rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* The view maths on its own: no cache, no image, just rects. */
static void test_view(void)
{
    printf("view: fit, 1:1, zoom about a point, pan\n");
    const int iw = 1280, ih = 720;
    Rect vp = rect_make(0, 0, 1000, 600);
    View v;

    view_init(&v);
    CHECK(rect_eq(view_rect(&v, vp, iw, ih), rect_fit(vp, iw, ih)), "a fresh view is the fitted rect");
    CHECK(view_scale(&v, vp, iw, ih) < 1.0 && view_scale(&v, vp, iw, ih) > 0.7,
          "the fitted scale is the fit ratio (got %.3f)", view_scale(&v, vp, iw, ih));

    view_one_to_one(&v, iw, ih);
    CHECK(rect_eq(view_rect(&v, vp, iw, ih), rect_center(vp, iw, ih)), "1:1 is the centred rect");
    CHECK(view_scale(&v, vp, iw, ih) == 1.0, "1:1 has a scale of exactly 1");

    /* Zooming in about a point keeps the source pixel under it in place. */
    view_init(&v);
    const int mx = 700, my = 150;
    double sx0 = source_x_under(&v, vp, iw, ih, mx), sy0 = source_y_under(&v, vp, iw, ih, my);
    double z0 = view_scale(&v, vp, iw, ih);
    view_zoom_at(&v, vp, iw, ih, mx, my, VIEW_WHEEL_STEP);
    CHECK(!v.fit, "zooming leaves fit mode");
    CHECK(view_scale(&v, vp, iw, ih) > z0, "zooming in raises the scale (%.3f -> %.3f)", z0, v.zoom);
    double sx1 = source_x_under(&v, vp, iw, ih, mx), sy1 = source_y_under(&v, vp, iw, ih, my);
    CHECK(fabs(sx1 - sx0) <= 2.0 && fabs(sy1 - sy0) <= 2.0,
          "the source pixel under the cursor stays put (%.1f,%.1f -> %.1f,%.1f)", sx0, sy0, sx1, sy1);
    for (int i = 0; i < 8; i++) view_zoom_at(&v, vp, iw, ih, mx, my, VIEW_WHEEL_STEP);
    double sx2 = source_x_under(&v, vp, iw, ih, mx), sy2 = source_y_under(&v, vp, iw, ih, my);
    CHECK(fabs(sx2 - sx0) <= 2.0 && fabs(sy2 - sy0) <= 2.0,
          "and stays put over many notches (%.1f,%.1f -> %.1f,%.1f)", sx0, sy0, sx2, sy2);
    Rect zoomed = view_rect(&v, vp, iw, ih);
    CHECK(zoomed.w > vp.w && zoomed.h > vp.h, "the frame now overflows the viewport");

    /* Zooming out about the same point comes back to where it started. */
    for (int i = 0; i < 9; i++) view_zoom_at(&v, vp, iw, ih, mx, my, 1.0 / VIEW_WHEEL_STEP);
    CHECK(fabs(v.zoom - z0) < 1e-6, "nine notches out undo nine notches in (%.4f vs %.4f)", v.zoom, z0);
    Rect back = view_rect(&v, vp, iw, ih);
    Rect fitted = rect_fit(vp, iw, ih);
    CHECK(abs(back.x - fitted.x) <= 1 && abs(back.y - fitted.y) <= 1 &&
          abs(back.w - fitted.w) <= 1 && abs(back.h - fitted.h) <= 1,
          "and the picture is back where fit put it (%d,%d %dx%d vs %d,%d %dx%d)",
          back.x, back.y, back.w, back.h, fitted.x, fitted.y, fitted.w, fitted.h);

    /* The zoom clamps at both ends rather than running away. */
    for (int i = 0; i < 100; i++) view_zoom_at(&v, vp, iw, ih, mx, my, VIEW_WHEEL_STEP);
    CHECK(v.zoom == VIEW_ZOOM_MAX, "zoom clamps at the maximum (got %.3f)", v.zoom);
    for (int i = 0; i < 100; i++) view_zoom_at(&v, vp, iw, ih, mx, my, 1.0 / VIEW_WHEEL_STEP);
    CHECK(v.zoom == VIEW_ZOOM_MIN, "zoom clamps at the minimum (got %.5f)", v.zoom);

    /* Panning moves the rect by exactly the mouse delta, and is not clamped. */
    view_init(&v);
    Rect before = view_rect(&v, vp, iw, ih);
    view_pan(&v, vp, iw, ih, 30, -20);
    CHECK(!v.fit, "panning leaves fit mode");
    Rect after = view_rect(&v, vp, iw, ih);
    CHECK(after.x == before.x + 30 && after.y == before.y - 20 && after.w == before.w && after.h == before.h,
          "a pan of (30,-20) moves the fitted rect by that (%d,%d -> %d,%d)",
          before.x, before.y, after.x, after.y);
    view_pan(&v, vp, iw, ih, 5000, 5000);
    after = view_rect(&v, vp, iw, ih);
    CHECK(after.x > vp.x + vp.w && after.y > vp.y + vp.h, "a pan may take the frame right off the viewport");
    view_pan(&v, vp, iw, ih, 0, 0);
    CHECK(rect_eq(view_rect(&v, vp, iw, ih), after), "a zero pan changes nothing");

    /* Fit restores the fitted rect from anywhere. */
    view_fit(&v);
    CHECK(rect_eq(view_rect(&v, vp, iw, ih), rect_fit(vp, iw, ih)), "fit restores the fitted rect");

    /* A resize in free mode keeps the same source point at the centre. */
    view_one_to_one(&v, iw, ih);
    view_pan(&v, vp, iw, ih, -100, -50);
    double cx = v.cx, cy = v.cy;
    Rect bigger = rect_make(0, 0, 1400, 900);
    Rect r1 = view_rect(&v, bigger, iw, ih);
    CHECK(v.cx == cx && v.cy == cy, "the centre point is untouched by a resize");
    double under_cx = (bigger.x + bigger.w / 2.0 - r1.x) / v.zoom;
    double under_cy = (bigger.y + bigger.h / 2.0 - r1.y) / v.zoom;
    CHECK(fabs(under_cx - cx) <= 1.0 && fabs(under_cy - cy) <= 1.0,
          "after a resize the same source point sits at the viewport centre (%.1f,%.1f vs %.1f,%.1f)",
          under_cx, under_cy, cx, cy);
}

/* The wheel and a drag on the picture, through the same entry points the
 * event loop uses. */
static void test_viewport_mouse(App *a)
{
    printf("viewport: wheel zooms about the cursor, drag pans\n");
    Rect vp = a->layout.viewport;
    int iw = a->src_w, ih = a->src_h;

    app_key(a, KEY_FIT);
    app_pause(a);
    app_set_frame(a, 30);

    /* A wheel notch over the transport does nothing. */
    Rect tr = a->layout.transport;
    app_wheel(a, tr.x + tr.w / 2, tr.y + tr.h / 2, +1);
    CHECK(a->view.fit, "the wheel over the transport does not zoom");

    /* Over the picture it zooms in about the cursor. */
    int mx = vp.x + vp.w * 3 / 4, my = vp.y + vp.h / 4;
    double sx0 = source_x_under(&a->view, vp, iw, ih, mx);
    double sy0 = source_y_under(&a->view, vp, iw, ih, my);
    double z0 = view_scale(&a->view, vp, iw, ih);
    app_wheel(a, mx, my, +2);
    CHECK(!a->view.fit, "the wheel over the picture leaves fit mode");
    CHECK(fabs(a->view.zoom - z0 * VIEW_WHEEL_STEP * VIEW_WHEEL_STEP) < 1e-9,
          "two notches are two wheel steps (%.4f vs %.4f)", a->view.zoom, z0 * VIEW_WHEEL_STEP * VIEW_WHEEL_STEP);
    CHECK(fabs(source_x_under(&a->view, vp, iw, ih, mx) - sx0) <= 2.0 &&
          fabs(source_y_under(&a->view, vp, iw, ih, my) - sy0) <= 2.0,
          "the pixel under the cursor stays under it");
    app_wheel(a, mx, my, -2);
    CHECK(fabs(a->view.zoom - z0) < 1e-9, "two notches back restore the scale (%.4f vs %.4f)", a->view.zoom, z0);
    CHECK(a->need_redraw, "zooming asks for a redraw");

    /* A drag on the picture pans it, and neither pauses nor scrubs. */
    app_key(a, KEY_FIT);
    app_key(a, KEY_PLAY_FWD);
    int frame = a->current;
    Rect before = view_rect(&a->view, vp, iw, ih);
    int px = vp.x + vp.w / 2, py = vp.y + vp.h / 2;
    app_mouse_down(a, px, py, MOUSE_LEFT);
    app_mouse_move(a, px + 40, py + 25, 1);
    app_mouse_move(a, px + 70, py + 15, 1);
    Rect after = view_rect(&a->view, vp, iw, ih);
    CHECK(!a->view.fit, "dragging the picture leaves fit mode");
    CHECK(after.x == before.x + 70 && after.y == before.y + 15,
          "the picture follows the drag (%d,%d -> %d,%d)", before.x, before.y, after.x, after.y);
    CHECK(a->play_dir == +1, "dragging the picture does not pause playback");
    CHECK(a->current == frame, "dragging the picture does not scrub (frame %d -> %d)", frame, a->current);
    app_mouse_up(a, px + 70, py + 15, MOUSE_LEFT);
    app_mouse_move(a, px + 200, py + 200, 0);
    CHECK(rect_eq(view_rect(&a->view, vp, iw, ih), after), "after release, moving the mouse no longer pans");
    app_pause(a);

    /* A press on the transport does not start a pan. */
    app_key(a, KEY_FIT);
    app_mouse_down(a, tr.x + tr.w / 2, tr.y + 2, MOUSE_LEFT);
    app_mouse_move(a, tr.x + tr.w / 2 + 30, tr.y + 2, 1);
    app_mouse_up(a, tr.x + tr.w / 2 + 30, tr.y + 2, MOUSE_LEFT);
    CHECK(a->view.fit, "a drag that starts on the transport does not pan");

    /* The middle button drags the picture just like the left one, and it
     * does not scrub when pressed on the timeline. */
    app_key(a, KEY_FIT);
    app_set_frame(a, 30);
    before = view_rect(&a->view, vp, iw, ih);
    app_mouse_down(a, px, py, APP_MOUSE_MIDDLE);
    app_mouse_move(a, px - 25, py + 10, APP_HELD_MIDDLE);
    after = view_rect(&a->view, vp, iw, ih);
    CHECK(!a->view.fit, "a middle-button drag leaves fit mode");
    CHECK(after.x == before.x - 25 && after.y == before.y + 10,
          "the picture follows a middle-button drag (%d,%d -> %d,%d)", before.x, before.y, after.x, after.y);
    app_mouse_move(a, px - 25, py + 10, APP_HELD_MIDDLE | APP_HELD_LEFT);
    app_mouse_up(a, px - 25, py + 10, APP_MOUSE_LEFT);
    app_mouse_move(a, px + 100, py + 10, APP_HELD_MIDDLE);
    CHECK(view_rect(&a->view, vp, iw, ih).x == after.x + 125,
          "releasing the left button does not end a middle-button pan");
    app_mouse_up(a, px + 100, py + 10, APP_MOUSE_MIDDLE);
    app_mouse_move(a, px + 300, py + 10, 0);
    CHECK(view_rect(&a->view, vp, iw, ih).x == after.x + 125, "releasing the middle button ends the pan");
    int ty = a->layout.track.y + a->layout.track.h / 2;
    app_mouse_down(a, ui_x_for_frame(&a->layout, 60, a->seq->count), ty, APP_MOUSE_MIDDLE);
    app_mouse_move(a, ui_x_for_frame(&a->layout, 70, a->seq->count), ty, APP_HELD_MIDDLE);
    app_mouse_up(a, ui_x_for_frame(&a->layout, 70, a->seq->count), ty, APP_MOUSE_MIDDLE);
    CHECK(a->current == 30, "the middle button does not scrub the timeline (frame %d)", a->current);
    app_key(a, KEY_FIT);
}

static void test_memory_budget(const Sequence *seq, const ColorLUT *lut)
{
    printf("memory budget and eviction\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    size_t limit = per_frame * 8; /* deliberately far smaller than the sequence */

    Cache *c = cache_create(seq, lut, limit, 4, per_frame);
    CHECK(c != NULL, "cache creates with a small budget");
    if (!c) return;

    cache_set_focus(c, 0, +1);
    wait_for_cache(c, 6, 10.0);

    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.bytes_used <= limit, "cache must stay inside its budget (%zu of %zu)",
          st.bytes_used, limit);
    CHECK(st.ready > 0 && st.ready <= 9, "cache holds about the budgeted frame count (%d)", st.ready);

    /* Frames near the playhead are the ones worth keeping. */
    CHECK(cache_frame_state(c, 0) == CACHE_READY, "the frame under the playhead is resident");

    /* Move the playhead far away; the loaders must follow it and the old
     * region must be given up rather than the cache growing. */
    int far = seq->count - 1;
    cache_set_focus(c, far, +1);
    double t0 = rp_now();
    while (cache_frame_state(c, far) != CACHE_READY && rp_now() - t0 < 10.0) {
        rp_sleep_ms(20);
    }
    CHECK(cache_frame_state(c, far) == CACHE_READY, "scrubbing elsewhere loads the new frame");

    cache_get_stats(c, &st);
    CHECK(st.bytes_used <= limit, "cache stays inside its budget after scrubbing (%zu of %zu)",
          st.bytes_used, limit);

    /* A reference taken by the drawing code must survive eviction. */
    Image *held = cache_acquire(c, far);
    CHECK(held != NULL, "can acquire a resident frame");
    if (held) {
        cache_set_focus(c, 0, +1);
        wait_for_cache(c, 4, 10.0);
        /* Reading the pixels must still be valid even if `far` was evicted. */
        volatile uint32_t probe = held->px[(size_t)held->width * held->height - 1];
        (void)probe;
        CHECK(held->width == seq->width, "held image stays intact after eviction");
        image_unref(held);
    }

    cache_destroy(c);
}

/* A budget too small for two frames cannot let playback advance, so the cache
 * raises it to that floor. Without it the player would sit on frame one
 * forever, which looks like a hang rather than a configuration problem. */
static void test_minimum_budget(const Sequence *seq, const ColorLUT *lut)
{
    printf("budget below one frame\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, 1024, 2, per_frame); /* absurdly small */
    CHECK(c != NULL, "cache creates with an unusable budget");
    if (!c) return;

    App app;
    app_init(&app, seq, c, 24.0);
    app_resize(&app, WIN_W, WIN_H);

    CHECK(wait_for_cache(c, 2, 15.0), "at least two frames become resident");

    app_toggle_play(&app, +1);
    CHECK(advance_one(&app), "playback can still advance on a minimal budget");
    /* With exactly two frames of room, the frame just shown must be worth
     * less than the next one ahead so that it is the one given up; otherwise
     * playback would stop here for good. */
    CHECK(advance_one(&app), "and advance again, giving up the frame just shown");
    CHECK(advance_one(&app), "and keep advancing");

    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.bytes_limit >= per_frame * 2, "the budget was raised to hold two frames");

    app_shutdown(&app);
    cache_destroy(c);
}

/* Waits until no loader is busy, or gives up after the timeout. */
static int wait_settled(Cache *c, double timeout)
{
    CacheStats st;
    double t0 = rp_now();
    do {
        rp_sleep_ms(20);
        cache_get_stats(c, &st);
    } while (st.loading > 0 && rp_now() - t0 < timeout);
    return st.loading == 0;
}

static int all_ready(Cache *c, int from, int to)
{
    for (int f = from; f <= to; f++)
        if (cache_frame_state(c, f) != CACHE_READY) return 0;
    return 1;
}

/* Polls until every frame in from..to is resident, or the timeout passes. */
static int wait_all_ready(Cache *c, int from, int to, double timeout)
{
    double t0 = rp_now();
    while (!all_ready(c, from, to) && rp_now() - t0 < timeout) rp_sleep_ms(5);
    return all_ready(c, from, to);
}

static int none_ready(Cache *c, int from, int to)
{
    for (int f = from; f <= to; f++)
        if (cache_frame_state(c, f) == CACHE_READY) return 0;
    return 1;
}

/* Plays forwards frame by frame the way the player does, waiting for each
 * frame. The resident region must run ahead of the playhead: frames behind are
 * given up as frames ahead are loaded, and every load admitted must land,
 * since one freed slot should start one load, not one per thread. Then
 * reverses: the frames given up first must be the ones farthest from the
 * playhead, so the frames just shown are still there while the loaders turn
 * around. That is checked as an invariant sampled while the loaders run,
 * rather than at one instant: the surviving old frames must always be a
 * contiguous run starting at the nearest one, which nearest-first eviction
 * breaks on its very first eviction. */
static void test_recycling_order(const Sequence *seq, const ColorLUT *lut)
{
    printf("recycling order: farthest first\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    const int budget_frames = 20;
    Cache *c = cache_create(seq, lut, per_frame * budget_frames, 4, per_frame);
    CHECK(c != NULL, "cache creates with a budget of %d frames", budget_frames);
    if (!c) return;

    const int last = 40;
    int reached = -1;
    for (int f = 0; f <= last; f++) {
        cache_set_focus(c, f, +1);
        double t0 = rp_now();
        while (cache_frame_state(c, f) != CACHE_READY && rp_now() - t0 < 10.0) rp_sleep_ms(5);
        if (cache_frame_state(c, f) != CACHE_READY) break;
        reached = f;
    }
    CHECK(reached == last, "played forwards to frame %d (got %d)", last, reached);
    CHECK(wait_settled(c, 10.0), "loaders settle after playing");

    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.bytes_used <= per_frame * budget_frames, "budget holds (%zu of %zu)",
          st.bytes_used, per_frame * budget_frames);

    /* Playing forwards, the budget runs ahead: the frames behind were given up
     * as the frames ahead came in. */
    const int old_lo = last, old_hi = last + budget_frames - 2;
    CHECK(all_ready(c, old_lo, old_hi), "the region ahead of the playhead (%d..%d) is resident",
          old_lo, old_hi);
    CHECK(none_ready(c, 0, last - 2), "the frames behind (0..%d) were given up", last - 2);
    CHECK(st.discarded == 0, "every load admitted while playing landed (%d discarded)", st.discarded);

    /* Reverse. While the loaders refill behind, sample the old region: reading
     * it in increasing order, farthest-first eviction can only ever show a
     * run of resident frames from old_lo up to some point, then nothing. */
    cache_set_focus(c, last - 1, -1);
    int prefix_always = 1, partial_seen = 0, resident = -1;
    double t0 = rp_now();
    while (rp_now() - t0 < 10.0) {
        resident = 0;
        int prefix = 1;
        for (int f = old_lo; f <= old_hi; f++) {
            int ready = cache_frame_state(c, f) == CACHE_READY;
            if (ready && f != old_lo + resident) prefix = 0;
            if (ready) resident++;
        }
        if (!prefix) prefix_always = 0;
        if (resident > 0 && resident < old_hi - old_lo + 1) partial_seen = 1;
        if (resident == 0) break;
        rp_sleep_ms(2);
    }
    CHECK(resident == 0, "after reversing, the old region is eventually recycled (%d left)", resident);
    CHECK(partial_seen, "the refill was observed in progress");
    CHECK(prefix_always, "the old region was always given up from its far end first");
    CHECK(all_ready(c, last - 8, last - 1), "frames %d..%d behind the reversal loaded", last - 8, last - 1);

    cache_destroy(c);
}

/* A budget holding more than half the sequence must still run the whole
 * budget ahead of the playhead while playing forwards; the frames just shown
 * are given up for frames farther ahead even past the midpoint of the loop. */
static void test_large_budget_runs_ahead(const Sequence *seq, const ColorLUT *lut)
{
    printf("a budget over half the sequence still runs ahead\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    const int budget_frames = seq->count / 2 + 8; /* 56 of 96 */
    Cache *c = cache_create(seq, lut, per_frame * budget_frames, 4, per_frame);
    CHECK(c != NULL, "cache creates with a budget of %d frames", budget_frames);
    if (!c) return;

    const int last = 24;
    int reached = -1;
    for (int f = 0; f <= last; f++) {
        cache_set_focus(c, f, +1);
        double t0 = rp_now();
        while (cache_frame_state(c, f) != CACHE_READY && rp_now() - t0 < 10.0) rp_sleep_ms(5);
        if (cache_frame_state(c, f) != CACHE_READY) break;
        reached = f;
    }
    CHECK(reached == last, "played forwards to frame %d (got %d)", last, reached);
    CHECK(wait_settled(c, 10.0), "loaders settle after playing");

    CHECK(all_ready(c, last, last + budget_frames - 1),
          "the whole budget (%d..%d) is ahead of the playhead", last, last + budget_frames - 1);
    CHECK(none_ready(c, 0, last - 1), "the frames behind (0..%d) were given up", last - 1);

    cache_destroy(c);
}

/* Drives the cache the way a user dragging the playhead around does: constant
 * focus changes against a budget far too small to hold the sequence, while
 * frames are still being decoded. Run under a sanitiser this is the best shot
 * at catching a race between the loaders and the drawing code. */
static void test_scrub_stress(const Sequence *seq, const ColorLUT *lut)
{
    printf("scrubbing under load\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, per_frame * 6, 4, per_frame);
    CHECK(c != NULL, "stress cache creates");
    if (!c) return;

    App app;
    app_init(&app, seq, c, 24.0);
    app_resize(&app, WIN_W, WIN_H);

    Rect track = app.layout.track;
    int ty = track.y + track.h / 2;
    unsigned seed = 0x9e3779b9u;
    int shown = 0, iterations = 0;
    double t0 = rp_now();

    while (rp_now() - t0 < 3.0) {
        seed = seed * 1103515245u + 12345u;
        int x = track.x + (int)((seed >> 8) % (unsigned)track.w);

        app_mouse_down(&app, x, ty, MOUSE_LEFT);
        for (int k = 0; k < 5; k++) {
            seed = seed * 1103515245u + 12345u;
            app_mouse_move(&app, track.x + (int)((seed >> 8) % (unsigned)track.w), ty, 1);
            app_update_shown(&app);
        }
        app_mouse_up(&app, x, ty, MOUSE_LEFT);

        /* Occasionally play for a moment, so the loaders see a direction
         * change as well as a jump. */
        if ((seed & 7) == 0) {
            app_toggle_play(&app, (seed & 8) ? +1 : -1);
            app.next_due = rp_now() - 1.0;
            app_tick(&app);
            app_pause(&app);
        }

        app_update_shown(&app);
        if (app.shown) shown++;
        iterations++;
    }

    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.bytes_used <= per_frame * 6,
          "the budget holds through %d scrubs (%zu of %zu bytes)",
          iterations, st.bytes_used, per_frame * 6);
    CHECK(shown > 0, "frames were displayed while scrubbing (%d of %d)", shown, iterations);
    CHECK(app.current >= 0 && app.current < seq->count, "the playhead stayed in range");

    app_shutdown(&app);
    cache_destroy(c);
}

/* Dragging sets the loaders' direction. After a press fills the budget ahead
 * of frame 80 in the default forward direction, dragging leftwards must turn
 * the read-ahead round: once the loaders settle, the frames below the cursor
 * are the resident ones and the frames above it have been given up. Without
 * the change the direction stays forwards and the loaders fill above the
 * cursor, which is where the drag came from. */
static void test_drag_sets_direction(const Sequence *seq, const ColorLUT *lut)
{
    printf("dragging turns the read-ahead round\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    const int budget_frames = 8;
    Cache *c = cache_create(seq, lut, per_frame * budget_frames, 4, per_frame);
    CHECK(c != NULL, "cache creates with a budget of %d frames", budget_frames);
    if (!c) return;

    App app;
    app_init(&app, seq, c, 24.0);
    app_resize(&app, WIN_W, WIN_H);
    const Layout *L = &app.layout;
    int n = seq->count;
    int ty = L->track.y + L->track.h / 2;

    const int start = 80, end = 70;
    app_mouse_down(&app, ui_x_for_frame(L, start, n), ty, MOUSE_LEFT);
    wait_all_ready(c, start, start + budget_frames - 1, 10.0);
    wait_settled(c, 10.0);
    CHECK(all_ready(c, start, start + budget_frames - 1),
          "the press fills the budget ahead (%d..%d)", start, start + budget_frames - 1);

    for (int f = start - 1; f >= end; f--) app_mouse_move(&app, ui_x_for_frame(L, f, n), ty, 1);
    CHECK(app.current == end, "the drag ends on frame %d (got %d)", end, app.current);
    CHECK(app.last_dir == -1, "a leftward drag sets the direction backwards (got %d)", app.last_dir);
    app_mouse_up(&app, ui_x_for_frame(L, end, n), ty, MOUSE_LEFT);
    CHECK(app.last_dir == -1, "the direction stays after release (got %d)", app.last_dir);

    const int lo = end - budget_frames + 1;
    wait_all_ready(c, lo, end, 10.0);
    CHECK(wait_settled(c, 10.0), "loaders settle after the drag");
    CHECK(all_ready(c, lo, end), "the budget now runs below the cursor (%d..%d)", lo, end);
    CHECK(none_ready(c, end + 1, start + budget_frames - 1),
          "the frames above the cursor (%d..%d) were given up", end + 1, start + budget_frames - 1);

    app_shutdown(&app);
    cache_destroy(c);
}

/* While the frame under the playhead is loading, the viewport shows the
 * nearest resident frame rather than whatever was last on screen. With frames
 * 0..7 resident and frame 0 on screen, pressing on a cold frame must put a
 * nearer frame up at once, and the real frame once it lands. */
static void test_nearest_resident_stand_in(const Sequence *seq, const ColorLUT *lut)
{
    printf("nearest resident frame stands in while loading\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    const int budget_frames = 8;
    Cache *c = cache_create(seq, lut, per_frame * budget_frames, 4, per_frame);
    CHECK(c != NULL, "cache creates with a budget of %d frames", budget_frames);
    if (!c) return;

    App app;
    app_init(&app, seq, c, 24.0);
    app_resize(&app, WIN_W, WIN_H);
    const Layout *L = &app.layout;
    int n = seq->count;
    int ty = L->track.y + L->track.h / 2;

    wait_all_ready(c, 0, budget_frames - 1, 10.0);
    wait_settled(c, 10.0);
    app_update_shown(&app);
    CHECK(app.shown && app.shown_frame == 0, "frame 0 is on screen to begin with");

    const int cold = 50, edge = budget_frames - 1;
    app_mouse_down(&app, ui_x_for_frame(L, cold, n), ty, MOUSE_LEFT);
    app_update_shown(&app);
    CHECK(app.shown != NULL, "something is on screen after pressing a cold frame");
    CHECK(app.shown_frame != 0, "the stale frame 0 is not left on screen");
    CHECK(abs(app.shown_frame - cold) <= cold - edge,
          "the stand-in is no farther from the playhead than the nearest resident frame was (showing %d)",
          app.shown_frame);
    CHECK(app.current == cold, "the playhead itself is on the cold frame (got %d)", app.current);

    wait_all_ready(c, cold, cold, 10.0);
    app_update_shown(&app);
    CHECK(app.shown_frame == cold, "once it lands the real frame replaces the stand-in (showing %d)",
          app.shown_frame);
    app_mouse_up(&app, ui_x_for_frame(L, cold, n), ty, MOUSE_LEFT);

    app_shutdown(&app);
    cache_destroy(c);
}

/* A decode whose frame the playhead has jumped away from is abandoned. With
 * one loader, sending the focus to a far frame and straight back must, over a
 * few tries, catch the decode in progress and cancel it; the cancelled frame
 * is left empty, not failed, and no error is reported. Each fixture frame
 * decodes in dozens of chunks, so the poll between chunks gets its chance. */
static void test_abort_stale_decodes(const Sequence *seq, const ColorLUT *lut)
{
    printf("decodes left out of reach are abandoned\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    const int budget_frames = 8;
    Cache *c = cache_create(seq, lut, per_frame * budget_frames, 1, per_frame);
    CHECK(c != NULL, "cache creates with one loader");
    if (!c) return;

    cache_set_focus(c, 0, +1);
    wait_all_ready(c, 0, budget_frames - 1, 10.0);
    wait_settled(c, 10.0);

    const int far = 50;
    CacheStats st;
    int tries;
    for (tries = 0; tries < 200; tries++) {
        cache_set_focus(c, far, +1);
        double t0 = rp_now();
        while (cache_frame_state(c, far) == CACHE_EMPTY && rp_now() - t0 < 1.0) { /* spin */ }
        cache_set_focus(c, 0, +1);
        wait_settled(c, 10.0);
        cache_get_stats(c, &st);
        if (st.aborted > 0) break;
    }
    CHECK(st.aborted > 0, "a jump away cancelled a decode in progress (%d tries)", tries + 1);
    CHECK(st.failed == 0, "a cancelled decode is not a failure (%d failed)", st.failed);
    CHECK(cache_last_error(c) == NULL, "a cancelled decode reports no error");
    CHECK(cache_frame_state(c, far) == CACHE_EMPTY, "the cancelled frame is left empty (state %d)",
          cache_frame_state(c, far));
    CHECK(all_ready(c, 0, budget_frames - 1), "the frames around the playhead are still resident");

    /* And a frame the playhead comes back to still loads. */
    cache_set_focus(c, far, +1);
    CHECK(wait_all_ready(c, far, far, 10.0), "the frame loads when the playhead returns to it");

    cache_destroy(c);
}

/* ---- the reserve behind the playhead ------------------------------------ */

/* Plays forwards frame by frame to `last` the way the player does. */
static int play_to(Cache *c, int last)
{
    int reached = -1;
    for (int f = 0; f <= last; f++) {
        cache_set_focus(c, f, +1);
        double t0 = rp_now();
        while (cache_frame_state(c, f) != CACHE_READY && rp_now() - t0 < 10.0) rp_sleep_ms(5);
        if (cache_frame_state(c, f) != CACHE_READY) break;
        reached = f;
    }
    return reached == last;
}

/* With a reserve of 8 in a budget of 20, playing to frame 40 leaves 32..39
 * behind the playhead and 40..51 ahead, and nothing else. Reversing then
 * finds 32..39 there at once, while the frames that were ahead become the
 * new reserve and the loaders fill below. */
static void test_reserve_images(const Sequence *seq, const ColorLUT *lut)
{
    printf("reserve behind the playhead: image sequence\n");

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, per_frame * 20, 4, per_frame);
    CHECK(c != NULL, "cache creates with a budget of 20 frames");
    if (!c) return;
    cache_set_reserve(c, 8);

    CHECK(play_to(c, 40), "played forwards to frame 40");
    CHECK(wait_settled(c, 10.0), "loaders settle");
    CHECK(all_ready(c, 32, 39), "the reserve 32..39 is resident");
    CHECK(all_ready(c, 40, 51), "the window ahead 40..51 is resident");
    CHECK(none_ready(c, 0, 31), "nothing older than the reserve is kept");
    CHECK(none_ready(c, 52, seq->count - 1), "the read-ahead stops at the window");

    cache_set_focus(c, 39, -1);
    CHECK(all_ready(c, 32, 39), "reversing finds the reserve resident at once");
    int misses = 0;
    for (int f = 39; f >= 32; f--) {
        cache_set_focus(c, f, -1);
        if (cache_frame_state(c, f) != CACHE_READY) misses++;
    }
    CHECK(misses == 0, "playing back through the reserve never waits (%d misses)", misses);
    /* At frame 32 going backwards the window is 32..21 and the reserve 33..40. */
    CHECK(wait_all_ready(c, 21, 31, 10.0), "the loaders fill below the reversal (21..31)");
    CHECK(wait_settled(c, 10.0), "and settle");
    CHECK(all_ready(c, 33, 40), "the frames just played are now the reserve (33..40)");

    cache_destroy(c);
}

/* ---- video ------------------------------------------------------------- */

static Sequence *open_video(const char *path)
{
    char err[256];
    char *one[1] = { (char *)path };
    Sequence *s = sequence_open(one, 1, err, sizeof err);
    if (!s) printf("  cannot open %s: %s\n", path, err);
    return s;
}

/* Frame i of the fixtures is a flat colour encoding i; see
 * tools/make_test_video.sh. */
static int video_frame_is(const Image *im, int i)
{
    int r = (i * 53) & 255, g = (i * 97) & 255, b = (i * 29) & 255;
    uint32_t p = im->px[(size_t)(im->height / 2) * im->width + im->width / 2];
    int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
    int ok = abs(pr - r) <= 12 && abs(pg - g) <= 12 && abs(pb - b) <= 12;
    if (!ok) printf("  frame %d: got R%d G%d B%d, wanted R%d G%d B%d\n", i, pr, pg, pb, r, g, b);
    return ok;
}

static int resident_frame_is(Cache *c, int f)
{
    Image *im = cache_acquire(c, f);
    int ok = im && video_frame_is(im, f);
    image_unref(im);
    return ok;
}

/* Two loaders, a budget of 30 frames, groups of 24. Filling from frame 0
 * must decode exactly the 30 frames that fit, once each: one loader takes
 * group 0, the other group 1 and stops when the budget is full. A scrub to
 * frame 50 seeks to its group and lands it, then the rest of the group. */
static void test_video_groups(const ColorLUT *lut)
{
    printf("video: groups fill without waste, scrubbing seeks\n");
    Sequence *seq = open_video("test/video/h264_gop24.mp4");
    if (!seq) { CHECK(0, "the video fixture opens"); return; }

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, per_frame * 30, 2, per_frame);
    CHECK(c != NULL, "a video cache creates");
    if (!c) { sequence_free(seq); return; }

    cache_set_focus(c, 0, +1);
    CHECK(wait_all_ready(c, 0, 29, 20.0), "frames 0..29 fill the budget");
    CHECK(wait_settled(c, 10.0), "loaders settle");

    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.ready == 30, "exactly the budget is resident (%d)", st.ready);
    CHECK(st.bytes_used <= per_frame * 30, "the budget holds");
    CHECK(st.discarded == 0, "no decoded frame was thrown away (%d)", st.discarded);
    CHECK(st.delivered == 30, "no frame was decoded twice (%d delivered for 30 resident)", st.delivered);
    CHECK(st.failed == 0 && cache_last_error(c) == NULL, "nothing failed");
    CHECK(resident_frame_is(c, 17) && resident_frame_is(c, 29), "resident frames are the right pictures");

    cache_set_focus(c, 50, +1);
    CHECK(wait_all_ready(c, 50, 50, 10.0), "a scrub to frame 50 lands it");
    CHECK(resident_frame_is(c, 50), "and it is the right picture");
    CHECK(wait_all_ready(c, 48, 71, 10.0), "the rest of its group follows (48..71)");
    cache_get_stats(c, &st);
    CHECK(st.failed == 0, "still nothing failed");

    cache_destroy(c);

    /* The scrub stress case, on video. */
    test_scrub_stress(seq, lut);
    sequence_free(seq);
}

/* One loader on the one-group file with a budget of 16: a jump far away
 * while the group decodes cancels it, leaves nothing failed, and the frame
 * the playhead comes back to loads again. The decode towards frame 60 runs
 * on from wherever the loader stopped, so the jump back waits until it is
 * well past the playhead's reach before pulling the playhead away. */
static void test_video_abort(const ColorLUT *lut)
{
    printf("video: a jump out of reach cancels the group\n");
    Sequence *seq = open_video("test/video/h264_onegop.mp4");
    if (!seq) { CHECK(0, "the one-group fixture opens"); return; }

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, per_frame * 16, 1, per_frame);
    CHECK(c != NULL, "a one-loader video cache creates");
    if (!c) { sequence_free(seq); return; }

    /* Budget 16, so the reserve is 8 and the window 8: 2..9 behind, 10..17
     * ahead. */
    cache_set_focus(c, 10, +1);
    CHECK(wait_all_ready(c, 2, 17, 20.0), "the budget fills around the playhead (2..17)");
    wait_settled(c, 10.0);

    CacheStats st;
    int tries;
    for (tries = 0; tries < 20; tries++) {
        cache_set_focus(c, 60, +1);
        double t0 = rp_now();
        while (cache_frame_state(c, 45) != CACHE_READY && rp_now() - t0 < 0.5) { /* spin */ }
        cache_set_focus(c, 10, +1);
        wait_settled(c, 10.0);
        cache_get_stats(c, &st);
        if (st.aborted > 0) break;
    }
    CHECK(st.aborted > 0, "a jump away cancelled a group decode in progress (%d tries)", tries + 1);
    CHECK(st.failed == 0, "a cancelled decode is not a failure (%d failed)", st.failed);
    CHECK(cache_last_error(c) == NULL, "a cancelled decode reports no error");
    CHECK(wait_all_ready(c, 10, 10, 10.0), "the frame the playhead returns to loads");
    CHECK(resident_frame_is(c, 10), "and is the right picture");

    cache_destroy(c);
    sequence_free(seq);
}

/* The video reserve follows the groups. With a budget of 30 and groups of
 * 24, playing to frame 40 keeps the shown part of its group plus the group
 * before, capped at half the budget: 25..39, with 40..54 ahead. Reversing
 * plays 39 down to 25 from RAM while the group below refills. */
static void test_video_reserve(const ColorLUT *lut)
{
    printf("video: a group's worth stays behind the playhead\n");
    Sequence *seq = open_video("test/video/h264_gop24.mp4");
    if (!seq) { CHECK(0, "the video fixture opens"); return; }

    size_t per_frame = (size_t)seq->width * seq->height * 4;
    Cache *c = cache_create(seq, lut, per_frame * 30, 2, per_frame);
    CHECK(c != NULL, "a video cache creates");
    if (!c) { sequence_free(seq); return; }

    CHECK(play_to(c, 40), "played forwards to frame 40");
    CHECK(wait_settled(c, 10.0), "loaders settle");
    CHECK(all_ready(c, 25, 39), "the reserve 25..39 is resident");
    CHECK(all_ready(c, 40, 54), "the window ahead 40..54 is resident");
    CHECK(none_ready(c, 0, 24), "nothing older than the reserve is kept");
    CHECK(none_ready(c, 55, 95), "the read-ahead stops at the window");

    /* Forward playback is one linear decode: 55 frames were needed, and a
     * pick always goes to the reader already positioned for it, so nothing
     * was decoded again from a keyframe. */
    CacheStats st;
    cache_get_stats(c, &st);
    CHECK(st.decoded <= 55 + 4, "playing to 40 decoded each frame once (%d pictures for 55 frames)", st.decoded);
    CHECK(st.discarded == 0, "and threw none away (%d)", st.discarded);

    cache_set_focus(c, 39, -1);
    int misses = 0;
    for (int f = 39; f >= 25; f--) {
        cache_set_focus(c, f, -1);
        if (cache_frame_state(c, f) != CACHE_READY) misses++;
    }
    CHECK(misses == 0, "playing back through the reserve never waits (%d misses)", misses);
    CHECK(wait_all_ready(c, 11, 24, 20.0), "the group below refills behind the reversal (11..24)");
    CHECK(wait_settled(c, 10.0), "and settles");
    CHECK(all_ready(c, 26, 40), "the frames just played are now the reserve (26..40)");
    CHECK(resident_frame_is(c, 12) && resident_frame_is(c, 30), "resident frames are the right pictures");

    cache_destroy(c);
    sequence_free(seq);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *input = (argc > 1) ? argv[1] : "test/seq_a";

    char err[512];
    char *const inputs[1] = { (char *)input };
    Sequence *seq = sequence_open(inputs, 1, err, sizeof err);
    if (!seq) {
        printf("cannot open '%s': %s\n", input, err);
        return 2;
    }
    if (!reader_probe(seq->frames[0].path, &seq->width, &seq->height, NULL, err, sizeof err)) {
        printf("cannot probe '%s': %s\n", seq->frames[0].path, err);
        return 2;
    }
    printf("sequence: %s, %d frames, %dx%d\n\n", seq->display, seq->count, seq->width, seq->height);

    font_init();
    static ColorLUT lut;
    color_lut_init(&lut);

    Cache *cache = cache_create(seq, &lut, (size_t)1024 * 1024 * 1024, 4,
                                (size_t)seq->width * seq->height * 4);
    if (!cache) { printf("cannot create cache\n"); return 2; }

    App app;
    app_init(&app, seq, cache, 24.0);
    app_resize(&app, WIN_W, WIN_H);

    test_view();
    test_timeline_mapping(&app);
    test_timeline_click_and_drag(&app);
    test_transport_buttons(&app);
    test_step_wrapping(&app);
    test_keyboard(&app);
    test_viewport_mouse(&app);

    wait_for_cache(cache, seq->count, 30.0);
    test_playback_loops(&app);

    app_shutdown(&app);
    cache_destroy(cache);

    test_memory_budget(seq, &lut);
    test_minimum_budget(seq, &lut);
    test_recycling_order(seq, &lut);
    test_large_budget_runs_ahead(seq, &lut);
    test_drag_sets_direction(seq, &lut);
    test_nearest_resident_stand_in(seq, &lut);
    test_abort_stale_decodes(seq, &lut);
    test_scrub_stress(seq, &lut);
    test_reserve_images(seq, &lut);

    sequence_free(seq);

    reader_set_decoder_threads(2);
    test_video_groups(&lut);
    test_video_abort(&lut);
    test_video_reserve(&lut);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
