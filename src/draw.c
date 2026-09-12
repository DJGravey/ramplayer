#include "draw.h"

#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "util.h"

Rect rect_intersect(Rect a, Rect b)
{
    int x0 = RP_MAX(a.x, b.x);
    int y0 = RP_MAX(a.y, b.y);
    int x1 = RP_MIN(a.x + a.w, b.x + b.w);
    int y1 = RP_MIN(a.y + a.h, b.y + b.h);
    Rect r = { x0, y0, RP_MAX(0, x1 - x0), RP_MAX(0, y1 - y0) };
    return r;
}

Rect rect_fit(Rect box, int iw, int ih)
{
    if (iw <= 0 || ih <= 0 || box.w <= 0 || box.h <= 0)
        return rect_make(box.x, box.y, 0, 0);

    /* Pick whichever axis runs out first, in integer terms to avoid an
     * off-by-one that would leave a stray row of background. */
    int w = box.w;
    int h = (int)(((int64_t)w * ih + iw / 2) / iw);
    if (h > box.h) {
        h = box.h;
        w = (int)(((int64_t)h * iw + ih / 2) / ih);
    }
    w = RP_MAX(w, 1);
    h = RP_MAX(h, 1);
    return rect_center(box, w, h);
}

Rect rect_center(Rect box, int iw, int ih)
{
    return rect_make(box.x + (box.w - iw) / 2, box.y + (box.h - ih) / 2, iw, ih);
}

void draw_clear(Surface *s, uint32_t col)
{
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++) s->px[i] = col;
}

void draw_rect(Surface *s, Rect r, uint32_t col)
{
    Rect c = rect_intersect(r, rect_make(0, 0, s->w, s->h));
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = s->px + (size_t)y * s->w + c.x;
        for (int x = 0; x < c.w; x++) row[x] = col;
    }
}

void draw_hline(Surface *s, int x, int y, int w, uint32_t col)
{
    draw_rect(s, rect_make(x, y, w, 1), col);
}

void draw_vline(Surface *s, int x, int y, int h, uint32_t col)
{
    draw_rect(s, rect_make(x, y, 1, h), col);
}

void draw_rect_outline(Surface *s, Rect r, uint32_t col)
{
    if (r.w <= 0 || r.h <= 0) return;
    draw_hline(s, r.x, r.y, r.w, col);
    draw_hline(s, r.x, r.y + r.h - 1, r.w, col);
    draw_vline(s, r.x, r.y, r.h, col);
    draw_vline(s, r.x + r.w - 1, r.y, r.h, col);
}

/* Lerps two packed ARGB values. t runs 0..256, where 256 selects `b`. The two
 * channel pairs are kept in separate 16-bit lanes so neither can carry into
 * the other. */
static inline uint32_t lerp_argb(uint32_t a, uint32_t b, uint32_t t)
{
    uint32_t it = 256u - t;
    uint32_t rb = ((((a & 0x00FF00FFu) * it) + ((b & 0x00FF00FFu) * t)) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * it) + (((b >> 8) & 0x00FF00FFu) * t)) & 0xFF00FF00u;
    return rb | ag;
}

uint32_t draw_lerp_color(uint32_t a, uint32_t b, int t)
{
    t = RP_CLAMP(t, 0, 255);
    return lerp_argb(a, b, (uint32_t)t + 1) | 0xff000000u;
}

void draw_rect_blend(Surface *s, Rect r, uint32_t col, int alpha)
{
    alpha = RP_CLAMP(alpha, 0, 255);
    if (alpha == 0) return;
    if (alpha >= 255) { draw_rect(s, r, col); return; }

    uint32_t t = (uint32_t)alpha + 1; /* map 255 -> 256 so full alpha is exact */
    Rect c = rect_intersect(r, rect_make(0, 0, s->w, s->h));
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *row = s->px + (size_t)y * s->w + c.x;
        for (int x = 0; x < c.w; x++)
            row[x] = lerp_argb(row[x], col, t) | 0xff000000u;
    }
}

void draw_text(Surface *s, int x, int y, const char *str, uint32_t col, int scale)
{
    if (scale < 1) scale = 1;
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        const uint8_t *glyph = font_glyph(*p);
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col_i = 0; col_i < FONT_W; col_i++) {
                if (!(bits & (1u << (FONT_W - 1 - col_i)))) continue;
                draw_rect(s, rect_make(pen + col_i * scale, y + row * scale, scale, scale), col);
            }
        }
        pen += font_advance(scale);
    }
}

void draw_text_shadow(Surface *s, int x, int y, const char *str, uint32_t col, int scale)
{
    draw_text(s, x + scale, y + scale, str, RP_RGB(0, 0, 0), scale);
    draw_text(s, x, y, str, col, scale);
}

void draw_triangle(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col)
{
    int minx = RP_MAX(0, RP_MIN(x0, RP_MIN(x1, x2)));
    int maxx = RP_MIN(s->w - 1, RP_MAX(x0, RP_MAX(x1, x2)));
    int miny = RP_MAX(0, RP_MIN(y0, RP_MIN(y1, y2)));
    int maxy = RP_MIN(s->h - 1, RP_MAX(y0, RP_MAX(y1, y2)));
    if (minx > maxx || miny > maxy) return;

    /* Edge functions, with the sign normalised so that "inside" is always
     * non-negative regardless of the winding the caller passed in. */
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
    if (area == 0) return;
    int flip = area < 0 ? -1 : 1;

    for (int y = miny; y <= maxy; y++) {
        uint32_t *row = s->px + (size_t)y * s->w;
        for (int x = minx; x <= maxx; x++) {
            long w0 = ((long)(x1 - x0) * (y - y0) - (long)(y1 - y0) * (x - x0)) * flip;
            long w1 = ((long)(x2 - x1) * (y - y1) - (long)(y2 - y1) * (x - x1)) * flip;
            long w2 = ((long)(x0 - x2) * (y - y2) - (long)(y0 - y2) * (x - x2)) * flip;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) row[x] = col;
        }
    }
}

/* ---- scaled image blit ---------------------------------------------------
 *
 * Source coordinates are stepped in 16.16 fixed point. The per-column source
 * indices and weights are the same for every row, so they are computed once
 * into scratch arrays that persist between calls (main thread only).
 */

static int      *g_col_x0;
static int      *g_col_x1;
static uint32_t *g_col_t;
static int       g_col_cap;
static uint32_t *g_row;     /* one source row, blended between two scanlines */
static int       g_row_cap;

static int ensure_col_scratch(int n)
{
    if (n <= g_col_cap) return 1;
    int cap = g_col_cap ? g_col_cap : 256;
    while (cap < n) cap *= 2;

    int      *x0 = realloc(g_col_x0, (size_t)cap * sizeof *x0);
    int      *x1 = realloc(g_col_x1, (size_t)cap * sizeof *x1);
    uint32_t *t  = realloc(g_col_t,  (size_t)cap * sizeof *t);
    if (!x0 || !x1 || !t) {
        /* Keep whatever did get reallocated; capacity stays at the old value
         * for any pointer that failed, so report failure and draw nothing. */
        if (x0) g_col_x0 = x0;
        if (x1) g_col_x1 = x1;
        if (t)  g_col_t  = t;
        return 0;
    }
    g_col_x0 = x0;
    g_col_x1 = x1;
    g_col_t  = t;
    g_col_cap = cap;
    return 1;
}

static int ensure_row_scratch(int n)
{
    if (n <= g_row_cap) return 1;
    uint32_t *p = realloc(g_row, (size_t)n * sizeof *p);
    if (!p) return 0;
    g_row = p;
    g_row_cap = n;
    return 1;
}

void draw_shutdown(void)
{
    free(g_col_x0); g_col_x0 = NULL;
    free(g_col_x1); g_col_x1 = NULL;
    free(g_col_t);  g_col_t  = NULL;
    free(g_row);    g_row    = NULL;
    g_col_cap = 0;
    g_row_cap = 0;
}

void draw_image(Surface *s, Rect clip, Rect dst, const Image *im, DrawFilter filter)
{
    if (!im || !im->px || dst.w <= 0 || dst.h <= 0) return;

    Rect vis = rect_intersect(rect_intersect(dst, clip), rect_make(0, 0, s->w, s->h));
    if (vis.w <= 0 || vis.h <= 0) return;

    /* 1:1 and unclipped in x: straight row copies. */
    if (dst.w == im->width && dst.h == im->height) {
        for (int y = 0; y < vis.h; y++) {
            int sy = vis.y + y - dst.y;
            memcpy(s->px + (size_t)(vis.y + y) * s->w + vis.x,
                   im->px + (size_t)sy * im->width + (vis.x - dst.x),
                   (size_t)vis.w * sizeof(uint32_t));
        }
        return;
    }

    if (!ensure_col_scratch(vis.w)) return;

    int64_t xstep = ((int64_t)im->width  << 16) / dst.w;
    int64_t ystep = ((int64_t)im->height << 16) / dst.h;
    int64_t xbase = xstep / 2 - 32768 + (int64_t)(vis.x - dst.x) * xstep;
    int64_t ybase = ystep / 2 - 32768 + (int64_t)(vis.y - dst.y) * ystep;

    const int maxx = im->width - 1;
    const int maxy = im->height - 1;

    /* Bilinear runs as two passes rather than one. Blending the two source
     * rows into a scratch line costs a lerp per source pixel but leaves only
     * one lerp per output pixel instead of three, and that vertical pass is a
     * straight walk over both rows, so the compiler can vectorise it. */
    if (filter == DRAW_BILINEAR && ensure_row_scratch(im->width)) {
        for (int i = 0; i < vis.w; i++) {
            int64_t u = xbase + (int64_t)i * xstep;
            int x0 = (int)(u >> 16);
            g_col_x0[i] = RP_CLAMP(x0, 0, maxx);
            g_col_x1[i] = RP_CLAMP(x0 + 1, 0, maxx);
            g_col_t[i]  = (uint32_t)(u & 0xffff) >> 8;
        }

        /* The index tables are monotonic, so the source columns the visible
         * output samples run from the first entry to the last. */
        int sx_lo = g_col_x0[0];
        int sx_hi = g_col_x1[vis.w - 1];

        for (int j = 0; j < vis.h; j++) {
            int64_t v = ybase + (int64_t)j * ystep;
            int y0 = (int)(v >> 16);
            uint32_t ft = (uint32_t)(v & 0xffff) >> 8;
            int y1 = RP_CLAMP(y0 + 1, 0, maxy);
            y0 = RP_CLAMP(y0, 0, maxy);

            /* When the output row lands on a source row there is nothing to
             * blend, and the source can be sampled where it already lies. */
            const uint32_t *r0 = im->px + (size_t)y0 * im->width;
            const uint32_t *row = r0;
            if (ft != 0 && y1 != y0) {
                const uint32_t *r1 = im->px + (size_t)y1 * im->width;
                for (int x = sx_lo; x <= sx_hi; x++)
                    g_row[x] = lerp_argb(r0[x], r1[x], ft);
                row = g_row;
            }

            uint32_t *out = s->px + (size_t)(vis.y + j) * s->w + vis.x;
            for (int i = 0; i < vis.w; i++)
                out[i] = lerp_argb(row[g_col_x0[i]], row[g_col_x1[i]], g_col_t[i]) | 0xff000000u;
        }
    } else {
        for (int i = 0; i < vis.w; i++) {
            int64_t u = xbase + (int64_t)i * xstep + 32768; /* round to nearest */
            g_col_x0[i] = RP_CLAMP((int)(u >> 16), 0, maxx);
        }
        for (int j = 0; j < vis.h; j++) {
            int64_t v = ybase + (int64_t)j * ystep + 32768;
            int sy = RP_CLAMP((int)(v >> 16), 0, maxy);
            const uint32_t *src = im->px + (size_t)sy * im->width;
            uint32_t *out = s->px + (size_t)(vis.y + j) * s->w + vis.x;
            for (int i = 0; i < vis.w; i++) out[i] = src[g_col_x0[i]];
        }
    }
}
