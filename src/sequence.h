/* sequence.h - turning a command line argument into an ordered list of frames.
 *
 * Accepts any of:
 *   /path/to/render.0042.exr   one frame of a sequence -> the whole sequence
 *   /path/to/render.%04d.exr   a printf pattern
 *   /path/to/render.####.exr   a hash pattern
 *   /path/to/dir               the largest sequence in that directory
 *   a.exr b.exr c.exr          an explicit list
 *   /path/to/shot.mp4          a video: one file, many frames
 *
 * The frame list is immutable once built, which is what lets the loader
 * threads read paths out of it without holding the cache lock.
 *
 * A video is a sequence whose frames all live in one file. Its frames are
 * numbered from 0, carry no path of their own, and come in groups: a group is
 * the run of frames that must be decoded together, from one keyframe to the
 * next. The group tables let the cache hand a whole group to one loader.
 */
#ifndef RP_SEQUENCE_H
#define RP_SEQUENCE_H

#include <stddef.h>

typedef struct {
    char *path;
    long  number; /* frame number parsed from the name, or the index if absent */
} SeqFrame;

typedef enum {
    SEQ_IMAGES = 0, /* one file per frame */
    SEQ_VIDEO  = 1  /* one file, decoded in groups */
} SeqKind;

typedef struct {
    SeqKind   kind;
    SeqFrame *frames;
    int       count;
    char     *dir;     /* directory the frames live in */
    char     *display; /* human readable pattern, e.g. "render.####.exr" */
    int       width;   /* images: filled in by the probe, 0 until then;
                          video: from the container */
    int       height;

    /* Video only; NULL or 0 for image sequences. */
    char   *video_path;  /* the file every frame comes from */
    double  fps;         /* the container's frame rate */
    int    *group_start; /* per frame: the first frame of its group */
    int    *group_end;   /* per frame: one past the last frame of its group */
} Sequence;

/* The group a frame belongs to, as [start, end). Every image frame is a
 * group of one. */
static inline int sequence_group_start(const Sequence *s, int f)
{
    return s->group_start ? s->group_start[f] : f;
}
static inline int sequence_group_end(const Sequence *s, int f)
{
    return s->group_end ? s->group_end[f] : f + 1;
}

Sequence *sequence_open(char *const *inputs, int n_inputs, char *err, size_t errsz);
void      sequence_free(Sequence *s);

#endif /* RP_SEQUENCE_H */
