/* color.h - scene-linear to display conversion.
 *
 * EXR pixels are scene-linear. Turning them into screen values means applying a
 * display transform, and doing that per pixel with powf() would cost more than
 * the decode itself. Because we ask OpenEXR to hand us half floats, every
 * possible input value is one of 65536 bit patterns, so the whole transform
 * collapses into a single 64 KB lookup table built once at startup.
 */
#ifndef RP_COLOR_H
#define RP_COLOR_H

#include <stdint.h>

typedef struct {
    uint8_t half_to_srgb8[65536]; /* half bit pattern -> 8-bit display value */
} ColorLUT;

/* Builds the table for a linear -> sRGB display transform. */
void color_lut_init(ColorLUT *lut);

/* Exposed for tests and for code paths that need a one-off conversion. */
float   color_half_to_float(uint16_t h);
uint8_t color_linear_to_srgb8(float v);

#endif /* RP_COLOR_H */
