/* reader.h - decoding one frame file into a display-ready Image.
 *
 * Frames are converted to 8-bit display pixels on the loader thread rather
 * than at draw time. That keeps scrubbing free of per-frame work and halves
 * the memory each cached frame costs compared with keeping half floats, so the
 * RAM budget holds roughly twice as many frames.
 *
 * A DecodeScratch holds the reusable intermediate buffers for one loader
 * thread; it must not be shared between threads.
 */
#ifndef RP_READER_H
#define RP_READER_H

#include <stdatomic.h>
#include <stddef.h>

#include "color.h"
#include "image.h"

typedef struct DecodeScratch DecodeScratch;

DecodeScratch *decode_scratch_create(void);
void           decode_scratch_destroy(DecodeScratch *s);

/* Reads just the header. `fps` is optional; it receives the file's
 * framesPerSecond attribute, or 0 when the file does not carry one. */
int reader_probe(const char *path, int *w, int *h, double *fps, char *err, size_t errsz);

/* Decodes `path`. Returns NULL and fills `err` on failure. If `abort_flag` is
 * non-NULL it is polled between chunks so a quitting app does not have to wait
 * out a large file. */
Image *reader_load(const char *path, const ColorLUT *lut, DecodeScratch *scratch,
                   const atomic_int *abort_flag, char *err, size_t errsz);

#endif /* RP_READER_H */
