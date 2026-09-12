/* ui.h - screen layout, hit testing, and widget drawing.
 *
 * The timeline maps its leftmost pixel to the first frame and its rightmost
 * pixel to the last, so ui_frame_at_x() and ui_x_for_frame() are exact
 * inverses at both ends. Everything that converts between pixels and frames
 * goes through those two functions.
 */
#ifndef RP_UI_H
#define RP_UI_H

#include "draw.h"

typedef struct App App;

typedef enum {
    BTN_PREV = 0,     /* step back one frame */
    BTN_PLAY_BACK,    /* play backwards / pause */
    BTN_PLAY_FWD,     /* play forwards / pause */
    BTN_NEXT,         /* step forward one frame */
    BTN_COUNT
} ButtonId;

typedef struct {
    Rect window;
    Rect viewport;              /* where the frame is drawn */
    Rect statusbar;
    Rect transport;             /* bottom bar */
    Rect buttons[BTN_COUNT];
    Rect timeline;              /* whole clickable timeline region */
    Rect track;                 /* the part that maps linearly to frames */
} Layout;

/* UI scale factor for HiDPI displays; 1 unless the window is scaled. */
void ui_set_scale(int scale);
int  ui_get_scale(void);

void ui_layout(Layout *L, int win_w, int win_h);

/* Which frame the given x coordinate refers to, clamped to the sequence. */
int ui_frame_at_x(const Layout *L, int x, int count);
int ui_x_for_frame(const Layout *L, int frame, int count);

/* Returns a ButtonId, or -1 if the point is not on a button. */
int ui_button_at(const Layout *L, int x, int y);

void ui_draw(Surface *s, App *app);

#endif /* RP_UI_H */
