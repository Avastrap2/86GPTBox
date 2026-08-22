#ifndef VIDEO_VID_ATI_MACH64_3D_SCALER_MATH_H
#define VIDEO_VID_ATI_MACH64_3D_SCALER_MATH_H

#include <stdint.h>

/* The scaler accumulators have four reserved low bits and a 16.16 register
 * image for the documented 4.12 unsigned increment. */
static inline int
mach64_scaler_accum_pixel(int32_t accumulator)
{
    int pixel = accumulator / 0x10000;

    if ((accumulator < 0) && (accumulator % 0x10000))
        pixel--;
    return pixel;
}

static inline unsigned
mach64_scaler_accum_fraction5(int32_t accumulator)
{
    return ((uint32_t) accumulator >> 11) & 31u;
}

static inline uint8_t
mach64_scaler_lerp5(uint8_t first, uint8_t second, unsigned coefficient)
{
    coefficient &= 31u;
    return (uint8_t) (((unsigned) first * (32u - coefficient) +
                       (unsigned) second * coefficient + 16u) >> 5);
}

/* Mach64 DP_MIX truth table. */
static inline uint32_t
mach64_scaler_mix(uint32_t source, uint32_t destination, unsigned function)
{
    switch (function & 0x1fu) {
        case 0x00:
            return ~destination;
        case 0x01:
            return 0;
        case 0x02:
            return 0xffffffffu;
        case 0x03:
            return destination;
        case 0x04:
            return ~source;
        case 0x05:
            return source ^ destination;
        case 0x06:
            return ~(source ^ destination);
        case 0x07:
            return source;
        case 0x08:
            return ~(source & destination);
        case 0x09:
            return ~source | destination;
        case 0x0a:
            return source | ~destination;
        case 0x0b:
            return source | destination;
        case 0x0c:
            return source & destination;
        case 0x0d:
            return source & ~destination;
        case 0x0e:
            return ~source & destination;
        case 0x0f:
            return ~(source | destination);
        case 0x17:
            return (source + destination) >> 1;
        default:
            return source;
    }
}

#endif
