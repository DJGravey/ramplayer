#include "image.h"

#include <stdlib.h>

Image *image_new(int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;

    /* Guard the multiply so a corrupt header cannot wrap the size. */
    size_t px = (size_t)w * (size_t)h;
    if (px / (size_t)w != (size_t)h) return NULL;
    size_t bytes = px * sizeof(uint32_t);
    if (bytes / sizeof(uint32_t) != px) return NULL;

    Image *im = calloc(1, sizeof *im);
    if (!im) return NULL;

    im->px = malloc(bytes);
    if (!im->px) {
        free(im);
        return NULL;
    }
    im->width  = w;
    im->height = h;
    im->bytes  = bytes;
    atomic_init(&im->refs, 1);
    return im;
}

Image *image_ref(Image *im)
{
    if (im) atomic_fetch_add_explicit(&im->refs, 1, memory_order_relaxed);
    return im;
}

void image_unref(Image *im)
{
    if (!im) return;
    if (atomic_fetch_sub_explicit(&im->refs, 1, memory_order_acq_rel) == 1) {
        free(im->px);
        free(im);
    }
}
