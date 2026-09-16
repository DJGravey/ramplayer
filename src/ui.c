#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "font.h"
#include "util.h"

/* ---- palette ------------------------------------------------------------ */

#define COL_VIEWPORT   RP_RGB(0x0b, 0x0c, 0x0e)
#define COL_PANEL      RP_RGB(0x1b, 0x1d, 0x22)
#define COL_STATUS     RP_RGB(0x15, 0x17, 0x1b)
#define COL_EDGE       RP_RGB(0x2d, 0x31, 0x39)
#define COL_TEXT       RP_RGB(0xcb, 0xd0, 0xd8)
#define COL_TEXT_DIM   RP_RGB(0x79, 0x80, 0x8d)
#define COL_ACCENT     RP_RGB(0x4c, 0x9d, 0xf0)
#define COL_TRACK      RP_RGB(0x25, 0x28, 0x2f)
#define COL_CACHED     RP_RGB(0x2e, 0x7b, 0x59)
#define COL_LOADING    RP_RGB(0x8c, 0x6e, 0x2c)
#define COL_FAILED     RP_RGB(0xa0, 0x3b, 0x3b)
#define COL_PLAYHEAD   RP_RGB(0xff, 0xcf, 0x52)
#define COL_BTN        RP_RGB(0x2a, 0x2e, 0x36)
#define COL_WHITE      RP_RGB(0xff, 0xff, 0xff)
#define COL_BLACK      RP_RGB(0x00, 0x00, 0x00)

/* Base metrics, in unscaled pixels. */
#define TRANSPORT_H 58
#define STATUS_H    22
#define BTN_SIZE    38
#define BTN_GAP     5
#define PAD         8
#define RULER_H     15

static int S = 1; /* UI scale, for HiDPI displays */

void ui_set_scale(int scale) { S = RP_CLAMP(scale, 1, 4); }
int  ui_get_scale(void) { return S; }

/* ---- layout ------------------------------------------------------------- */

void ui_layout(Layout *L, int w, int h)
{
    memset(L, 0, sizeof *L);
    L->window = rect_make(0, 0, w, h);

    int transport_h = TRANSPORT_H * S;
    int status_h    = STATUS_H * S;
    if (transport_h + status_h > h) {
        transport_h = RP_MIN(transport_h, h);
        status_h    = h - transport_h;
    }
    int vp_h = h - transport_h - status_h;

    L->viewport  = rect_make(0, 0, w, vp_h);
    L->statusbar = rect_make(0, vp_h, w, status_h);
    L->transport = rect_make(0, vp_h + status_h, w, transport_h);

    int btn = BTN_SIZE * S;
    int gap = BTN_GAP * S;
    int pad = PAD * S;

    int by = L->transport.y + (transport_h - btn) / 2;
    int bx = pad;
    for (int i = 0; i < BTN_COUNT; i++) {
        L->buttons[i] = rect_make(bx, by, btn, btn);
        bx += btn + gap;
    }

    int tl_x = bx + gap * 2;
    int tl_w = RP_MAX(0, w - pad - tl_x);
    L->timeline = rect_make(tl_x, L->transport.y + 5 * S, tl_w, transport_h - 10 * S);

    int ruler = RP_MIN(RULER_H * S, RP_MAX(0, L->timeline.h - 6 * S));
    L->track = rect_make(L->timeline.x, L->timeline.y + ruler,
                         RP_MAX(1, L->timeline.w), RP_MAX(1, L->timeline.h - ruler));
}

int ui_frame_at_x(const Layout *L, int x, int count)
{
    if (count <= 1) return 0;
    int span = L->track.w - 1;
    if (span <= 0) return 0;
    double t = (double)(x - L->track.x) / (double)span;
    t = RP_CLAMP(t, 0.0, 1.0);
    int f = (int)(t * (double)(count - 1) + 0.5);
    return RP_CLAMP(f, 0, count - 1);
}

int ui_x_for_frame(const Layout *L, int frame, int count)
{
    if (count <= 1) return L->track.x;
    int span = L->track.w - 1;
    if (span <= 0) return L->track.x;
    frame = RP_CLAMP(frame, 0, count - 1);
    return L->track.x + (int)((double)frame / (double)(count - 1) * (double)span + 0.5);
}

int ui_button_at(const Layout *L, int x, int y)
{
    for (int i = 0; i < BTN_COUNT; i++)
        if (rect_contains(L->buttons[i], x, y)) return i;
    return -1;
}

/* ---- icons -------------------------------------------------------------- */

static void icon_play(Surface *s, int cx, int cy, int hw, int hh, int dir, uint32_t c)
{
    if (dir > 0) draw_triangle(s, cx - hw, cy - hh, cx - hw, cy + hh, cx + hw, cy, c);
    else         draw_triangle(s, cx + hw, cy - hh, cx + hw, cy + hh, cx - hw, cy, c);
}

static void icon_pause(Surface *s, int cx, int cy, int hw, int hh, uint32_t c)
{
    int bw = RP_MAX(2, hw * 2 / 3);
    draw_rect(s, rect_make(cx - hw, cy - hh, bw, hh * 2), c);
    draw_rect(s, rect_make(cx + hw - bw, cy - hh, bw, hh * 2), c);
}

static void icon_step(Surface *s, int cx, int cy, int hw, int hh, int dir, uint32_t c)
{
    int bw = RP_MAX(2, hw / 3);
    if (dir > 0) {
        icon_play(s, cx - bw, cy, hw - bw, hh, +1, c);
        draw_rect(s, rect_make(cx + hw - bw, cy - hh, bw, hh * 2), c);
    } else {
        icon_play(s, cx + bw, cy, hw - bw, hh, -1, c);
        draw_rect(s, rect_make(cx - hw, cy - hh, bw, hh * 2), c);
    }
}

static void draw_button(Surface *s, const App *a, ButtonId id)
{
    Rect r = a->layout.buttons[id];
    int active = (id == BTN_PLAY_FWD  && a->play_dir > 0) ||
                 (id == BTN_PLAY_BACK && a->play_dir < 0);
    int hover  = a->hover_button == (int)id;
    int press  = a->press_button == (int)id;

    uint32_t bg = active ? COL_ACCENT : COL_BTN;
    if (press)      bg = draw_lerp_color(bg, COL_BLACK, 90);
    else if (hover) bg = draw_lerp_color(bg, COL_WHITE, 36);

    draw_rect(s, r, bg);
    draw_rect_outline(s, r, active ? draw_lerp_color(bg, COL_WHITE, 70) : COL_EDGE);

    uint32_t fg = active ? RP_RGB(0x0d, 0x12, 0x18) : COL_TEXT;
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    int hw = RP_MAX(3, r.w / 5);
    int hh = RP_MAX(3, r.h * 7 / 26);

    switch (id) {
    case BTN_PREV:      icon_step(s, cx, cy, hw, hh, -1, fg); break;
    case BTN_NEXT:      icon_step(s, cx, cy, hw, hh, +1, fg); break;
    case BTN_PLAY_BACK:
        if (active) icon_pause(s, cx, cy, hw, hh, fg);
        else        icon_play(s, cx, cy, hw, hh, -1, fg);
        break;
    case BTN_PLAY_FWD:
        if (active) icon_pause(s, cx, cy, hw, hh, fg);
        else        icon_play(s, cx, cy, hw, hh, +1, fg);
        break;
    default: break;
    }
}

/* ---- timeline ----------------------------------------------------------- */

static int index_of_number(const Sequence *seq, long v)
{
    int lo = 0, hi = seq->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        long m = seq->frames[mid].number;
        if (m == v) return mid;
        if (m < v) lo = mid + 1;
        else       hi = mid - 1;
    }
    return -1;
}

static long nice_step(double frames_per_px, int min_px)
{
    static const long cands[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500,
                                  1000, 2000, 2500, 5000, 10000, 25000, 50000,
                                  100000, 250000, 1000000 };
    double raw = frames_per_px * (double)min_px;
    if (raw < 1.0) raw = 1.0;
    for (int i = 0; i < RP_ARRAY_LEN(cands); i++)
        if ((double)cands[i] >= raw) return cands[i];
    return cands[RP_ARRAY_LEN(cands) - 1];
}

/* Paints one pixel column per screen column, mixing the residency of every
 * frame that falls under it so a long sequence still reads accurately when
 * there are more frames than pixels. */
static void draw_cache_strip(Surface *s, const App *a)
{
    const Layout *L = &a->layout;
    Rect tr = L->track;
    int count = a->seq->count;
    int span = RP_MAX(1, tr.w - 1);

    for (int i = 0; i < tr.w; i++) {
        int f0, f1;
        if (count <= 1) {
            f0 = f1 = 0;
        } else {
            double lo = ((double)i - 0.5) / span * (count - 1) + 0.5;
            double hi = ((double)i + 0.5) / span * (count - 1) + 0.5;
            f0 = RP_CLAMP((int)lo, 0, count - 1);
            f1 = RP_CLAMP((int)hi, 0, count - 1);
            if (f1 < f0) f1 = f0;
        }

        int total = f1 - f0 + 1, ready = 0, loading = 0, failed = 0;
        for (int f = f0; f <= f1; f++) {
            switch (cache_frame_state(a->cache, f)) {
            case CACHE_READY:   ready++;   break;
            case CACHE_LOADING: loading++; break;
            case CACHE_FAILED:  failed++;  break;
            default: break;
            }
        }

        uint32_t col;
        if (failed == total) {
            col = COL_FAILED;
        } else if (ready > 0) {
            col = draw_lerp_color(COL_TRACK, COL_CACHED, 255 * ready / total);
        } else if (loading > 0) {
            col = draw_lerp_color(COL_TRACK, COL_LOADING, 160);
        } else if (failed > 0) {
            col = draw_lerp_color(COL_TRACK, COL_FAILED, 160);
        } else {
            col = COL_TRACK;
        }
        draw_vline(s, tr.x + i, tr.y, tr.h, col);
    }
}

static void draw_ruler(Surface *s, const App *a)
{
    const Layout *L = &a->layout;
    const Sequence *seq = a->seq;
    int count = seq->count;
    Rect tl = L->timeline;

    int text_y = tl.y + 1 * S;
    int tick_y = tl.y + FONT_H * S + 3 * S;
    int tick_h = RP_MAX(1, L->track.y - tick_y);

    char buf[32];
    long n_first = seq->frames[0].number;
    long n_last  = seq->frames[count - 1].number;

    /* The two ends are always labelled, aligned inwards so they stay on
     * screen, and interior labels give way to them. */
    snprintf(buf, sizeof buf, "%ld", n_first);
    int first_w = font_text_width(buf, S);
    int first_x = L->track.x;
    draw_text(s, first_x, text_y, buf, COL_TEXT_DIM, S);
    int left_edge_right = first_x + first_w;

    snprintf(buf, sizeof buf, "%ld", n_last);
    int last_w = font_text_width(buf, S);
    int last_x = L->track.x + L->track.w - last_w;
    int right_edge_left = last_x;
    if (last_x > left_edge_right + 6 * S)
        draw_text(s, last_x, text_y, buf, COL_TEXT_DIM, S);
    else
        right_edge_left = s->w; /* no room; suppress and let interiors fill */

    draw_vline(s, L->track.x, tick_y, tick_h, COL_EDGE);
    draw_vline(s, L->track.x + L->track.w - 1, tick_y, tick_h, COL_EDGE);

    if (count <= 2) return;

    double frames_per_px = (double)(count - 1) / RP_MAX(1, L->track.w - 1);
    int min_px = font_text_width("000000", S) + 18 * S;
    long step = nice_step(frames_per_px, min_px);

    long start = ((n_first + step) / step) * step;
    int last_right = left_edge_right + 6 * S;

    for (long v = start; v <= n_last; v += step) {
        int idx = index_of_number(seq, v);
        if (idx <= 0 || idx >= count - 1) continue;

        snprintf(buf, sizeof buf, "%ld", v);
        int w = font_text_width(buf, S);
        int x = ui_x_for_frame(L, idx, count);
        int tx = x - w / 2;

        if (tx < last_right) continue;
        if (tx + w > right_edge_left - 6 * S) continue;

        draw_text(s, tx, text_y, buf, COL_TEXT_DIM, S);
        draw_vline(s, x, tick_y, tick_h, COL_EDGE);
        last_right = tx + w + 6 * S;
    }
}

static void draw_playhead(Surface *s, const App *a)
{
    const Layout *L = &a->layout;
    int count = a->seq->count;
    int x = ui_x_for_frame(L, a->current, count);
    Rect tr = L->track;

    int hw = RP_MAX(1, S);
    draw_rect(s, rect_make(x - hw / 2 - (hw % 2 ? 0 : 1), tr.y, hw + 1, tr.h), COL_PLAYHEAD);

    /* A handle above the track so the playhead stays findable when the track
     * itself is busy with cache colouring. */
    int tip = RP_MAX(3, 4 * S);
    draw_triangle(s, x - tip, tr.y - tip, x + tip, tr.y - tip, x, tr.y + 1, COL_PLAYHEAD);

    /* Current frame number, on a chip so it stays readable over the ruler. */
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", a->seq->frames[a->current].number);
    int w = font_text_width(buf, S);
    int cx = RP_CLAMP(x - w / 2 - 3 * S, L->track.x, L->track.x + L->track.w - w - 6 * S);
    Rect chip = rect_make(cx, L->timeline.y, w + 6 * S, FONT_H * S + 3 * S);
    draw_rect(s, chip, COL_PLAYHEAD);
    draw_text(s, cx + 3 * S, L->timeline.y + 1 * S, buf, RP_RGB(0x18, 0x14, 0x06), S);
}

/* ---- status bar --------------------------------------------------------- */

/* Copies `src` into `buf`, dropping characters from the front (and marking the
 * cut with "...") until it fits inside max_px. */
static void fit_text_tail(char *buf, size_t bufsz, const char *src, int max_px, int scale)
{
    size_t len = strlen(src);
    if (font_text_width(src, scale) <= max_px || bufsz < 8) {
        snprintf(buf, bufsz, "%s", src);
        return;
    }
    int adv = font_advance(scale);
    int fits = (max_px + scale) / (adv > 0 ? adv : 1) - 3;
    if (fits < 1) fits = 1;
    if ((size_t)fits > len) fits = (int)len;
    snprintf(buf, bufsz, "...%s", src + (len - (size_t)fits));
}

static void draw_statusbar(Surface *s, App *a)
{
    const Layout *L = &a->layout;
    Rect sb = L->statusbar;
    draw_rect(s, sb, COL_STATUS);
    draw_hline(s, sb.x, sb.y, sb.w, COL_EDGE);

    int ty = sb.y + (sb.h - FONT_H * S) / 2;
    int pad = PAD * S;

    CacheStats st;
    cache_get_stats(a->cache, &st);

    char used[32], limit[32];
    rp_human_bytes(used, sizeof used, st.bytes_used);
    rp_human_bytes(limit, sizeof limit, st.bytes_limit);

    char right[256];
    if (a->src_w > 0)
        snprintf(right, sizeof right,
                 "%dx%d   %d/%d   RAM %s / %s (%d frames)   %.4g fps%s",
                 a->src_w, a->src_h, a->current + 1, a->seq->count,
                 used, limit, st.ready, a->fps,
                 st.failed ? "   ERRORS" : "");
    else
        snprintf(right, sizeof right, "%d/%d   RAM %s / %s",
                 a->current + 1, a->seq->count, used, limit);

    int rw = font_text_width(right, S);
    int rx = sb.x + sb.w - pad - rw;
    draw_text(s, rx, ty, right, st.failed ? COL_FAILED : COL_TEXT_DIM, S);

    char name[256];
    fit_text_tail(name, sizeof name, a->seq->display, RP_MAX(0, rx - pad * 2 - sb.x), S);
    draw_text(s, sb.x + pad, ty, name, COL_TEXT, S);
}

/* ---- overlays ----------------------------------------------------------- */

static void draw_badge(Surface *s, Rect area, const char *text, uint32_t bg, uint32_t fg)
{
    int pad = 6 * S;
    int w = font_text_width(text, S) + pad * 2;
    int h = FONT_H * S + pad;
    Rect r = rect_make(area.x + 10 * S, area.y + 10 * S, w, h);
    draw_rect_blend(s, r, bg, 210);
    draw_text(s, r.x + pad, r.y + pad / 2, text, fg, S);
}

static void draw_help(Surface *s, const App *a)
{
    static const char *const lines[] = {
        "SPACE     play / pause forwards",
        "B         play / pause backwards",
        "LEFT      previous frame",
        "RIGHT     next frame",
        "PGUP/DOWN jump 10 frames",
        "HOME/END  first / last frame",
        "F         fit image to window",
        "1         actual size (1:1)",
        "S         toggle smooth scaling",
        "?         this help",
        "Q / ESC   quit",
        "",
        "Drag on the timeline to scrub.",
    };

    int n = RP_ARRAY_LEN(lines);
    int pad = 14 * S;
    int lh = (FONT_H + 4) * S;
    int w = 0;
    for (int i = 0; i < n; i++) w = RP_MAX(w, font_text_width(lines[i], S));

    Rect box = rect_center(a->layout.viewport, w + pad * 2, n * lh + pad * 2);
    draw_rect_blend(s, box, RP_RGB(0x10, 0x12, 0x16), 235);
    draw_rect_outline(s, box, COL_EDGE);
    for (int i = 0; i < n; i++)
        draw_text(s, box.x + pad, box.y + pad + i * lh, lines[i],
                  lines[i][0] ? COL_TEXT : COL_TEXT_DIM, S);
}

/* ---- entry point -------------------------------------------------------- */

void ui_draw(Surface *s, App *a)
{
    const Layout *L = &a->layout;

    draw_rect(s, L->viewport, COL_VIEWPORT);
    if (a->shown) {
        Rect dst = a->fit ? rect_fit(L->viewport, a->shown->width, a->shown->height)
                          : rect_center(L->viewport, a->shown->width, a->shown->height);
        draw_image(s, L->viewport, dst, a->shown, a->filter);
    }

    int state = cache_frame_state(a->cache, a->current);
    if (state == CACHE_FAILED) {
        /* The picture underneath is a stand-in too, so say which. */
        char msg[256];
        if (a->shown && a->shown_frame != a->current)
            snprintf(msg, sizeof msg, "FRAME %ld FAILED TO LOAD, SHOWING %ld",
                     a->seq->frames[a->current].number,
                     a->seq->frames[a->shown_frame].number);
        else
            snprintf(msg, sizeof msg, "FRAME %ld FAILED TO LOAD",
                     a->seq->frames[a->current].number);
        draw_badge(s, L->viewport, msg, COL_FAILED, COL_WHITE);
    } else if (a->shown_frame != a->current || !a->shown) {
        /* The image on screen is a stand-in: the nearest frame that is in
         * RAM. Say which, so a scrub into unloaded frames reads as tracking
         * rather than stuck. */
        char msg[96];
        if (a->shown)
            snprintf(msg, sizeof msg, "LOADING %ld, SHOWING %ld",
                     a->seq->frames[a->current].number,
                     a->seq->frames[a->shown_frame].number);
        else
            snprintf(msg, sizeof msg, "LOADING FIRST FRAME");
        draw_badge(s, L->viewport, msg, RP_RGB(0x20, 0x24, 0x2c), COL_TEXT);
    }

    if (a->show_help) draw_help(s, a);

    draw_statusbar(s, a);

    draw_rect(s, L->transport, COL_PANEL);
    draw_hline(s, L->transport.x, L->transport.y, L->transport.w, COL_EDGE);
    for (int i = 0; i < BTN_COUNT; i++) draw_button(s, a, (ButtonId)i);

    draw_rect(s, L->track, COL_TRACK);
    draw_cache_strip(s, a);
    draw_rect_outline(s, L->track, COL_EDGE);
    draw_ruler(s, a);
    draw_playhead(s, a);
}
