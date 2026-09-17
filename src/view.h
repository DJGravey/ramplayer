/* view.h - how the frame sits in the viewport: fitted, or zoomed and panned.
 *
 * The view is either fitted, in which case the frame is scaled to the
 * viewport exactly as rect_fit() does it, or free, described by a zoom (screen
 * pixels per source pixel) and the source-image point that sits at the centre
 * of the viewport. Keeping the centre point rather than an offset means a
 * resize keeps the same picture centred, and zooming about the cursor is a
 * matter of moving that point so the pixel under the cursor stays put.
 *
 * Pure functions over a small struct, so the input handling can be tested
 * without a window or a cache.
 */
#ifndef RP_VIEW_H
#define RP_VIEW_H

#include "draw.h"

#define VIEW_ZOOM_MIN   (1.0 / 64.0)
#define VIEW_ZOOM_MAX   64.0
#define VIEW_WHEEL_STEP 1.25 /* zoom factor per wheel notch */

typedef struct {
    int    fit;    /* 1: fitted to the viewport; zoom, cx and cy are unused */
    double zoom;   /* screen pixels per source pixel */
    double cx, cy; /* the source point at the centre of the viewport */
} View;

/* Fitted to the viewport. */
void view_init(View *v);
void view_fit(View *v);

/* 100%, centred on the frame: black borders when the viewport is larger than
 * the frame, cropped when it is smaller. */
void view_one_to_one(View *v, int iw, int ih);

/* Leaves fit mode without moving the picture, so a zoom or a pan that follows
 * starts from what is on screen. A no-op when already free. */
void view_release(View *v, Rect vp, int iw, int ih);

/* Multiplies the zoom by `factor`, clamped to VIEW_ZOOM_MIN..VIEW_ZOOM_MAX,
 * keeping the source pixel under the screen point (mx, my) where it is. */
void view_zoom_at(View *v, Rect vp, int iw, int ih, int mx, int my, double factor);

/* Moves the picture by (dx, dy) screen pixels. Not clamped: the frame may go
 * anywhere, and view_fit() brings it back. */
void view_pan(View *v, Rect vp, int iw, int ih, int dx, int dy);

/* Where a frame of iw x ih is drawn, in screen pixels; may overflow `vp`. */
Rect view_rect(const View *v, Rect vp, int iw, int ih);

/* Screen pixels per source pixel, in either mode. */
double view_scale(const View *v, Rect vp, int iw, int ih);

#endif /* RP_VIEW_H */
