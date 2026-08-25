#ifndef VID_ATI_MACH64_3D_EXPAND_H
#define VID_ATI_MACH64_3D_EXPAND_H

#include <stdint.h>

/* SCALE_3D_CNTL.SCALE_PIX_EXPAND selects how sub-8-bit RGB source components
 * enter the 24-bit scaler/3D pipeline.  Clear means zero-extension; set means
 * dynamic-range correction to the full 0..255 range. */
static inline uint8_t
mach64_3d_expand_component(unsigned value, unsigned bits, int dynamic_range)
{
    unsigned max;

    if (!bits || bits >= 8u)
        return (uint8_t) value;

    max = (1u << bits) - 1u;
    value &= max;
    if (dynamic_range)
        return (uint8_t) ((value * 255u + max / 2u) / max);
    return (uint8_t) (value << (8u - bits));
}

#endif
