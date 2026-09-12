/* test_reader.c - EXR decoding and sequence discovery against awkward inputs.
 *
 * The fixtures under test/edge are written by tools/mkexr.c, whose pattern
 * encodes each pixel's position in the display window:
 *     R = x / (W-1)   G = y / (H-1)   B = 0.25
 * so a decoder that mislays the data window offset fails a position check
 * rather than merely looking wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "color.h"
#include "reader.h"
#include "sequence.h"
#include "util.h"

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
               printf(__VA_ARGS__); printf("\n"); }                           \
    } while (0)

static ColorLUT g_lut;
static DecodeScratch *g_scratch;

static Image *load(const char *path, char *err, size_t errsz)
{
    return reader_load(path, &g_lut, g_scratch, NULL, err, errsz);
}

static int px_r(const Image *im, int x, int y) { return (im->px[(size_t)y * im->width + x] >> 16) & 0xff; }
static int px_g(const Image *im, int x, int y) { return (im->px[(size_t)y * im->width + x] >> 8) & 0xff; }
static int px_b(const Image *im, int x, int y) { return im->px[(size_t)y * im->width + x] & 0xff; }
static int px_a(const Image *im, int x, int y) { return (im->px[(size_t)y * im->width + x] >> 24) & 0xff; }

/* Half floats only carry ~11 bits, so allow a code value or two of slack. */
static int near(int a, int b) { return abs(a - b) <= 2; }

static int expect_at(int coord, int extent)
{
    return color_linear_to_srgb8((extent > 1) ? (float)coord / (float)(extent - 1) : 0.0f);
}

/* Verifies the positional pattern over the whole image. */
static int check_pattern(const Image *im, const char *what)
{
    int bad = 0;
    for (int y = 0; y < im->height && bad < 4; y++) {
        for (int x = 0; x < im->width && bad < 4; x++) {
            if (!near(px_r(im, x, y), expect_at(x, im->width)) ||
                !near(px_g(im, x, y), expect_at(y, im->height))) {
                printf("  %s: pixel (%d,%d) = R%d G%d, wanted R%d G%d\n", what, x, y,
                       px_r(im, x, y), px_g(im, x, y),
                       expect_at(x, im->width), expect_at(y, im->height));
                bad++;
            }
        }
    }
    return bad == 0;
}

static void test_windows(void)
{
    printf("data window handling\n");
    char err[256];

    Image *plain = load("test/edge/plain.exr", err, sizeof err);
    CHECK(plain != NULL, "plain.exr must load: %s", err);
    if (!plain) return;
    CHECK(plain->width == 64 && plain->height == 48, "plain.exr is 64x48 (got %dx%d)",
          plain->width, plain->height);
    CHECK(check_pattern(plain, "plain"), "plain.exr pixels are in the right place");
    CHECK(px_b(plain, 10, 10) == color_linear_to_srgb8(0.25f), "constant blue channel decodes");
    CHECK(px_a(plain, 10, 10) == 0xff, "output alpha is opaque");

    /* Overscan: the data window is larger than the display window, so the
     * result must be cropped to the display window and otherwise identical. */
    Image *over = load("test/edge/overscan.exr", err, sizeof err);
    CHECK(over != NULL, "overscan.exr must load: %s", err);
    if (over) {
        CHECK(over->width == 64 && over->height == 48,
              "overscan is cropped to the display window (got %dx%d)", over->width, over->height);
        int same = over->width == plain->width && over->height == plain->height &&
                   memcmp(over->px, plain->px, plain->bytes) == 0;
        CHECK(same, "overscan crops to exactly the same image as plain.exr");
        image_unref(over);
    }

    /* Crop: the data window covers only part of the display window; the rest
     * must come back black rather than as uninitialised memory. */
    Image *crop = load("test/edge/crop.exr", err, sizeof err);
    CHECK(crop != NULL, "crop.exr must load: %s", err);
    if (crop) {
        CHECK(crop->width == 64 && crop->height == 48, "crop keeps the display window size");
        CHECK(px_r(crop, 0, 0) == 0 && px_g(crop, 0, 0) == 0 && px_b(crop, 0, 0) == 0,
              "outside the data window is black (got R%d G%d B%d)",
              px_r(crop, 0, 0), px_g(crop, 0, 0), px_b(crop, 0, 0));
        CHECK(px_r(crop, 63, 47) == 0 && px_b(crop, 63, 47) == 0,
              "the far corner is black too");
        CHECK(near(px_r(crop, 30, 20), expect_at(30, 64)) &&
              near(px_g(crop, 30, 20), expect_at(20, 48)),
              "inside the data window the pattern lands at the right place "
              "(got R%d G%d, wanted R%d G%d)",
              px_r(crop, 30, 20), px_g(crop, 30, 20), expect_at(30, 64), expect_at(20, 48));
        CHECK(px_b(crop, 30, 20) == color_linear_to_srgb8(0.25f), "inside pixels carry data");
        image_unref(crop);
    }
    image_unref(plain);
}

static void test_channel_sets(void)
{
    printf("channel layouts\n");
    char err[256];

    Image *mono = load("test/edge/mono.exr", err, sizeof err);
    CHECK(mono != NULL, "a single Y channel must load: %s", err);
    if (mono) {
        int x = 40, y = 12;
        CHECK(px_r(mono, x, y) == px_g(mono, x, y) && px_g(mono, x, y) == px_b(mono, x, y),
              "luminance replicates across RGB (got %d %d %d)",
              px_r(mono, x, y), px_g(mono, x, y), px_b(mono, x, y));
        CHECK(near(px_r(mono, x, y), expect_at(x, 64)), "luminance value is correct");
        image_unref(mono);
    }

    Image *rgba = load("test/edge/rgba.exr", err, sizeof err);
    CHECK(rgba != NULL, "RGBA must load: %s", err);
    if (rgba) {
        CHECK(check_pattern(rgba, "rgba"), "RGBA colour channels decode correctly");
        image_unref(rgba);
    }

    /* Files written by another encoder, covering the pixel types and
     * compressions we did not write ourselves. */
    static const char *const others[] = {
        "test/edge/float_none.exr", /* 32-bit float channels, uncompressed */
        "test/edge/half_rle.exr",   /* RLE compression */
        "test/edge/gray_zip1.exr",  /* single channel, zip1 */
    };
    for (int i = 0; i < (int)(sizeof others / sizeof *others); i++) {
        Image *im = load(others[i], err, sizeof err);
        CHECK(im != NULL, "%s must load: %s", others[i], err);
        if (im) {
            CHECK(im->width == 160 && im->height == 120, "%s is 160x120", others[i]);
            image_unref(im);
        }
    }
}

static void test_tiled(void)
{
    printf("tiled files\n");
    char err[256];

    /* exrmaketiled rewrites the scanline original losslessly, so the two must
     * decode to identical pixels. */
    Image *scan = load("test/seq_a/beauty.1001.exr", err, sizeof err);
    CHECK(scan != NULL, "the scanline original must load: %s", err);

    Image *tiled = load("test/edge/tiled.exr", err, sizeof err);
    CHECK(tiled != NULL, "a tiled file must load: %s", err);

    if (scan && tiled) {
        CHECK(scan->width == tiled->width && scan->height == tiled->height,
              "tiled and scanline agree on size (%dx%d vs %dx%d)",
              scan->width, scan->height, tiled->width, tiled->height);
        CHECK(scan->bytes == tiled->bytes && memcmp(scan->px, tiled->px, scan->bytes) == 0,
              "tiled decodes to exactly the same pixels as the scanline original");
    }

    Image *mip = load("test/edge/mipmap.exr", err, sizeof err);
    CHECK(mip != NULL, "a mipmapped file must load: %s", err);
    if (mip && scan) {
        CHECK(mip->width == scan->width && mip->height == scan->height,
              "a mipmapped file reads its full resolution level (got %dx%d)",
              mip->width, mip->height);
        CHECK(memcmp(mip->px, scan->px, scan->bytes) == 0,
              "mipmap level 0 matches the original");
        image_unref(mip);
    }
    image_unref(scan);
    image_unref(tiled);
}

static void test_bad_files(void)
{
    printf("damaged and missing files\n");
    char err[256];

    static const char *const bad[] = {
        "test/edge/truncated.exr", /* intact header, pixel data cut short */
        "test/edge/garbage.exr",
        "test/edge/empty.exr",
        "test/edge/does_not_exist.exr",
    };
    for (int i = 0; i < (int)(sizeof bad / sizeof *bad); i++) {
        err[0] = '\0';
        Image *im = load(bad[i], err, sizeof err);
        CHECK(im == NULL, "%s must be rejected rather than partly decoded", bad[i]);
        CHECK(err[0] != '\0', "%s must report a reason", bad[i]);
        image_unref(im);
    }

    /* Probing only reads the header, so a file whose header survived is
     * legitimately probeable even though its pixels are gone; that is what
     * lets a sequence still report its resolution when one frame is damaged.
     * Files with no readable header must fail. */
    int w = -1, h = -1;
    CHECK(reader_probe("test/edge/truncated.exr", &w, &h, NULL, err, sizeof err),
          "a truncated file with an intact header still probes");
    CHECK(w == 64 && h == 48, "and reports the right size (got %dx%d)", w, h);

    static const char *const unreadable[] = {
        "test/edge/garbage.exr",
        "test/edge/empty.exr",
        "test/edge/does_not_exist.exr",
    };
    for (int i = 0; i < (int)(sizeof unreadable / sizeof *unreadable); i++) {
        err[0] = '\0';
        CHECK(!reader_probe(unreadable[i], &w, &h, NULL, err, sizeof err),
              "%s must fail probing", unreadable[i]);
        CHECK(err[0] != '\0', "%s must report why probing failed", unreadable[i]);
    }
}

static void test_sequences(void)
{
    printf("sequence discovery\n");
    char err[512];
    char *one[1];

    /* One frame of a sequence expands to the whole sequence. */
    one[0] = (char *)"test/seq_a/beauty.1042.exr";
    Sequence *s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a single frame expands to its sequence: %s", err);
    if (s) {
        CHECK(s->count == 96, "found all 96 frames (got %d)", s->count);
        CHECK(s->frames[0].number == 1001, "first frame number is 1001 (got %ld)", s->frames[0].number);
        CHECK(s->frames[95].number == 1096, "last frame number is 1096 (got %ld)", s->frames[95].number);
        CHECK(strcmp(s->display, "beauty.####.exr") == 0,
              "display name is the pattern (got '%s')", s->display);
        sequence_free(s);
    }

    /* printf and hash patterns. */
    one[0] = (char *)"test/seq_a/beauty.%04d.exr";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s && s->count == 96, "a printf pattern resolves the sequence");
    sequence_free(s);

    one[0] = (char *)"test/seq_a/beauty.####.exr";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s && s->count == 96, "a hash pattern resolves the sequence");
    sequence_free(s);

    /* A directory picks the largest sequence in it. */
    one[0] = (char *)"test/mixed";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a directory resolves: %s", err);
    if (s) {
        CHECK(s->count == 5, "the largest group wins (got %d frames of %s)", s->count, s->display);
        CHECK(strcmp(s->display, "main.###.exr") == 0, "picked 'main' (got '%s')", s->display);
        sequence_free(s);
    }

    /* Gaps in the numbering are kept as gaps, in numeric order. */
    one[0] = (char *)"test/sparse";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a sparse sequence resolves: %s", err);
    if (s) {
        static const long want[] = { 1, 2, 3, 7, 8, 20, 100 };
        CHECK(s->count == 7, "all 7 sparse frames found (got %d)", s->count);
        int ok = s->count == 7;
        for (int i = 0; ok && i < 7; i++) ok = s->frames[i].number == want[i];
        CHECK(ok, "sparse frames are in numeric order with gaps preserved");
        sequence_free(s);
    }

    /* A file with no number in its name is a sequence of one. */
    one[0] = (char *)"test/mixed/single.exr";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "an unnumbered file resolves: %s", err);
    if (s) {
        CHECK(s->count == 1, "an unnumbered file is one frame (got %d)", s->count);
        sequence_free(s);
    }

    /* An explicit list is taken in numeric order. */
    char *many[3] = { (char *)"test/seq_a/beauty.1010.exr",
                      (char *)"test/seq_a/beauty.1002.exr",
                      (char *)"test/seq_a/beauty.1005.exr" };
    s = sequence_open(many, 3, err, sizeof err);
    CHECK(s != NULL, "an explicit list resolves: %s", err);
    if (s) {
        CHECK(s->count == 3 && s->frames[0].number == 1002 &&
              s->frames[1].number == 1005 && s->frames[2].number == 1010,
              "an explicit list is sorted by frame number");
        sequence_free(s);
    }

    /* Nonsense inputs fail with a message rather than crashing. */
    one[0] = (char *)"test/no/such/place";
    err[0] = '\0';
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s == NULL && err[0] != '\0', "a missing path reports an error");
    sequence_free(s);

    one[0] = (char *)"test/edge/does_not_exist.%04d.exr";
    err[0] = '\0';
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s == NULL && err[0] != '\0', "a pattern matching nothing reports an error");
    sequence_free(s);
}

int main(void)
{
    color_lut_init(&g_lut);
    g_scratch = decode_scratch_create();

    test_windows();
    test_channel_sets();
    test_tiled();
    test_bad_files();
    test_sequences();

    decode_scratch_destroy(g_scratch);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
