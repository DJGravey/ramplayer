/* reader_internal.h - what the reader backends share with the dispatcher.
 *
 * A Reader is a vtable pointer at the start of a backend's own struct; the
 * backend allocates the struct, points ops at its functions, and the
 * dispatcher in reader.c never looks past the first field.
 */
#ifndef RP_READER_INTERNAL_H
#define RP_READER_INTERNAL_H

#include "reader.h"

typedef struct {
    int  (*load_range)(Reader *r, int from, int to, ReaderDeliver deliver, ReaderWanted wanted,
                       void *ud, const atomic_int *abort_flag, char *err, size_t errsz);
    int  (*position)(const Reader *r);
    void (*close)(Reader *r);
} ReaderOps;

struct Reader {
    const ReaderOps *ops;
};

Reader *reader_exr_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz);
Reader *reader_video_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz);

#endif /* RP_READER_INTERNAL_H */
