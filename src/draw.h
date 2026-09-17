/* draw.h - CPU rasteriser.
 *
 * Everything the user sees is composited into a single ARGB8888 buffer in
 * system memory; the platform layer only has to hand that one buffer to the
 * display. No drawing here touches a GPU.
 *
 * These routines are not thread safe: they share scratch buffers and are only
 * ever called from the main thread.
 */
#ifndef RP_DRAW_H
#define RP_DRAW_H

#include <stdint.h>

#include "image.h"

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    uint32_t *px;
    int       w, h;
} Surface;

#define RP_RGB(r, g, b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static inline Rect rect_make(int x, int y, int w, int h)
{
    Rect r = { x, y, w, h };
    return r;
}

static inline int rect_contains(Rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

Rect rect_intersect(Rect a, Rect b);

/* Largest rect of aspect iw:ih that fits inside `box`, centred. */
Rect rect_fit(Rect box, int iw, int ih);

/* Rect of exactly iw x ih centred in `box` (may overflow it; callers clip). */
Rect rect_center(Rect box, int iw, int ih);

void draw_clear(Surface *s, uint32_t col);
void draw_rect(Surface *s, Rect r, uint32_t col);
void draw_rect_outline(Surface *s, Rect r, uint32_t col);
/* An outline `thickness` pixels wide straddling the edge of `r`: half the
 * band lies outside the rect and half inside (the inside gets the odd pixel),
 * so a two-pixel frame marks the boundary without hiding what it encloses.
 * Clipped to `clip`. */
void draw_frame(Surface *s, Rect clip, Rect r, int thickness, uint32_t col);
void draw_hline(Surface *s, int x, int y, int w, uint32_t col);
void draw_vline(Surface *s, int x, int y, int h, uint32_t col);

/* Blends `col` over the destination with the given alpha (0-255). */
void draw_rect_blend(Surface *s, Rect r, uint32_t col, int alpha);

/* Mixes two packed colours; t runs 0 (all a) to 255 (all b). */
uint32_t draw_lerp_color(uint32_t a, uint32_t b, int t);

void draw_text(Surface *s, int x, int y, const char *str, uint32_t col, int scale);
/* Same, but with a one-pixel drop shadow so text stays legible over imagery. */
void draw_text_shadow(Surface *s, int x, int y, const char *str, uint32_t col, int scale);

/* Filled triangle; vertices in any winding order. */
void draw_triangle(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col);

typedef enum { DRAW_NEAREST = 0, DRAW_BILINEAR = 1 } DrawFilter;

/* Draws `im` scaled into `dst`, clipped to `clip`. */
void draw_image(Surface *s, Rect clip, Rect dst, const Image *im, DrawFilter filter);

void draw_shutdown(void);

#endif /* RP_DRAW_H */
