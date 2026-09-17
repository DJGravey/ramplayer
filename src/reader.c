/* reader.c - hands a sequence to the backend that can decode it. */
#include "reader_internal.h"

#include <stdio.h>

Reader *reader_open(const Sequence *seq, const ColorLUT *lut, char *err, size_t errsz)
{
    if (errsz) err[0] = '\0';
    switch (seq->kind) {
    case SEQ_IMAGES: return reader_exr_open(seq, lut, err, errsz);
    case SEQ_VIDEO:  return reader_video_open(seq, lut, err, errsz);
    }
    snprintf(err, errsz, "unknown sequence kind");
    return NULL;
}

void reader_close(Reader *r)
{
    if (r) r->ops->close(r);
}

int reader_load_range(Reader *r, int from, int to, ReaderDeliver deliver, ReaderWanted wanted,
                      void *ud, const atomic_int *abort_flag, char *err, size_t errsz)
{
    if (errsz) err[0] = '\0';
    return r->ops->load_range(r, from, to, deliver, wanted, ud, abort_flag, err, errsz);
}

int reader_position(const Reader *r)
{
    return r ? r->ops->position(r) : -1;
}
