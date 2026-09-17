/* reader_exr.c - EXR decoding via the pure C OpenEXRCore API.
 *
 * Pixels are requested as half floats regardless of how the file stores them,
 * which means the linear-to-display conversion is a single lookup per channel
 * against the 64 KB table in color.c instead of a pow() per pixel.
 *
 * Decoding proceeds chunk by chunk into a small scratch buffer that is
 * converted into the destination image immediately, so peak memory per loader
 * thread stays in the tens of kilobytes rather than a full float image.
 */
#include "reader_internal.h"

#include <openexr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

struct DecodeScratch {
    uint16_t *buf; /* interleaved half RGBA */
    size_t    cap; /* capacity in uint16_t elements */

    /* Whole-file mode: the file being decoded, kept between loads so steady
     * state allocates nothing. */
    uint8_t *file;
    size_t   file_cap;
    size_t   file_len;
};

DecodeScratch *decode_scratch_create(void)
{
    return rp_xcalloc(1, sizeof(DecodeScratch));
}

void decode_scratch_destroy(DecodeScratch *s)
{
    if (!s) return;
    free(s->buf);
    free(s->file);
    free(s);
}

/* ---- whole-file reading ---------------------------------------------------
 * A counting semaphore of g_readers permits bounds how many files are being
 * read at once, across every loader thread. Decoding happens outside it. */

static int     g_readers;
static int     g_permits;
static int     g_permits_init;
static RpMutex g_permit_mu;
static RpCond  g_permit_cv;

void reader_set_readers(int n)
{
    if (!g_permits_init) {
        rp_mutex_init(&g_permit_mu);
        rp_cond_init(&g_permit_cv);
        g_permits_init = 1;
    }
    g_readers = n > 0 ? n : 0;
    g_permits = g_readers;
}

static void permit_acquire(void)
{
    rp_mutex_lock(&g_permit_mu);
    while (g_permits <= 0) rp_cond_wait(&g_permit_cv, &g_permit_mu);
    g_permits--;
    rp_mutex_unlock(&g_permit_mu);
}

static void permit_release(void)
{
    rp_mutex_lock(&g_permit_mu);
    g_permits++;
    rp_cond_broadcast(&g_permit_cv);
    rp_mutex_unlock(&g_permit_mu);
}

static int aborted(const atomic_int *flag, int early);

/* Reads the whole of `path` into the scratch's file buffer. Only the read
 * itself holds a permit; opening, sizing and growing the buffer happen
 * outside it so the disk's time goes on transfers. */
static int read_whole_file(const char *path, DecodeScratch *s, const atomic_int *abort_flag,
                           char *err, size_t errsz)
{
    RpFile *f = rp_file_open(path);
    if (!f) {
        snprintf(err, errsz, "cannot open: file not found or unreadable");
        return 0;
    }
    int64_t size = rp_file_size(f);
    if (size < 0) {
        snprintf(err, errsz, "cannot open: size unknown");
        rp_file_close(f);
        return 0;
    }
    if ((uint64_t)size > s->file_cap) {
        /* The old contents are a previous frame; nothing to carry over. */
        free(s->file);
        s->file     = malloc((size_t)size);
        s->file_cap = s->file ? (size_t)size : 0;
        if (!s->file) {
            snprintf(err, errsz, "out of memory for a %lld byte file", (long long)size);
            rp_file_close(f);
            return 0;
        }
    }

    int ok = 0;
    permit_acquire();
    if (aborted(abort_flag, 1)) {
        /* Threads queued on the permit must not each do a full read of a
         * frame nobody wants any more before noticing; nothing is done yet,
         * so a soft cancel counts. */
        snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
    } else if (size > 0 && !rp_file_read(f, s->file, (size_t)size)) {
        snprintf(err, errsz, "read failed");
    } else {
        s->file_len = (size_t)size;
        ok = 1;
    }
    permit_release();

    rp_file_close(f);
    return ok;
}

/* OpenEXRCore's stream hooks over the file buffer, with pread semantics: a
 * request past the end returns a short count, which the library reports as a
 * read error just as it would for a truncated file on disk. */
static int64_t mem_read(exr_const_context_t ctxt, void *userdata, void *buffer,
                        uint64_t sz, uint64_t offset, exr_stream_error_func_ptr_t error_cb)
{
    (void)ctxt;
    (void)error_cb;
    const DecodeScratch *s = userdata;
    if (offset >= s->file_len) return 0;
    uint64_t avail = s->file_len - offset;
    if (sz > avail) sz = avail;
    memcpy(buffer, s->file + offset, (size_t)sz);
    return (int64_t)sz;
}

static int64_t mem_size(exr_const_context_t ctxt, void *userdata)
{
    (void)ctxt;
    return (int64_t)((const DecodeScratch *)userdata)->file_len;
}

static uint16_t *scratch_get(DecodeScratch *s, size_t need_elems)
{
    if (s->cap < need_elems) {
        uint16_t *nb = realloc(s->buf, need_elems * sizeof *nb);
        if (!nb) return NULL;
        s->buf = nb;
        s->cap = need_elems;
    }
    return s->buf;
}

/* OpenEXR's default handler prints to stderr. With several loader threads and
 * one bad file that turns into a flood, and we report errors ourselves. */
static void quiet_error_handler(exr_const_context_t ctxt, exr_result_t code, const char *msg)
{
    (void)ctxt; (void)code; (void)msg;
}

static void fail(char *err, size_t errsz, const char *what, exr_result_t rv)
{
    snprintf(err, errsz, "%s: %s", what, exr_get_error_code_as_string(rv));
}

/* ---- channel selection --------------------------------------------------- */

/* The part after the final '.' in a layered channel name ("diffuse.R" -> "R"). */
static const char *leaf_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static int find_channel(const exr_attr_chlist_t *ch, const char *want, int exact_only)
{
    for (int i = 0; i < ch->num_channels; i++) {
        const char *n = ch->entries[i].name.str;
        if (!n) continue;
        if (strcmp(n, want) == 0) return i;
    }
    if (exact_only) return -1;
    for (int i = 0; i < ch->num_channels; i++) {
        const char *n = ch->entries[i].name.str;
        if (n && strcmp(leaf_name(n), want) == 0) return i;
    }
    return -1;
}

/* Maps file channels onto our fixed R,G,B,A scratch slots. Returns 0 and fills
 * `err` if the file has nothing we can display. */
static int map_channels(const exr_attr_chlist_t *ch, int idx[4], int *mono,
                        char *err, size_t errsz)
{
    static const char *const names[4] = { "R", "G", "B", "A" };
    *mono = 0;

    if (ch->num_channels <= 0) {
        snprintf(err, errsz, "file has no channels");
        return 0;
    }

    for (int s = 0; s < 4; s++)
        idx[s] = find_channel(ch, names[s], 1);
    for (int s = 0; s < 4; s++)
        if (idx[s] < 0) idx[s] = find_channel(ch, names[s], 0);

    if (idx[0] < 0 && idx[1] < 0 && idx[2] < 0) {
        /* Not colour: try luminance, and failing that just show whatever the
         * first channel is as greyscale. */
        int y = find_channel(ch, "Y", 0);
        idx[0] = (y >= 0) ? y : 0;
        idx[1] = idx[2] = -1;
        *mono = 1;
    }

    for (int s = 0; s < 4; s++) {
        if (idx[s] < 0) continue;
        const exr_attr_chlist_entry_t *e = &ch->entries[idx[s]];
        if (e->x_sampling != 1 || e->y_sampling != 1) {
            snprintf(err, errsz, "subsampled channel '%s' is not supported",
                     e->name.str ? e->name.str : "?");
            return 0;
        }
    }
    return 1;
}

/* Points the decoder's channel outputs at our interleaved scratch. Channels we
 * do not want keep a NULL pointer, which tells OpenEXR to skip them. */
static void bind_channels(exr_decode_pipeline_t *d, const int fidx[4],
                          uint16_t *base, int stride_px)
{
    for (int c = 0; c < d->channel_count; c++) {
        exr_coding_channel_info_t *ci = &d->channels[c];
        ci->decode_to_ptr          = NULL;
        ci->user_data_type         = (uint16_t)EXR_PIXEL_HALF;
        ci->user_bytes_per_element = 2;
        ci->user_pixel_stride      = 4 * 2;
        ci->user_line_stride       = stride_px * 4 * 2;
    }
    for (int s = 0; s < 4; s++) {
        if (fidx[s] < 0 || fidx[s] >= d->channel_count) continue;
        d->channels[fidx[s]].decode_to_ptr = (uint8_t *)(base + s);
    }
}

/* ---- scratch -> image ---------------------------------------------------- */

static void convert_block(Image *im, const ColorLUT *lut, const uint16_t *src,
                          int sw, int sh, int src_stride_px,
                          int dst_x, int dst_y, int mono)
{
    for (int j = 0; j < sh; j++) {
        int y = dst_y + j;
        if (y < 0 || y >= im->height) continue;

        int i0 = (dst_x < 0) ? -dst_x : 0;
        int i1 = sw;
        if (dst_x + i1 > im->width) i1 = im->width - dst_x;
        if (i1 <= i0) continue;

        const uint16_t *srow = src + (size_t)j * (size_t)src_stride_px * 4;
        uint32_t *drow = im->px + (size_t)y * im->width + dst_x;

        if (mono) {
            for (int i = i0; i < i1; i++) {
                uint32_t v = lut->half_to_srgb8[srow[(size_t)i * 4 + 0]];
                drow[i] = 0xff000000u | (v << 16) | (v << 8) | v;
            }
        } else {
            for (int i = i0; i < i1; i++) {
                const uint16_t *p = srow + (size_t)i * 4;
                uint32_t r = lut->half_to_srgb8[p[0]];
                uint32_t g = lut->half_to_srgb8[p[1]];
                uint32_t b = lut->half_to_srgb8[p[2]];
                drow[i] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/* ---- public entry points ------------------------------------------------- */

/* With a scratch holding the file, the library decodes from memory and the
 * path only names the file in error messages; otherwise it reads the file
 * itself. */
static exr_result_t open_ctx(const char *path, DecodeScratch *from_memory, exr_context_t *out)
{
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.error_handler_fn = quiet_error_handler;
    if (from_memory) {
        init.user_data = from_memory;
        init.read_fn   = mem_read;
        init.size_fn   = mem_size;
    }
    return exr_start_read(out, path, &init);
}

int reader_probe(const char *path, int *w, int *h, double *fps, char *err, size_t errsz)
{
    if (fps) *fps = 0.0;

    exr_context_t ctxt = NULL;
    exr_result_t rv = open_ctx(path, NULL, &ctxt);
    if (rv != EXR_ERR_SUCCESS) {
        fail(err, errsz, "cannot open", rv);
        return 0;
    }

    exr_attr_box2i_t dispw;
    rv = exr_get_display_window(ctxt, 0, &dispw);

    /* Optional: sequences written by a tool that knows the rate carry it. */
    if (fps && rv == EXR_ERR_SUCCESS) {
        const exr_attribute_t *a = NULL;
        if (exr_get_attribute_by_name(ctxt, 0, "framesPerSecond", &a) == EXR_ERR_SUCCESS &&
            a && a->type == EXR_ATTR_RATIONAL && a->rational && a->rational->denom != 0)
            *fps = (double)a->rational->num / (double)a->rational->denom;
    }

    exr_finish(&ctxt);
    if (rv != EXR_ERR_SUCCESS) {
        fail(err, errsz, "cannot read display window", rv);
        return 0;
    }

    *w = dispw.max.x - dispw.min.x + 1;
    *h = dispw.max.y - dispw.min.y + 1;
    if (*w <= 0 || *h <= 0) {
        snprintf(err, errsz, "empty display window");
        return 0;
    }
    return 1;
}

/* `early` is whether less than half the work is done; a soft cancel is only
 * honoured then. */
static int aborted(const atomic_int *flag, int early)
{
    if (!flag) return 0;
    int v = atomic_load_explicit(flag, memory_order_relaxed);
    return v >= READER_CANCEL_HARD || (v == READER_CANCEL_SOFT && early);
}

Image *reader_load(const char *path, const ColorLUT *lut, DecodeScratch *scratch,
                   const atomic_int *abort_flag, char *err, size_t errsz)
{
    exr_context_t         ctxt    = NULL;
    exr_decode_pipeline_t decode;
    int                   started = 0;
    Image                *im      = NULL;
    exr_result_t          rv;

    /* Equivalent to EXR_DECODE_PIPELINE_INITIALIZER, written this way so the
     * struct's many unnamed fields do not trip -Wmissing-field-initializers. */
    memset(&decode, 0, sizeof decode);
    decode.pipe_size = sizeof decode;

    if (errsz) err[0] = '\0';

    DecodeScratch *from_memory = NULL;
    if (g_readers > 0) {
        if (aborted(abort_flag, 1)) {
            snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
            return NULL;
        }
        if (!read_whole_file(path, scratch, abort_flag, err, errsz)) return NULL;
        from_memory = scratch;
    }

    rv = open_ctx(path, from_memory, &ctxt);
    if (rv != EXR_ERR_SUCCESS) {
        fail(err, errsz, "cannot open", rv);
        return NULL;
    }

    const int part = 0;

    exr_storage_t storage = EXR_STORAGE_UNKNOWN;
    exr_get_storage(ctxt, part, &storage);
    if (storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED) {
        snprintf(err, errsz, "deep EXR files are not supported");
        goto done;
    }
    if (storage != EXR_STORAGE_SCANLINE && storage != EXR_STORAGE_TILED) {
        snprintf(err, errsz, "unrecognised EXR storage type");
        goto done;
    }

    exr_attr_box2i_t dw, dispw;
    if ((rv = exr_get_data_window(ctxt, part, &dw)) != EXR_ERR_SUCCESS ||
        (rv = exr_get_display_window(ctxt, part, &dispw)) != EXR_ERR_SUCCESS) {
        fail(err, errsz, "cannot read image windows", rv);
        goto done;
    }

    int W = dispw.max.x - dispw.min.x + 1;
    int H = dispw.max.y - dispw.min.y + 1;
    int dww = dw.max.x - dw.min.x + 1;
    int dwh = dw.max.y - dw.min.y + 1;
    if (W <= 0 || H <= 0 || dww <= 0 || dwh <= 0) {
        snprintf(err, errsz, "degenerate image window");
        goto done;
    }

    const exr_attr_chlist_t *chans = NULL;
    if ((rv = exr_get_channels(ctxt, part, &chans)) != EXR_ERR_SUCCESS || !chans) {
        fail(err, errsz, "cannot read channel list", rv);
        goto done;
    }

    int fidx[4], mono;
    if (!map_channels(chans, fidx, &mono, err, errsz)) goto done;

    im = image_new(W, H);
    if (!im) {
        snprintf(err, errsz, "out of memory for a %dx%d frame", W, H);
        goto done;
    }

    /* Anything the data window does not cover shows through as black, so only
     * pay for the clear when the data window really is smaller. */
    int covers = dw.min.x <= dispw.min.x && dw.min.y <= dispw.min.y &&
                 dw.max.x >= dispw.max.x && dw.max.y >= dispw.max.y;
    if (!covers) {
        size_t n = (size_t)W * (size_t)H;
        for (size_t i = 0; i < n; i++) im->px[i] = 0xff000000u;
    }

    const int off_x = dw.min.x - dispw.min.x;
    const int off_y = dw.min.y - dispw.min.y;

    if (storage == EXR_STORAGE_SCANLINE) {
        int32_t spc = 1;
        exr_get_scanlines_per_chunk(ctxt, part, &spc);
        if (spc < 1) spc = 1;

        size_t need = (size_t)dww * (size_t)spc * 4;
        uint16_t *buf = scratch_get(scratch, need);
        if (!buf) {
            snprintf(err, errsz, "out of memory for the decode buffer");
            goto fail_image;
        }
        if (fidx[1] < 0 || fidx[2] < 0) memset(buf, 0, need * sizeof *buf);

        int y = dw.min.y;
        while (y <= dw.max.y) {
            /* In whole-file mode the read, the costly part, is already done,
             * so a soft cancel would only throw that away. */
            if (aborted(abort_flag, !from_memory && (y - dw.min.y) * 2 < dw.max.y - dw.min.y + 1)) {
                snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
                goto fail_image;
            }

            exr_chunk_info_t ci;
            if ((rv = exr_read_scanline_chunk_info(ctxt, part, y, &ci)) != EXR_ERR_SUCCESS) {
                fail(err, errsz, "cannot read chunk header", rv);
                goto fail_image;
            }

            if (!started) {
                if ((rv = exr_decoding_initialize(ctxt, part, &ci, &decode)) != EXR_ERR_SUCCESS) {
                    fail(err, errsz, "cannot start decoding", rv);
                    goto fail_image;
                }
                started = 1;
                bind_channels(&decode, fidx, buf, dww);
                if ((rv = exr_decoding_choose_default_routines(ctxt, part, &decode)) != EXR_ERR_SUCCESS) {
                    fail(err, errsz, "unsupported pixel layout", rv);
                    goto fail_image;
                }
            } else {
                if ((rv = exr_decoding_update(ctxt, part, &ci, &decode)) != EXR_ERR_SUCCESS) {
                    fail(err, errsz, "cannot advance decoder", rv);
                    goto fail_image;
                }
                bind_channels(&decode, fidx, buf, dww);
            }

            if ((rv = exr_decoding_run(ctxt, part, &decode)) != EXR_ERR_SUCCESS) {
                fail(err, errsz, "decode failed", rv);
                goto fail_image;
            }

            int rows = RP_MIN(ci.height, dw.max.y - ci.start_y + 1);
            convert_block(im, lut, buf, RP_MIN(ci.width, dww), rows, dww,
                          off_x, ci.start_y - dispw.min.y, mono);

            int next = ci.start_y + ci.height;
            y = (next > y) ? next : y + spc;
        }
    } else {
        int32_t levx = 1, levy = 1;
        exr_get_tile_levels(ctxt, part, &levx, &levy);
        if (levx < 1 || levy < 1) {
            snprintf(err, errsz, "tiled file reports no levels");
            goto fail_image;
        }

        int32_t tilew = 0, tileh = 0, ntx = 0, nty = 0;
        if ((rv = exr_get_tile_sizes(ctxt, part, 0, 0, &tilew, &tileh)) != EXR_ERR_SUCCESS ||
            (rv = exr_get_tile_counts(ctxt, part, 0, 0, &ntx, &nty)) != EXR_ERR_SUCCESS) {
            fail(err, errsz, "cannot read tile layout", rv);
            goto fail_image;
        }
        if (tilew <= 0 || tileh <= 0) {
            snprintf(err, errsz, "invalid tile size");
            goto fail_image;
        }

        size_t need = (size_t)tilew * (size_t)tileh * 4;
        uint16_t *buf = scratch_get(scratch, need);
        if (!buf) {
            snprintf(err, errsz, "out of memory for the decode buffer");
            goto fail_image;
        }
        if (fidx[1] < 0 || fidx[2] < 0) memset(buf, 0, need * sizeof *buf);

        for (int ty = 0; ty < nty; ty++) {
            for (int tx = 0; tx < ntx; tx++) {
                if (aborted(abort_flag, !from_memory && (ty * ntx + tx) * 2 < ntx * nty)) {
                    snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
                    goto fail_image;
                }

                exr_chunk_info_t ci;
                if ((rv = exr_read_tile_chunk_info(ctxt, part, tx, ty, 0, 0, &ci)) != EXR_ERR_SUCCESS) {
                    fail(err, errsz, "cannot read tile header", rv);
                    goto fail_image;
                }

                if (!started) {
                    if ((rv = exr_decoding_initialize(ctxt, part, &ci, &decode)) != EXR_ERR_SUCCESS) {
                        fail(err, errsz, "cannot start decoding", rv);
                        goto fail_image;
                    }
                    started = 1;
                    bind_channels(&decode, fidx, buf, tilew);
                    if ((rv = exr_decoding_choose_default_routines(ctxt, part, &decode)) != EXR_ERR_SUCCESS) {
                        fail(err, errsz, "unsupported pixel layout", rv);
                        goto fail_image;
                    }
                } else {
                    if ((rv = exr_decoding_update(ctxt, part, &ci, &decode)) != EXR_ERR_SUCCESS) {
                        fail(err, errsz, "cannot advance decoder", rv);
                        goto fail_image;
                    }
                    bind_channels(&decode, fidx, buf, tilew);
                }

                if ((rv = exr_decoding_run(ctxt, part, &decode)) != EXR_ERR_SUCCESS) {
                    fail(err, errsz, "decode failed", rv);
                    goto fail_image;
                }

                convert_block(im, lut, buf,
                              RP_MIN(ci.width, tilew), RP_MIN(ci.height, tileh), tilew,
                              off_x + tx * tilew, off_y + ty * tileh, mono);
            }
        }
    }

    if (started) exr_decoding_destroy(ctxt, &decode);
    exr_finish(&ctxt);
    return im;

fail_image:
    image_unref(im);
    im = NULL;
done:
    if (started) exr_decoding_destroy(ctxt, &decode);
    exr_finish(&ctxt);
    return NULL;
}

/* ---- the Reader over EXR sequences ---------------------------------------
 * Every frame is its own file, so a range is loaded one frame at a time and
 * each delivered as it lands. A frame that fails is reported through the
 * callback and the range goes on; only a cancel ends it. */

typedef struct {
    Reader          base;
    const Sequence *seq;
    const ColorLUT *lut;
    DecodeScratch  *scratch;
} ExrReader;

static int exr_load_range(Reader *r, int from, int to, ReaderDeliver deliver, ReaderWanted wanted,
                          void *ud, const atomic_int *abort_flag, char *err, size_t errsz)
{
    ExrReader *er = (ExrReader *)r;
    char ferr[256];

    for (int f = from; f < to; f++) {
        if (wanted && !wanted(ud, f)) continue; /* every frame is its own file */
        ferr[0] = '\0';
        Image *im = reader_load(er->seq->frames[f].path, er->lut, er->scratch, abort_flag,
                                ferr, sizeof ferr);
        if (!im && strcmp(ferr, READER_ERR_CANCELLED) == 0) {
            snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
            return 0;
        }
        if (!deliver(ud, f, im, im ? NULL : ferr)) break;
    }
    return 1;
}

/* Any frame is as cheap as any other: no position to prefer. */
static int exr_position(const Reader *r)
{
    (void)r;
    return -1;
}

static void exr_close(Reader *r)
{
    ExrReader *er = (ExrReader *)r;
    decode_scratch_destroy(er->scratch);
    free(er);
}

static const ReaderOps exr_ops = { exr_load_range, exr_position, exr_close };

Reader *reader_exr_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz)
{
    (void)err; (void)errsz;
    ExrReader *er = rp_xcalloc(1, sizeof *er);
    er->base.ops = &exr_ops;
    er->seq      = seq;
    er->lut      = lut;
    er->scratch  = decode_scratch_create();
    return &er->base;
}
