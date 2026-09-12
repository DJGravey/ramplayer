/* font.h - a built-in 5x7 bitmap font.
 *
 * The UI has to label frame numbers and print status text, and pulling in a
 * font stack for that would dwarf the rest of the program. The glyphs live in
 * font.c as ASCII art and are packed into bitmasks once at startup.
 */
#ifndef RP_FONT_H
#define RP_FONT_H

#include <stdint.h>

#define FONT_W 5
#define FONT_H 7

void font_init(void);

/* Returns FONT_H rows; in each row bit 4 is the leftmost pixel. Unknown
 * characters fall back to a blank. */
const uint8_t *font_glyph(int ch);

/* Horizontal distance from one character cell to the next, including the
 * one-pixel gap between glyphs. */
static inline int font_advance(int scale) { return (FONT_W + 1) * scale; }

int font_text_width(const char *s, int scale);

#endif /* RP_FONT_H */
