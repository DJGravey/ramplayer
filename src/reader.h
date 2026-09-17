/* reader.h - decoding frames into display-ready Images.
 *
 * Frames are converted to 8-bit display pixels on the loader thread rather
 * than at draw time. That keeps scrubbing free of per-frame work and halves
 * the memory each cached frame costs compared with keeping half floats, so the
 * RAM budget holds roughly twice as many frames.
 *
 * Two layers. The Reader is what the cache uses: one per loader thread, made
 * for a Sequence, loading a range of frames and handing each over as it is
 * decoded. Behind it sit two backends: EXR files (reader_exr.c), where every
 * frame is its own file and a range is one frame, and video (reader_video.c),
 * where frames come in groups from one keyframe to the next and a range is
 * decoded in order. The path-based EXR functions further down are the EXR
 * backend's own interface, kept public for the tests.
 */
#ifndef RP_READER_H
#define RP_READER_H

#include <stdatomic.h>
#include <stddef.h>

#include "color.h"
#include "image.h"
#include "sequence.h"

/* ---- the reader ---------------------------------------------------------- */

typedef struct Reader Reader;

/* A reader for one loader thread. Returns NULL and fills `err` if the
 * sequence cannot be decoded at all. */
Reader *reader_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz);
void    reader_close(Reader *r);

/* Called for each frame of a range as it is decoded, in order, on the loader
 * thread. Takes ownership of `im`. When `im` is NULL that frame failed to
 * decode and `err` says why; the range goes on to the next frame. Returns 1
 * to continue with the next frame, 0 to stop the range there. */
typedef int (*ReaderDeliver)(void *ud, int frame, Image *im, const char *err);

/* Optional: asked before a frame of the range is converted. Returning 0 skips
 * the frame (it is decoded if the ones after it need it, but never converted
 * or delivered), so frames the caller already holds cost nothing. */
typedef int (*ReaderWanted)(void *ud, int frame);

/* Decodes frames from..to-1 in order, delivering each. Returns 1 when the
 * range ended or the callback stopped it, 0 with `err` set when the reader
 * itself stopped: cancelled through `abort_flag` (`err` is
 * READER_ERR_CANCELLED) or failed in a way that ends the whole range, such as
 * a seek or read error. Frames not delivered by then are untouched. A frame
 * that produces no picture is delivered as a failure, so every frame of the
 * range is accounted for one way or the other. `wanted` may be NULL. */
int reader_load_range(Reader *r, int from, int to, ReaderDeliver deliver, ReaderWanted wanted,
                      void *ud, const atomic_int *abort_flag, char *err, size_t errsz);

/* The frame this reader would produce next without any seeking, or -1 when
 * it has no position (an image reader, or a video reader after a seek). A
 * range starting there, or ahead of it with no keyframe in between, costs no
 * seek; the cache uses this to give such a range to the reader that is
 * already there. */
int reader_position(const Reader *r);

/* ---- video ------------------------------------------------------------- */

/* What the container says about a video stream, read from its index without
 * touching frame data. `keyframe` has `count` entries, 1 where a group
 * starts; the caller frees it. */
typedef struct {
    int            count;
    int            width, height;
    double         fps;
    unsigned char *keyframe;
} VideoIndex;

int reader_video_index(const char *path, VideoIndex *out, char *err, size_t errsz);

/* Threads inside each video decoder (libavcodec frame threads). Call before
 * any reader is opened; 0 lets the decoder choose. */
void reader_set_decoder_threads(int n);

/* How a video reader got where it is: seeks made and pictures decoded since
 * it was opened. Returns 0 for a reader that is not a video reader. For the
 * tests, which check that following ranges continue without a seek. */
int reader_video_stats(const Reader *r, int *seeks, int *decoded);

/* ---- EXR files ----------------------------------------------------------- */

/* A DecodeScratch holds the reusable intermediate buffers for one loader
 * thread; it must not be shared between threads. */

typedef struct DecodeScratch DecodeScratch;

DecodeScratch *decode_scratch_create(void);
void           decode_scratch_destroy(DecodeScratch *s);

/* How frame files are read. With n == 0 (the default) the decoder reads the
 * file itself, chunk by chunk, and every loader thread is independent; the
 * right shape for SSDs and fast networks. With n > 0 each file is read whole
 * in one sequential pass, at most n files at a time across all threads, and
 * decoded from memory; n == 1 keeps a spinning disk streaming instead of
 * seeking between the loaders' files. Each scratch then keeps a buffer the
 * size of the largest file it has seen, outside the frame cache's budget.
 * Call before any loader thread starts. */
void reader_set_readers(int n);

/* Reads just the header. `fps` is optional; it receives the file's
 * framesPerSecond attribute, or 0 when the file does not carry one. */
int reader_probe(const char *path, int *w, int *h, double *fps, char *err, size_t errsz);

/* Values for the abort flag reader_load() polls. A soft cancel is honoured
 * only while less than half the chunks are decoded: past that, finishing is
 * cheaper than decoding the frame again, and the frame is useful when it
 * lands. A hard cancel stops at the next chunk regardless. */
enum {
    READER_CANCEL_NONE = 0,
    READER_CANCEL_SOFT = 1,
    READER_CANCEL_HARD = 2
};

/* The `err` text reader_load() returns when it stopped for a cancel rather
 * than failed. Callers compare against it: the flag alone cannot say whether
 * the decoder honoured a soft cancel or failed afterwards for real. */
#define READER_ERR_CANCELLED "cancelled"

/* Decodes `path`. Returns NULL and fills `err` on failure. If `abort_flag` is
 * non-NULL it is polled between chunks, so a quitting app does not have to
 * wait out a large file and a frame the playhead has left can be abandoned
 * early; see the READER_CANCEL values. */
Image *reader_load(const char *path, const ColorLUT *lut, DecodeScratch *scratch,
                   const atomic_int *abort_flag, char *err, size_t errsz);

#endif /* RP_READER_H */
