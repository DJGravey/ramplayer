/* reader_video.c - H.264 and H.265 video through FFmpeg.
 *
 * libavformat demuxes, libavcodec decodes, libswscale converts to the BGRA
 * layout the rest of the player wants. Video is display-referred already, so
 * the linear-to-sRGB table is not applied.
 *
 * Frames of a video are numbered in display order at a constant rate: frame
 * i sits at start + i * duration in the stream's time base. That is the one
 * assumption made everywhere here, and it is what turns a decoded picture's
 * timestamp back into a frame number. The container's packet index gives the
 * frame count and the keyframes; keyframes are where groups start, and a
 * group is what one loader decodes in one go.
 *
 * Demuxing sits behind the small Demux interface below so that an own MP4
 * demuxer could replace libavformat without touching the decoder.
 */
#include "reader_internal.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int g_decoder_threads;

void reader_set_decoder_threads(int n)
{
    g_decoder_threads = n > 0 ? n : 0;
}

static void av_fail(char *err, size_t errsz, const char *what, int ret)
{
    char buf[128];
    av_strerror(ret, buf, sizeof buf);
    snprintf(err, errsz, "%s: %s", what, buf);
}

/* FFmpeg's default logging prints to stderr from every thread. Errors are
 * reported through our own paths, so keep it quiet. */
static void quiet_ffmpeg(void)
{
    static int done;
    if (!done) {
        av_log_set_level(AV_LOG_QUIET);
        done = 1;
    }
}

/* ---- demuxing ------------------------------------------------------------
 * The interface: open a file on its first video stream, learn its timing,
 * seek to a keyframe, read packets in decode order. */

typedef struct {
    AVFormatContext *fmt;
    int              stream;
    AVRational       tb;    /* the stream's time base */
    AVRational       fps;
    int64_t          start; /* timestamp of frame 0 */
} Demux;

static int demux_open(Demux *d, const char *path, char *err, size_t errsz)
{
    memset(d, 0, sizeof *d);
    quiet_ffmpeg();

    int ret = avformat_open_input(&d->fmt, path, NULL, NULL);
    if (ret < 0) {
        av_fail(err, errsz, "cannot open", ret);
        return 0;
    }
    ret = avformat_find_stream_info(d->fmt, NULL);
    if (ret < 0) {
        av_fail(err, errsz, "cannot read stream info", ret);
        avformat_close_input(&d->fmt);
        return 0;
    }
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->stream < 0) {
        snprintf(err, errsz, "no video stream");
        avformat_close_input(&d->fmt);
        return 0;
    }

    /* Only the video stream is ever wanted; let the demuxer drop the rest
     * rather than hand every audio and subtitle packet over to be unreffed. */
    for (unsigned i = 0; i < d->fmt->nb_streams; i++)
        if ((int)i != d->stream) d->fmt->streams[i]->discard = AVDISCARD_ALL;

    AVStream *st = d->fmt->streams[d->stream];
    d->tb  = st->time_base;
    d->fps = st->avg_frame_rate;
    if (d->fps.num <= 0 || d->fps.den <= 0) d->fps = st->r_frame_rate;
    if (d->fps.num <= 0 || d->fps.den <= 0) {
        snprintf(err, errsz, "cannot determine the frame rate");
        avformat_close_input(&d->fmt);
        return 0;
    }
    d->start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    return 1;
}

static void demux_close(Demux *d)
{
    if (d->fmt) avformat_close_input(&d->fmt);
}

static AVStream *demux_stream(const Demux *d)
{
    return d->fmt->streams[d->stream];
}

/* Timestamp of frame i, and the frame a timestamp belongs to. */
static int64_t demux_pts_of(const Demux *d, int i)
{
    AVRational per_frame = { d->fps.den, d->fps.num };
    return d->start + av_rescale_q(i, per_frame, d->tb);
}

static int demux_index_of(const Demux *d, int64_t pts)
{
    double seconds = (double)(pts - d->start) * av_q2d(d->tb);
    return (int)llround(seconds * av_q2d(d->fps));
}

/* Positions the demuxer at the keyframe at or before frame `frame`. */
static int demux_seek(Demux *d, int frame, char *err, size_t errsz)
{
    int64_t ts = demux_pts_of(d, frame);
    int ret = avformat_seek_file(d->fmt, d->stream, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        av_fail(err, errsz, "seek failed", ret);
        return 0;
    }
    return 1;
}

/* The next packet of the video stream. Returns 1, 0 at the end of the file,
 * -1 on a read error. */
static int demux_read(Demux *d, AVPacket *pkt, char *err, size_t errsz)
{
    for (;;) {
        int ret = av_read_frame(d->fmt, pkt);
        if (ret == AVERROR_EOF) return 0;
        if (ret < 0) {
            av_fail(err, errsz, "read failed", ret);
            return -1;
        }
        if (pkt->stream_index == d->stream) return 1;
        av_packet_unref(pkt);
    }
}

/* ---- the index ----------------------------------------------------------- */

int reader_video_index(const char *path, VideoIndex *out, char *err, size_t errsz)
{
    memset(out, 0, sizeof *out);
    if (errsz) err[0] = '\0';

    Demux d;
    if (!demux_open(&d, path, err, errsz)) return 0;

    AVStream *st = demux_stream(&d);
    const AVCodecParameters *par = st->codecpar;
    if (par->width <= 0 || par->height <= 0) {
        snprintf(err, errsz, "video stream has no size");
        demux_close(&d);
        return 0;
    }
    if (!avcodec_find_decoder(par->codec_id)) {
        snprintf(err, errsz, "no decoder for %s", avcodec_get_name(par->codec_id));
        demux_close(&d);
        return 0;
    }

    /* The packet index: one entry per frame for MP4, keyframes flagged. Some
     * containers carry no index, in which case the stream's own frame count
     * or its duration has to do, and only frame 0 is known to be a keyframe. */
    int n = avformat_index_get_entries_count(st);
    int from_index = n > 0;
    if (n <= 0 && st->nb_frames > 0) n = (int)RP_MIN(st->nb_frames, 1 << 30);
    if (n <= 0 && st->duration > 0)
        n = demux_index_of(&d, d.start + st->duration);
    if (n <= 0) {
        snprintf(err, errsz, "cannot determine the frame count");
        demux_close(&d);
        return 0;
    }

    out->count    = n;
    out->width    = par->width;
    out->height   = par->height;
    out->fps      = av_q2d(d.fps);
    out->keyframe = rp_xcalloc((size_t)n, 1);
    out->keyframe[0] = 1;
    if (from_index) {
        for (int i = 0; i < n; i++) {
            const AVIndexEntry *e = avformat_index_get_entry(st, i);
            if (e && (e->flags & AVINDEX_KEYFRAME)) out->keyframe[i] = 1;
        }
    }

    demux_close(&d);
    return 1;
}

/* ---- the reader ---------------------------------------------------------- */

typedef struct {
    Reader          base;
    const Sequence *seq;

    Demux              dm;
    AVCodecContext    *cc;
    AVPacket          *pkt;
    AVFrame           *frame;
    struct SwsContext *sws;
    int                sws_w, sws_h, sws_fmt, sws_space, sws_range;

    int next;     /* the frame the decoder produces next, -1 after a seek */
    int drained;  /* end of stream sent to the decoder */
    int pending;  /* r->pkt holds a packet the decoder has not taken yet */

    /* How the reader got where it is; read by the tests and the cache's
     * stats from other threads, hence atomic. */
    atomic_int seeks;
    atomic_int decoded;
} VideoReader;

enum { PICTURE = 1, END_OF_STREAM = 0, FAILED = -1, STOPPED = -2 };

static int aborted(const atomic_int *flag)
{
    return flag && atomic_load_explicit(flag, memory_order_relaxed) >= READER_CANCEL_SOFT;
}

/* Converts the decoded picture to a display-ready image, in the stream's own
 * colour matrix and range. */
static Image *convert(VideoReader *r, const AVFrame *f, char *err, size_t errsz)
{
    int space = f->colorspace, range = f->color_range;
    if (r->sws && (f->width != r->sws_w || f->height != r->sws_h || f->format != r->sws_fmt ||
                   space != r->sws_space || range != r->sws_range)) {
        sws_freeContext(r->sws);
        r->sws = NULL;
    }
    if (!r->sws) {
        r->sws = sws_getContext(f->width, f->height, f->format, f->width, f->height,
                                AV_PIX_FMT_BGRA, SWS_BILINEAR | SWS_ACCURATE_RND, NULL, NULL, NULL);
        if (!r->sws) {
            snprintf(err, errsz, "cannot convert %s to BGRA",
                     av_get_pix_fmt_name(f->format) ? av_get_pix_fmt_name(f->format) : "?");
            return NULL;
        }
        /* An untagged stream is taken as BT.709 for HD and BT.601 below it,
         * the same guess every player makes. */
        int cs;
        switch (space) {
        case AVCOL_SPC_BT709:     cs = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: cs = SWS_CS_ITU601; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
        default:                  cs = f->height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
        }
        int full = range == AVCOL_RANGE_JPEG;
        sws_setColorspaceDetails(r->sws, sws_getCoefficients(cs), full,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        r->sws_w = f->width;
        r->sws_h = f->height;
        r->sws_fmt = f->format;
        r->sws_space = space;
        r->sws_range = range;
    }

    Image *im = image_new(f->width, f->height);
    if (!im) {
        snprintf(err, errsz, "out of memory for a %dx%d frame", f->width, f->height);
        return NULL;
    }
    uint8_t *dst[4]      = { (uint8_t *)im->px, NULL, NULL, NULL };
    int      dst_line[4] = { f->width * 4, 0, 0, 0 };
    sws_scale(r->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_line);
    return im;
}

/* Pulls the next decoded picture into r->frame. Returns PICTURE with one,
 * END_OF_STREAM when there are no more, FAILED with `err` set on a cancel or
 * a read error, STOPPED when the callback ended the range. A packet the
 * decoder rejects is reported to `deliver` as that frame's failure when it
 * is within the range, and decoding goes on. */
static int next_picture(VideoReader *r, int from, int to, ReaderDeliver deliver, void *ud,
                        const atomic_int *abort_flag, char *err, size_t errsz)
{
    int refused = 0;
    for (;;) {
        int ret = avcodec_receive_frame(r->cc, r->frame);
        if (ret == 0) {
            atomic_fetch_add_explicit(&r->decoded, 1, memory_order_relaxed);
            return PICTURE;
        }
        if (ret == AVERROR_EOF) return END_OF_STREAM;
        if (ret != AVERROR(EAGAIN)) {
            av_fail(err, errsz, "decode failed", ret);
            return FAILED;
        }
        if (r->drained) return END_OF_STREAM;

        if (aborted(abort_flag)) {
            snprintf(err, errsz, "%s", READER_ERR_CANCELLED);
            return FAILED;
        }

        if (!r->pending) {
            int rr = demux_read(&r->dm, r->pkt, err, errsz);
            if (rr < 0) return FAILED;
            if (rr == 0) {
                avcodec_send_packet(r->cc, NULL);
                r->drained = 1;
                continue;
            }
            r->pending = 1;
        }
        ret = avcodec_send_packet(r->cc, r->pkt);
        if (ret == AVERROR(EAGAIN)) {
            /* The decoder wants its output read first. The API promises
             * this cannot follow a receive that asked for input, so a
             * second refusal in a row is a stall, not a queue. */
            if (refused++) {
                snprintf(err, errsz, "decoder stalled");
                return FAILED;
            }
            continue; /* the packet stays pending: receive, then resend */
        }
        refused = 0;
        r->pending = 0;
        if (ret < 0) {
            int idx = r->pkt->pts == AV_NOPTS_VALUE ? -1 : demux_index_of(&r->dm, r->pkt->pts);
            av_packet_unref(r->pkt);
            if (idx >= from && idx < to) {
                char msg[128];
                av_fail(msg, sizeof msg, "frame rejected", ret);
                if (!deliver(ud, idx, NULL, msg)) return STOPPED;
            }
            continue;
        }
        av_packet_unref(r->pkt);
    }
}

static int video_seek(VideoReader *r, int frame, char *err, size_t errsz)
{
    if (!demux_seek(&r->dm, frame, err, errsz)) return 0;
    avcodec_flush_buffers(r->cc);
    if (r->pending) av_packet_unref(r->pkt);
    r->pending = 0;
    r->drained = 0;
    r->next = -1;
    atomic_fetch_add_explicit(&r->seeks, 1, memory_order_relaxed);
    return 1;
}

/* Frames from..to-1 that no picture will ever arrive for are reported as
 * failures, so the caller does not ask for them again and again. Returns 0
 * if the callback stopped the range. */
static int deliver_missing(int from, int to, const char *why, ReaderDeliver deliver,
                           ReaderWanted wanted, void *ud)
{
    for (int k = from; k < to; k++) {
        if (wanted && !wanted(ud, k)) continue;
        if (!deliver(ud, k, NULL, why)) return 0;
    }
    return 1;
}

static int video_load_range(Reader *base, int from, int to, ReaderDeliver deliver, ReaderWanted wanted,
                            void *ud, const atomic_int *abort_flag, char *err, size_t errsz)
{
    VideoReader *r = (VideoReader *)base;
    if (from >= to) return 1;

    /* Continue where the decoder is when the range follows on, or lies
     * ahead with no keyframe in between; otherwise seek to the group. */
    int gs = sequence_group_start(r->seq, from);
    int can_continue = r->next >= 0 && r->next <= from && gs <= r->next;
    if (!can_continue && !video_seek(r, gs, err, errsz)) return 0;

    int retried = 0;
    while (r->next < 0 || r->next < to) {
        int got = next_picture(r, from, to, deliver, ud, abort_flag, err, errsz);
        if (got == FAILED) return 0;
        if (got == STOPPED) return 1;
        if (got == END_OF_STREAM) {
            /* The stream ended short of the range: the container promised
             * frames that are not there. */
            int k = r->next < 0 ? from : RP_MAX(r->next, from);
            deliver_missing(k, to, "past the end of the stream", deliver, wanted, ud);
            r->next = to;
            return 1;
        }

        int64_t pts = r->frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = r->frame->pts;
        int idx = pts == AV_NOPTS_VALUE ? (r->next < 0 ? from : r->next) : demux_index_of(&r->dm, pts);

        if (r->next < 0 && idx > from && !retried && gs > 0) {
            /* Landed past the range: an open group whose keyframe comes
             * after its first frames in display order. Once more, from the
             * group before. */
            av_frame_unref(r->frame);
            retried = 1;
            if (!video_seek(r, sequence_group_start(r->seq, gs - 1), err, errsz)) return 0;
            continue;
        }

        /* Frames between the one expected and the one that arrived have no
         * picture: a dropped or damaged one, or a file whose frames are not
         * evenly spaced. Report them, else the cache would ask for them
         * again and this reader would seek and decode the group for nothing,
         * without end. */
        int expect = r->next < 0 ? from : r->next;
        r->next = idx + 1;
        if (idx > expect && idx > from &&
            !deliver_missing(RP_MAX(expect, from), RP_MIN(idx, to), "no picture for this frame",
                             deliver, wanted, ud)) {
            av_frame_unref(r->frame);
            return 1;
        }

        if (idx < from || idx >= to || (wanted && !wanted(ud, idx))) {
            av_frame_unref(r->frame);
            continue;
        }

        char ferr[256];
        Image *im = convert(r, r->frame, ferr, sizeof ferr);
        av_frame_unref(r->frame);
        if (!deliver(ud, idx, im, im ? NULL : ferr)) return 1;
    }
    return 1;
}

static int video_position(const Reader *base)
{
    return ((const VideoReader *)base)->next;
}

static void video_close(Reader *base)
{
    VideoReader *r = (VideoReader *)base;
    if (r->pending && r->pkt) av_packet_unref(r->pkt);
    if (r->sws) sws_freeContext(r->sws);
    av_frame_free(&r->frame);
    av_packet_free(&r->pkt);
    avcodec_free_context(&r->cc);
    demux_close(&r->dm);
    free(r);
}

static const ReaderOps video_ops = { video_load_range, video_position, video_close };

Reader *reader_video_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz)
{
    (void)lut; /* video is display-referred already */
    VideoReader *r = rp_xcalloc(1, sizeof *r);
    r->base.ops = &video_ops;
    r->seq      = seq;
    r->next     = -1;
    atomic_init(&r->seeks, 0);
    atomic_init(&r->decoded, 0);

    if (!demux_open(&r->dm, seq->video_path, err, errsz)) {
        free(r);
        return NULL;
    }
    const AVCodecParameters *par = demux_stream(&r->dm)->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        snprintf(err, errsz, "no decoder for %s", avcodec_get_name(par->codec_id));
        video_close(&r->base);
        return NULL;
    }
    r->cc = avcodec_alloc_context3(codec);
    r->pkt = av_packet_alloc();
    r->frame = av_frame_alloc();
    if (!r->cc || !r->pkt || !r->frame) {
        snprintf(err, errsz, "out of memory opening the decoder");
        video_close(&r->base);
        return NULL;
    }
    int ret = avcodec_parameters_to_context(r->cc, par);
    if (ret >= 0) {
        r->cc->pkt_timebase = r->dm.tb;
        r->cc->thread_count = g_decoder_threads;
        r->cc->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        ret = avcodec_open2(r->cc, codec, NULL);
    }
    if (ret < 0) {
        av_fail(err, errsz, "cannot open the decoder", ret);
        video_close(&r->base);
        return NULL;
    }
    return &r->base;
}

/* For the tests: how many seeks and decoded pictures a reader has done. */
int reader_video_stats(const Reader *base, int *seeks, int *decoded)
{
    const VideoReader *r = (const VideoReader *)base;
    if (base->ops != &video_ops) return 0;
    if (seeks)   *seeks = atomic_load_explicit(&r->seeks, memory_order_relaxed);
    if (decoded) *decoded = atomic_load_explicit(&r->decoded, memory_order_relaxed);
    return 1;
}
