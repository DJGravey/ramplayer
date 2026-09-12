#include "color.h"

#include <math.h>
#include <string.h>

float color_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1fu;
    uint32_t man  = (uint32_t)h & 0x3ffu;
    uint32_t bits;

    if (exp == 0) {
        if (man == 0) {
            bits = sign;                       /* +/- zero */
        } else {
            /* Subnormal half: renormalise into the float exponent range. */
            exp = 127 - 15 + 1;
            while (!(man & 0x400u)) {
                man <<= 1;
                exp--;
            }
            man &= 0x3ffu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (man << 13); /* inf / NaN */
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }

    float out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

uint8_t color_linear_to_srgb8(float v)
{
    /* The !(v > 0) form also rejects NaN, which renders as black rather than
     * as whatever garbage the cast would produce. */
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;

    float s = (v <= 0.0031308f) ? v * 12.92f
                                : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    int i = (int)(s * 255.0f + 0.5f);
    return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

void color_lut_init(ColorLUT *lut)
{
    for (int i = 0; i < 65536; i++)
        lut->half_to_srgb8[i] = color_linear_to_srgb8(color_half_to_float((uint16_t)i));
}
