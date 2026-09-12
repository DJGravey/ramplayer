/* sequence.h - turning a command line argument into an ordered list of frames.
 *
 * Accepts any of:
 *   /path/to/render.0042.exr   one frame of a sequence -> the whole sequence
 *   /path/to/render.%04d.exr   a printf pattern
 *   /path/to/render.####.exr   a hash pattern
 *   /path/to/dir               the largest sequence in that directory
 *   a.exr b.exr c.exr          an explicit list
 *
 * The frame list is immutable once built, which is what lets the loader
 * threads read paths out of it without holding the cache lock.
 */
#ifndef RP_SEQUENCE_H
#define RP_SEQUENCE_H

#include <stddef.h>

typedef struct {
    char *path;
    long  number; /* frame number parsed from the name, or the index if absent */
} SeqFrame;

typedef struct {
    SeqFrame *frames;
    int       count;
    char     *dir;     /* directory the frames live in */
    char     *display; /* human readable pattern, e.g. "render.####.exr" */
    int       width;   /* filled in by sequence_probe(), 0 until then */
    int       height;
} Sequence;

Sequence *sequence_open(char *const *inputs, int n_inputs, char *err, size_t errsz);
void      sequence_free(Sequence *s);

#endif /* RP_SEQUENCE_H */
