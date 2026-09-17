/* test_reader.c - EXR decoding and sequence discovery against awkward inputs.
 *
 * The fixtures under test/edge are written by tools/mkexr.c, whose pattern
 * encodes each pixel's position in the display window:
 *     R = x / (W-1)   G = y / (H-1)   B = 0.25
 * so a decoder that mislays the data window offset fails a position check
 * rather than merely looking wrong.
 */
#include <stdatomic.h>
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

#ifdef _WIN32
    /* Backslash paths, as a Windows shell hands them over, split the same way
     * as forward-slash ones: one frame expands to its whole sequence. */
    one[0] = (char *)"test\\sparse\\shot_0001.exr";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a backslash path resolves: %s", err);
    if (s) {
        CHECK(s->count == 7, "a backslash path finds all 7 sparse frames (got %d)", s->count);
        CHECK(strcmp(s->dir, "test\\sparse") == 0, "the directory keeps its spelling (got '%s')", s->dir);
        sequence_free(s);
    }
#endif

    /* Non-ASCII names go through the platform's file APIs as UTF-8. The
     * literals below are UTF-8 (MSVC is given /utf-8 for that). */
    one[0] = (char *)"test/unicod\xc3\xa9";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a non-ASCII directory resolves: %s", err);
    if (s) {
        CHECK(s->count == 3, "all 3 frames with non-ASCII names found (got %d)", s->count);
        CHECK(strcmp(s->display, "\xc3\xbcn\xc3\xaf.###.exr") == 0,
              "the display name keeps its UTF-8 (got '%s')", s->display);
        sequence_free(s);
    }
    one[0] = (char *)"test/unicod\xc3\xa9/\xc3\xbcn\xc3\xaf.002.exr";
    s = sequence_open(one, 1, err, sizeof err);
    CHECK(s != NULL, "a non-ASCII frame path resolves: %s", err);
    if (s) {
        CHECK(s->count == 3, "a non-ASCII frame expands to its sequence (got %d)", s->count);
        Image *im = s->count == 3 ? load(s->frames[1].path, err, sizeof err) : NULL;
        CHECK(im != NULL, "a non-ASCII frame decodes: %s", im ? "" : err);
        image_unref(im);
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

/* Whole-file mode keeps the file buffer between loads, so one scratch must
 * cope with a small file, then a much larger one, then the small one again,
 * and decode each exactly as it would fresh. */
static void test_scratch_reuse(void)
{
    printf("whole-file buffer reuse\n");
    char err[256];

    Image *small = load("test/edge/plain.exr", err, sizeof err);
    CHECK(small != NULL, "small file decodes first: %s", err);
    if (small) CHECK(check_pattern(small, "plain"), "small file is correct before the buffer grows");
    image_unref(small);

    Image *big = load("test/seq_a/beauty.1001.exr", err, sizeof err);
    CHECK(big != NULL, "a larger file decodes through the same scratch: %s", err);
    if (big) CHECK(big->width == 1280 && big->height == 720, "and has its own size (got %dx%d)", big->width, big->height);
    image_unref(big);

    small = load("test/edge/plain.exr", err, sizeof err);
    CHECK(small != NULL, "small file decodes again after the buffer grew: %s", err);
    if (small) CHECK(check_pattern(small, "plain"), "small file is correct with a larger buffer than it needs");
    image_unref(small);
}

/* ---- the Reader interface ------------------------------------------------ */

/* Collects what a range delivers: the frame numbers in order, whether every
 * frame decoded and carried the pattern expected of it, and optionally the
 * first image, kept for inspection. Can raise a soft cancel after a number
 * of frames. */
typedef struct {
    int        got[128];
    int        n;
    int        all_ok;
    Image     *first;
    int        stop_after; /* raise the cancel once this many arrived; 0 never */
    atomic_int abort;
    int      (*check)(const Image *, int frame);
} Collected;

static void collected_init(Collected *d, int (*check)(const Image *, int))
{
    memset(d, 0, sizeof *d);
    d->all_ok = 1;
    d->check  = check;
    atomic_init(&d->abort, 0);
}

static int collect(void *ud, int frame, Image *im, const char *err)
{
    Collected *d = ud;
    if (d->n < (int)(sizeof d->got / sizeof *d->got)) d->got[d->n] = frame;
    d->n++;
    if (!im) {
        printf("  frame %d failed: %s\n", frame, err ? err : "?");
        d->all_ok = 0;
    } else if (d->check && !d->check(im, frame)) {
        d->all_ok = 0;
    }
    if (im && !d->first) d->first = im;
    else image_unref(im);
    if (d->stop_after && d->n >= d->stop_after)
        atomic_store(&d->abort, READER_CANCEL_SOFT);
    return 1;
}

/* Did the range deliver exactly from..to-1, in order? */
static int got_exactly(const Collected *d, int from, int to)
{
    if (d->n != to - from) return 0;
    for (int i = 0; i < d->n; i++)
        if (d->got[i] != from + i) return 0;
    return 1;
}

static Sequence *open_one(const char *path)
{
    char err[256];
    char *one[1] = { (char *)path };
    Sequence *s = sequence_open(one, 1, err, sizeof err);
    if (!s) printf("  cannot open %s: %s\n", path, err);
    return s;
}

/* The EXR backend behind the Reader interface must give exactly what the
 * path-based loader gives. */
static void test_reader_interface_exr(void)
{
    printf("the Reader interface over EXR files\n");
    char err[256];

    Sequence *seq = open_one("test/seq_a/beauty.1001.exr");
    CHECK(seq != NULL, "the EXR sequence opens");
    if (!seq) return;

    Reader *r = reader_open(seq, &g_lut, err, sizeof err);
    CHECK(r != NULL, "an EXR reader opens: %s", err);
    if (r) {
        Collected d;
        collected_init(&d, NULL);
        CHECK(reader_load_range(r, 5, 6, collect, NULL, &d, NULL, err, sizeof err), "a one-frame range loads: %s", err);
        CHECK(got_exactly(&d, 5, 6), "and delivers exactly frame 5 (got %d frames)", d.n);
        Image *direct = load(seq->frames[5].path, err, sizeof err);
        CHECK(d.first && direct && d.first->bytes == direct->bytes &&
              memcmp(d.first->px, direct->px, direct->bytes) == 0,
              "the delivered frame is pixel for pixel the path-based decode");
        image_unref(direct);
        image_unref(d.first);

        /* A range spanning several frames delivers each in order. */
        collected_init(&d, NULL);
        CHECK(reader_load_range(r, 10, 13, collect, NULL, &d, NULL, err, sizeof err), "a three-frame range loads");
        CHECK(got_exactly(&d, 10, 13), "and delivers 10, 11, 12 in order");
        image_unref(d.first);

        /* A cancel raised before the load ends the range as cancelled. */
        collected_init(&d, NULL);
        atomic_store(&d.abort, READER_CANCEL_HARD);
        err[0] = '\0';
        CHECK(!reader_load_range(r, 20, 21, collect, NULL, &d, &d.abort, err, sizeof err) &&
              strcmp(err, READER_ERR_CANCELLED) == 0,
              "a cancelled range reports the cancel (got '%s')", err);
        CHECK(d.n == 0, "and delivers nothing");
        reader_close(r);
    }
    sequence_free(seq);
}

/* ---- video ---------------------------------------------------------------- */

static const char *const VIDEOS[] = {
    "test/video/h264_gop24.mp4",
    "test/video/h265_gop24_10bit.mp4",
    "test/video/h264_onegop.mp4",
    "test/video/h264_opengop.mp4",
};

/* Frame i of every fixture is a flat colour that encodes i (see
 * tools/make_test_video.sh). Lossy coding of a flat frame is close, so the
 * tolerance is generous but far below the step between neighbours. */
static int video_frame_is(const Image *im, int i)
{
    int r = (i * 53) & 255, g = (i * 97) & 255, b = (i * 29) & 255;
    int x = im->width / 2, y = im->height / 2;
    int ok = abs(px_r(im, x, y) - r) <= 12 && abs(px_g(im, x, y) - g) <= 12 &&
             abs(px_b(im, x, y) - b) <= 12;
    if (!ok)
        printf("  frame %d: got R%d G%d B%d, wanted R%d G%d B%d\n", i,
               px_r(im, x, y), px_g(im, x, y), px_b(im, x, y), r, g, b);
    return ok;
}

static void test_video_index(void)
{
    printf("video: the container index\n");
    char err[256];

    for (int v = 0; v < (int)(sizeof VIDEOS / sizeof *VIDEOS); v++) {
        const char *path = VIDEOS[v];
        VideoIndex vi;
        int ok = reader_video_index(path, &vi, err, sizeof err);
        CHECK(ok, "%s indexes: %s", path, err);
        if (!ok) continue;
        CHECK(vi.count == 96, "%s has 96 frames (got %d)", path, vi.count);
        CHECK(vi.width == 320 && vi.height == 180, "%s is 320x180 (got %dx%d)", path, vi.width, vi.height);
        CHECK(vi.fps > 23.9 && vi.fps < 24.1, "%s runs at 24 fps (got %g)", path, vi.fps);
        /* The index is in decode order. In a closed group the keyframe is
         * first in both orders; in an open group up to three leading frames
         * display before it, so its entry sits that much earlier. The reader
         * maps pictures by timestamp, so this only shifts where a group is
         * said to start. */
        int onegop  = strstr(path, "onegop") != NULL;
        int opengop = strstr(path, "opengop") != NULL;
        int keys[8], n_keys = 0;
        if (vi.count == 96) {
            CHECK(vi.keyframe[0], "%s: frame 0 is a keyframe", path);
            for (int i = 0; i < 96; i++)
                if (vi.keyframe[i] && n_keys < 8) keys[n_keys++] = i;
            if (onegop) {
                CHECK(n_keys == 1, "%s: no keyframe after the first (%d)", path, n_keys);
            } else {
                int placed = n_keys == 4;
                for (int k = 1; k < n_keys && k < 4; k++) {
                    int lo = opengop ? 24 * k - 3 : 24 * k;
                    if (keys[k] < lo || keys[k] > 24 * k) placed = 0;
                }
                CHECK(placed, "%s: four keyframes, at 24, 48, 72%s (got %d at %d, %d, %d)", path,
                      opengop ? " or up to three earlier" : "", n_keys,
                      n_keys > 1 ? keys[1] : -1, n_keys > 2 ? keys[2] : -1, n_keys > 3 ? keys[3] : -1);
            }
        }
        free(vi.keyframe);

        Sequence *s = open_one(path);
        CHECK(s != NULL, "%s opens as a sequence", path);
        if (s) {
            CHECK(s->kind == SEQ_VIDEO, "%s is a video sequence", path);
            CHECK(s->count == 96 && s->fps > 23.9 && s->width == 320, "%s carries count, rate and size", path);
            CHECK(s->frames[5].number == 5 && s->frames[5].path == NULL, "%s frames are numbered from 0 with no path", path);
            CHECK(strcmp(s->display, path + strlen("test/video/")) == 0, "%s display name is the file name (got '%s')", path, s->display);
            if (onegop) {
                CHECK(sequence_group_start(s, 95) == 0 && sequence_group_end(s, 0) == 96,
                      "%s: one group covering everything", path);
            } else if (n_keys == 4) {
                CHECK(sequence_group_start(s, 30) == keys[1] && sequence_group_end(s, 30) == keys[2],
                      "%s: frame 30 is in group %d..%d (got %d..%d)", path, keys[1], keys[2] - 1,
                      sequence_group_start(s, 30), sequence_group_end(s, 30));
                CHECK(sequence_group_start(s, 95) == keys[3] && sequence_group_end(s, keys[3]) == 96,
                      "%s: the last group is %d..95", path, keys[3]);
            }
            sequence_free(s);
        }
    }

    /* Bad inputs fail with a message. */
    VideoIndex vi;
    err[0] = '\0';
    CHECK(!reader_video_index("test/video/does_not_exist.mp4", &vi, err, sizeof err) && err[0],
          "a missing video reports an error");
    err[0] = '\0';
    CHECK(!reader_video_index("test/edge/plain.exr", &vi, err, sizeof err) && err[0],
          "a file that is not a video reports an error (got '%s')", err);
    err[0] = '\0';
    char *one[1] = { (char *)"test/video" };
    Sequence *s = sequence_open(one, 1, err, sizeof err);
    CHECK(s == NULL, "a directory of videos is not opened as a video");
    sequence_free(s);
}

static void test_video_decode(void)
{
    printf("video: decoding, seeking and continuing\n");
    char err[256];

    for (int v = 0; v < (int)(sizeof VIDEOS / sizeof *VIDEOS); v++) {
        const char *path = VIDEOS[v];
        Sequence *seq = open_one(path);
        if (!seq) { CHECK(0, "%s opens", path); continue; }

        /* The whole file, start to end, every frame the right one. */
        Reader *r = reader_open(seq, &g_lut, err, sizeof err);
        CHECK(r != NULL, "%s: a reader opens: %s", path, err);
        if (r) {
            Collected d;
            collected_init(&d, video_frame_is);
            CHECK(reader_load_range(r, 0, 96, collect, NULL, &d, NULL, err, sizeof err), "%s: 0..95 loads: %s", path, err);
            CHECK(got_exactly(&d, 0, 96), "%s: delivers all 96 frames in order (got %d)", path, d.n);
            CHECK(d.all_ok, "%s: every frame carries its own colour", path);
            CHECK(d.first && d.first->width == 320 && d.first->height == 180, "%s: frames are 320x180", path);
            image_unref(d.first);
            reader_close(r);
        }

        /* Ranges that follow on continue without a seek; a jump seeks. */
        r = reader_open(seq, &g_lut, err, sizeof err);
        if (r) {
            int seeks = 0;
            Collected d;
            collected_init(&d, video_frame_is);
            reader_load_range(r, 0, 10, collect, NULL, &d, NULL, err, sizeof err);
            CHECK(got_exactly(&d, 0, 10) && d.all_ok, "%s: 0..9 loads", path);
            reader_video_stats(r, &seeks, NULL);
            CHECK(seeks == 1, "%s: the first range seeks once (got %d)", path, seeks);
            image_unref(d.first);

            collected_init(&d, video_frame_is);
            reader_load_range(r, 10, 20, collect, NULL, &d, NULL, err, sizeof err);
            CHECK(got_exactly(&d, 10, 20) && d.all_ok, "%s: 10..19 follows on", path);
            reader_video_stats(r, &seeks, NULL);
            CHECK(seeks == 1, "%s: and does so without a seek (got %d)", path, seeks);
            image_unref(d.first);

            /* A jump forwards seeks to the group, unless there is no keyframe
             * between here and there, when decoding on is the cheaper way. */
            collected_init(&d, video_frame_is);
            reader_load_range(r, 50, 60, collect, NULL, &d, NULL, err, sizeof err);
            CHECK(got_exactly(&d, 50, 60) && d.all_ok, "%s: 50..59 after a jump delivers exactly those (got %d)", path, d.n);
            reader_video_stats(r, &seeks, NULL);
            int onegop = strstr(path, "onegop") != NULL;
            if (onegop) CHECK(seeks == 1, "%s: the jump decodes on rather than seeking to frame 0 (got %d)", path, seeks);
            else        CHECK(seeks >= 2, "%s: the jump seeks (got %d)", path, seeks);
            image_unref(d.first);

            collected_init(&d, video_frame_is);
            reader_load_range(r, 30, 35, collect, NULL, &d, NULL, err, sizeof err);
            CHECK(got_exactly(&d, 30, 35) && d.all_ok, "%s: a jump backwards to 30..34 delivers exactly those", path);
            image_unref(d.first);

            /* Every group's first frames, straight from a seek: the place
             * where an open group's decode-order keyframe misleads. */
            int starts_ok = 1;
            for (int gs = 0; gs < 96; gs += 24) {
                collected_init(&d, video_frame_is);
                reader_load_range(r, gs, gs + 3, collect, NULL, &d, NULL, err, sizeof err);
                if (!got_exactly(&d, gs, gs + 3) || !d.all_ok) starts_ok = 0;
                image_unref(d.first);
                collected_init(&d, video_frame_is);
                reader_load_range(r, gs + 20, gs + 22, collect, NULL, &d, NULL, err, sizeof err);
                if (!got_exactly(&d, gs + 20, gs + 22) || !d.all_ok) starts_ok = 0;
                image_unref(d.first);
            }
            CHECK(starts_ok, "%s: the frames after every seek are the right ones", path);
            reader_close(r);
        }
        sequence_free(seq);
    }

    /* A soft cancel stops a range at the next frame, keeps what was
     * delivered, and leaves the reader able to continue where it stopped. */
    Sequence *seq = open_one("test/video/h264_onegop.mp4");
    Reader *r = seq ? reader_open(seq, &g_lut, err, sizeof err) : NULL;
    CHECK(r != NULL, "the one-group file opens for the cancel case");
    if (r) {
        Collected d;
        collected_init(&d, video_frame_is);
        d.stop_after = 5;
        err[0] = '\0';
        int ok = reader_load_range(r, 0, 96, collect, NULL, &d, &d.abort, err, sizeof err);
        CHECK(!ok && strcmp(err, READER_ERR_CANCELLED) == 0, "a soft cancel ends the range as cancelled (got '%s')", err);
        CHECK(d.n >= 5 && d.n < 96, "the cancel took effect within a few frames (%d delivered)", d.n);
        CHECK(d.all_ok, "the frames delivered before the cancel are right");
        image_unref(d.first);

        int seeks_before = 0;
        reader_video_stats(r, &seeks_before, NULL);
        int resume = d.n;
        collected_init(&d, video_frame_is);
        CHECK(reader_load_range(r, resume, resume + 5, collect, NULL, &d, NULL, err, sizeof err),
              "the range after a cancel loads");
        int seeks_after = 0;
        reader_video_stats(r, &seeks_after, NULL);
        CHECK(got_exactly(&d, resume, resume + 5) && d.all_ok && seeks_after == seeks_before,
              "and continues from where the cancel stopped, without a seek");
        image_unref(d.first);
        reader_close(r);
    }
    sequence_free(seq);
}

int main(void)
{
    color_lut_init(&g_lut);
    g_scratch = decode_scratch_create();
    reader_set_decoder_threads(2);

    /* Every decode case runs with the decoder reading the file itself (0),
     * and in whole-file mode with one and with two permits. */
    static const int readers[] = { 0, 1, 2 };
    for (int i = 0; i < (int)(sizeof readers / sizeof *readers); i++) {
        printf("== readers=%d ==\n", readers[i]);
        reader_set_readers(readers[i]);
        test_windows();
        test_channel_sets();
        test_tiled();
        test_bad_files();
        test_sequences();
    }

    reader_set_readers(1);
    test_scratch_reuse();
    reader_set_readers(0);

    test_reader_interface_exr();
    test_video_index();
    test_video_decode();

    decode_scratch_destroy(g_scratch);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
