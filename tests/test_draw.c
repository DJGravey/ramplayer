/* test_draw.c - the CPU rasteriser.
 *
 * The image blit is the one piece of drawing where a mistake corrupts what the
 * user sees rather than merely looking untidy, so it is checked for exactness
 * where exactness is required (1:1 copies, flat colours), for staying inside
 * its clip, and for not reading outside the source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"
#include "font.h"
#include "util.h"

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
               printf(__VA_ARGS__); printf("\n"); }                           \
    } while (0)

#define SENTINEL 0xff123456u

static Surface make_surface(int w, int h)
{
    Surface s = { malloc((size_t)w * h * sizeof(uint32_t)), w, h };
    for (int i = 0; i < w * h; i++) s.px[i] = SENTINEL;
    return s;
}

static Image *make_gradient(int w, int h)
{
    Image *im = image_new(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t r = (uint32_t)(x * 255 / (w > 1 ? w - 1 : 1));
            uint32_t g = (uint32_t)(y * 255 / (h > 1 ? h - 1 : 1));
            im->px[(size_t)y * w + x] = 0xff000000u | (r << 16) | (g << 8) | 0x40u;
        }
    return im;
}

static Image *make_flat(int w, int h, uint32_t col)
{
    Image *im = image_new(w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) im->px[i] = col;
    return im;
}

/* How many pixels outside `keep` were written. */
static int touched_outside(const Surface *s, Rect keep)
{
    int n = 0;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (!rect_contains(keep, x, y) && s->px[(size_t)y * s->w + x] != SENTINEL) n++;
    return n;
}

static void test_one_to_one(void)
{
    printf("1:1 blits\n");
    Surface s = make_surface(100, 80);
    Image *im = make_gradient(40, 30);
    Rect clip = rect_make(0, 0, 100, 80);
    Rect dst = rect_make(10, 5, 40, 30);

    draw_image(&s, clip, dst, im, DRAW_NEAREST);
    int exact = 1;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
            if (s.px[(size_t)(5 + y) * 100 + 10 + x] != im->px[(size_t)y * 40 + x]) exact = 0;
    CHECK(exact, "a 1:1 blit must reproduce the source exactly");
    CHECK(touched_outside(&s, dst) == 0, "a 1:1 blit must not write outside its rect");

    image_unref(im);
    free(s.px);
}

static void test_clipping(void)
{
    printf("clipping\n");
    Surface s = make_surface(100, 80);
    Image *im = make_gradient(64, 64);

    /* A destination that hangs off every edge, with a clip well inside. */
    Rect clip = rect_make(20, 15, 40, 30);
    Rect dst  = rect_make(-50, -40, 300, 250);

    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < s.w * s.h; i++) s.px[i] = SENTINEL;
        draw_image(&s, clip, dst, im, f ? DRAW_BILINEAR : DRAW_NEAREST);
        CHECK(touched_outside(&s, clip) == 0,
              "%s must stay inside the clip", f ? "bilinear" : "nearest");
        int filled = 1;
        for (int y = clip.y; y < clip.y + clip.h; y++)
            for (int x = clip.x; x < clip.x + clip.w; x++)
                if (s.px[(size_t)y * s.w + x] == SENTINEL) filled = 0;
        CHECK(filled, "%s must fill the whole clip", f ? "bilinear" : "nearest");
    }

    /* Entirely off-surface destinations must be harmless. */
    for (int i = 0; i < s.w * s.h; i++) s.px[i] = SENTINEL;
    draw_image(&s, rect_make(0, 0, 100, 80), rect_make(-500, -500, 100, 100), im, DRAW_BILINEAR);
    draw_image(&s, rect_make(0, 0, 100, 80), rect_make(9000, 9000, 100, 100), im, DRAW_BILINEAR);
    draw_image(&s, rect_make(0, 0, 100, 80), rect_make(0, 0, 0, 0), im, DRAW_BILINEAR);
    CHECK(touched_outside(&s, rect_make(0, 0, 0, 0)) == 0,
          "destinations outside the surface must draw nothing");

    image_unref(im);
    free(s.px);
}

/* Interpolating between equal values must return that value, at every scale.
 * This is the check that catches rounding and channel-packing mistakes in the
 * lerp, which a gradient would hide. */
static void test_flat_colour_is_preserved(void)
{
    printf("flat colours survive scaling\n");
    static const uint32_t cols[] = {
        0xff000000u, 0xffffffffu, 0xff7f7f7fu, 0xff010203u, 0xfffe0102u, 0xff00ff80u,
    };
    static const int sizes[][2] = { { 7, 5 }, { 64, 64 }, { 200, 13 } };
    static const int dsts[][2]  = { { 1, 1 }, { 3, 2 }, { 63, 64 }, { 200, 150 }, { 511, 97 } };

    Surface s = make_surface(600, 300);
    Rect clip = rect_make(0, 0, 600, 300);
    int bad = 0;

    for (int c = 0; c < RP_ARRAY_LEN(cols) && bad < 5; c++) {
        for (int z = 0; z < RP_ARRAY_LEN(sizes) && bad < 5; z++) {
            Image *im = make_flat(sizes[z][0], sizes[z][1], cols[c]);
            for (int d = 0; d < RP_ARRAY_LEN(dsts) && bad < 5; d++) {
                Rect dst = rect_make(2, 3, dsts[d][0], dsts[d][1]);
                for (int i = 0; i < s.w * s.h; i++) s.px[i] = SENTINEL;
                draw_image(&s, clip, dst, im, DRAW_BILINEAR);
                for (int y = dst.y; y < dst.y + dst.h && bad < 5; y++)
                    for (int x = dst.x; x < dst.x + dst.w && bad < 5; x++)
                        if (s.px[(size_t)y * s.w + x] != cols[c]) {
                            printf("  %08x scaled %dx%d -> %dx%d gave %08x at (%d,%d)\n",
                                   cols[c], im->width, im->height, dst.w, dst.h,
                                   s.px[(size_t)y * s.w + x], x - dst.x, y - dst.y);
                            bad++;
                        }
            }
            image_unref(im);
        }
    }
    CHECK(bad == 0, "bilinear scaling of a flat colour must return that colour exactly");
    free(s.px);
}

static void test_monotonic_gradient(void)
{
    printf("gradients stay ordered\n");
    Surface s = make_surface(400, 200);
    Rect clip = rect_make(0, 0, 400, 200);
    Image *im = make_gradient(37, 23); /* awkward size, not a clean ratio */
    Rect dst = rect_make(0, 0, 400, 200);

    draw_image(&s, clip, dst, im, DRAW_BILINEAR);

    int rows_ok = 1, cols_ok = 1, alpha_ok = 1;
    for (int y = 0; y < 200; y++) {
        int prev = -1;
        for (int x = 0; x < 400; x++) {
            uint32_t p = s.px[(size_t)y * 400 + x];
            int r = (int)((p >> 16) & 0xff);
            if (r < prev) rows_ok = 0;
            if (((p >> 24) & 0xff) != 0xff) alpha_ok = 0;
            prev = r;
        }
    }
    for (int x = 0; x < 400; x++) {
        int prev = -1;
        for (int y = 0; y < 200; y++) {
            int g = (int)((s.px[(size_t)y * 400 + x] >> 8) & 0xff);
            if (g < prev) cols_ok = 0;
            prev = g;
        }
    }
    CHECK(rows_ok, "a horizontal ramp must not go backwards after scaling");
    CHECK(cols_ok, "a vertical ramp must not go backwards after scaling");
    CHECK(alpha_ok, "scaled output must be opaque");

    /* Corners must still carry the source's corner values. */
    CHECK(((s.px[0] >> 16) & 0xff) <= 4, "top-left keeps the low end of the ramp");
    CHECK(((s.px[399] >> 16) & 0xff) >= 251, "top-right keeps the high end of the ramp");

    image_unref(im);
    free(s.px);
}

static void test_primitives(void)
{
    printf("rects, text and triangles\n");
    Surface s = make_surface(120, 60);

    draw_rect(&s, rect_make(10, 10, 20, 15), 0xffaabbccu);
    CHECK(s.px[10 * 120 + 10] == 0xffaabbccu, "rect fills its top-left corner");
    CHECK(s.px[24 * 120 + 29] == 0xffaabbccu, "rect fills its bottom-right corner");
    CHECK(s.px[25 * 120 + 30] == SENTINEL, "rect stops at its edges");
    CHECK(touched_outside(&s, rect_make(10, 10, 20, 15)) == 0, "rect writes nothing else");

    /* Rects that hang off the surface must clip rather than corrupt memory. */
    draw_rect(&s, rect_make(-40, -40, 500, 500), 0xff00ff00u);
    draw_rect(&s, rect_make(1000, 1000, 50, 50), 0xff0000ffu);
    draw_rect(&s, rect_make(0, 0, -5, -5), 0xff0000ffu);
    int all_green = 1;
    for (int i = 0; i < 120 * 60; i++) if (s.px[i] != 0xff00ff00u) all_green = 0;
    CHECK(all_green, "an oversized rect fills exactly the surface");

    /* Blending at the extremes must be exact. */
    draw_rect_blend(&s, rect_make(0, 0, 10, 10), 0xffff0000u, 255);
    CHECK(s.px[0] == 0xffff0000u, "alpha 255 replaces the destination (got %08x)", s.px[0]);
    draw_rect_blend(&s, rect_make(0, 0, 10, 10), 0xff0000ffu, 0);
    CHECK(s.px[0] == 0xffff0000u, "alpha 0 leaves the destination alone (got %08x)", s.px[0]);

    CHECK(draw_lerp_color(0xff204060u, 0xff204060u, 128) == 0xff204060u,
          "lerping a colour with itself is a no-op");
    CHECK(draw_lerp_color(0xff000000u, 0xffffffffu, 0) == 0xff000000u, "lerp at 0 picks a");
    CHECK(draw_lerp_color(0xff000000u, 0xffffffffu, 255) == 0xffffffffu, "lerp at 255 picks b");

    /* Text and triangles must stay inside the surface. */
    font_init();
    draw_text(&s, -50, -50, "clipped text", 0xffffffffu, 3);
    draw_text(&s, 110, 55, "clipped text", 0xffffffffu, 3);
    draw_triangle(&s, -100, -100, 400, 30, 30, 400, 0xff888888u);
    draw_triangle(&s, 0, 0, 0, 0, 0, 0, 0xff888888u); /* degenerate */
    CHECK(1, "text and triangles clipped without crashing");

    CHECK(font_text_width("A", 1) == 5, "one glyph is 5 px wide (got %d)", font_text_width("A", 1));
    CHECK(font_text_width("AB", 1) == 11, "two glyphs are 11 px with the gap (got %d)",
          font_text_width("AB", 1));
    CHECK(font_text_width("", 1) == 0, "empty text has no width");

    free(s.px);
}

static void test_fit(void)
{
    printf("fitting to the window\n");
    Rect box = rect_make(0, 0, 800, 600);

    Rect r = rect_fit(box, 1920, 1080);
    CHECK(r.w == 800, "a wide image fills the width (got %d)", r.w);
    CHECK(r.h == 450, "and keeps its aspect (got %d)", r.h);
    CHECK(r.y == 75, "and is centred vertically (got %d)", r.y);

    r = rect_fit(box, 1080, 1920);
    CHECK(r.h == 600, "a tall image fills the height (got %d)", r.h);
    CHECK(r.w == 338, "and keeps its aspect (got %d)", r.w);

    r = rect_fit(box, 800, 600);
    CHECK(r.w == 800 && r.h == 600 && r.x == 0 && r.y == 0, "an exact fit is 1:1");

    /* Degenerate inputs must not produce a nonsense rect. */
    r = rect_fit(rect_make(0, 0, 0, 0), 100, 100);
    CHECK(r.w == 0 || r.h == 0, "an empty box fits nothing");
    r = rect_fit(box, 0, 0);
    CHECK(r.w == 0 || r.h == 0, "an empty image fits nothing");

    Rect c = rect_center(box, 100, 50);
    CHECK(c.x == 350 && c.y == 275 && c.w == 100 && c.h == 50, "centring puts it in the middle");
}

/* Counts pixels on the surface that hold `col`. */
static int count_colour(const Surface *s, uint32_t col)
{
    int n = 0;
    for (int i = 0; i < s->w * s->h; i++) if (s->px[i] == col) n++;
    return n;
}

/* A two-pixel frame straddles the boundary of its rect: one pixel in, one
 * out. The interior is untouched and the bands stop at the clip. */
static void test_frame(void)
{
    printf("frame outline\n");
    const uint32_t col = 0xff11ee33u;
    Surface s = make_surface(40, 40);
    Rect r = rect_make(10, 10, 20, 20);

    draw_frame(&s, rect_make(0, 0, 40, 40), r, 2, col);
    /* The band runs from 9 to 30 inclusive on each axis: the outer pixel is
     * outside the rect, the inner one inside. */
    CHECK(s.px[9 * 40 + 9] == col,   "the outer corner pixel, one outside the rect, is painted");
    CHECK(s.px[10 * 40 + 10] == col, "the rect's own corner pixel is painted");
    CHECK(s.px[11 * 40 + 11] == SENTINEL, "the pixel two inside the corner is not");
    CHECK(s.px[8 * 40 + 8] == SENTINEL, "the pixel two outside the corner is not");
    CHECK(s.px[30 * 40 + 30] == col, "the far corner is one outside the rect (29 is its last pixel)");
    CHECK(s.px[29 * 40 + 29] == col, "and the rect's own far corner pixel is painted");
    CHECK(s.px[28 * 40 + 28] == SENTINEL, "two inside the far corner is not");
    CHECK(s.px[20 * 40 + 20] == SENTINEL, "the middle is untouched");
    CHECK(count_colour(&s, col) == 22 * 22 - 18 * 18,
          "exactly the band is painted (%d pixels, want %d)", count_colour(&s, col), 22 * 22 - 18 * 18);
    CHECK(touched_outside(&s, rect_make(9, 9, 22, 22)) == 0, "nothing outside the band is written");

    /* Clipped: only what lies inside the clip is painted. */
    Surface c = make_surface(40, 40);
    draw_frame(&c, rect_make(0, 0, 20, 20), r, 2, col);
    CHECK(touched_outside(&c, rect_make(0, 0, 20, 20)) == 0, "the frame stays inside its clip");
    CHECK(c.px[9 * 40 + 9] == col && c.px[19 * 40 + 9] == col, "the part inside the clip is painted");
    CHECK(c.px[9 * 40 + 20] == SENTINEL, "the part outside the clip is not");

    /* A thickness of 1 is the plain inside outline. */
    Surface o = make_surface(40, 40);
    draw_frame(&o, rect_make(0, 0, 40, 40), r, 1, col);
    CHECK(o.px[10 * 40 + 10] == col && o.px[9 * 40 + 9] == SENTINEL && o.px[11 * 40 + 11] == SENTINEL,
          "a one-pixel frame lies on the rect's own edge");
    CHECK(count_colour(&o, col) == 20 * 20 - 18 * 18, "a one-pixel frame paints just the edge");

    /* Nothing to draw: no crash, no pixels. */
    Surface e = make_surface(40, 40);
    draw_frame(&e, rect_make(0, 0, 40, 40), rect_make(5, 5, 0, 10), 2, col);
    draw_frame(&e, rect_make(0, 0, 40, 40), r, 0, col);
    draw_frame(&e, rect_make(0, 0, 40, 40), rect_make(-100, -100, 20, 20), 2, col);
    CHECK(count_colour(&e, col) == 0, "empty rects, zero thickness and off-surface frames paint nothing");

    free(s.px);
    free(c.px);
    free(o.px);
    free(e.px);
}

int main(void)
{
    test_one_to_one();
    test_clipping();
    test_flat_colour_is_preserved();
    test_monotonic_gradient();
    test_primitives();
    test_fit();
    test_frame();

    draw_shutdown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
