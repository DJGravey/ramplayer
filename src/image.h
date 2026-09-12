/* image.h - reference-counted, display-ready frame images.
 *
 * Pixels are ARGB8888 packed into a uint32_t (0xAARRGGBB), top-down, with the
 * row stride equal to the width. This is exactly the layout the compositor in
 * draw.c and the SDL presentation texture both want, so a decoded frame needs
 * no further conversion between the loader thread and the screen.
 *
 * Images are shared between the cache (which owns one reference) and the main
 * thread (which takes a reference while drawing), so an image evicted from the
 * cache mid-draw stays alive until the drawing code lets go of it.
 */
#ifndef RP_IMAGE_H
#define RP_IMAGE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int         width;
    int         height;
    uint32_t   *px;     /* width * height ARGB8888 pixels */
    size_t      bytes;  /* pixel bytes, charged against the cache budget */
    atomic_int  refs;
} Image;

/* Returns NULL if the allocation fails (the caller treats that as a load
 * failure rather than a crash: we are deliberately filling memory). */
Image *image_new(int w, int h);

Image *image_ref(Image *im);
void   image_unref(Image *im);

#endif /* RP_IMAGE_H */
