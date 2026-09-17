#include "view.h"

#include <math.h>

#include "util.h"

void view_init(View *v)
{
    v->fit  = 1;
    v->zoom = 1.0;
    v->cx   = 0.0;
    v->cy   = 0.0;
}

void view_fit(View *v)
{
    v->fit = 1;
}

void view_one_to_one(View *v, int iw, int ih)
{
    v->fit  = 0;
    v->zoom = 1.0;
    v->cx   = iw / 2.0;
    v->cy   = ih / 2.0;
}

double view_scale(const View *v, Rect vp, int iw, int ih)
{
    if (!v->fit) return v->zoom;
    if (iw <= 0) return 1.0;
    Rect f = rect_fit(vp, iw, ih);
    return (double)f.w / (double)iw;
}

void view_release(View *v, Rect vp, int iw, int ih)
{
    if (!v->fit) return;
    v->zoom = view_scale(v, vp, iw, ih);
    v->cx   = iw / 2.0;
    v->cy   = ih / 2.0;
    v->fit  = 0;
}

void view_zoom_at(View *v, Rect vp, int iw, int ih, int mx, int my, double factor)
{
    view_release(v, vp, iw, ih);

    /* The cursor's offset from the viewport centre, in screen pixels, is the
     * same before and after; what changes is how many source pixels that
     * offset spans. */
    double ox = mx - (vp.x + vp.w / 2.0);
    double oy = my - (vp.y + vp.h / 2.0);
    double sx = v->cx + ox / v->zoom; /* source point under the cursor */
    double sy = v->cy + oy / v->zoom;

    double z = v->zoom * factor;
    z = RP_CLAMP(z, VIEW_ZOOM_MIN, VIEW_ZOOM_MAX);

    v->zoom = z;
    v->cx   = sx - ox / z;
    v->cy   = sy - oy / z;
}

void view_pan(View *v, Rect vp, int iw, int ih, int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    view_release(v, vp, iw, ih);
    v->cx -= dx / v->zoom;
    v->cy -= dy / v->zoom;
}

Rect view_rect(const View *v, Rect vp, int iw, int ih)
{
    if (v->fit) return rect_fit(vp, iw, ih);

    /* Centre the scaled frame, then shift it by how far the view's centre
     * point is from the frame's own middle. Done this way, 100% centred is
     * rect_center() exactly, the same rect the 1:1 blit fast path wants. */
    int w = (int)RP_MAX(1, lround(iw * v->zoom));
    int h = (int)RP_MAX(1, lround(ih * v->zoom));
    Rect r = rect_center(vp, w, h);
    r.x += (int)lround((iw / 2.0 - v->cx) * v->zoom);
    r.y += (int)lround((ih / 2.0 - v->cy) * v->zoom);
    return r;
}
