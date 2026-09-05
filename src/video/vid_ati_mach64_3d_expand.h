#ifndef VID_ATI_MACH64_3D_EXPAND_H
#define VID_ATI_MACH64_3D_EXPAND_H

#include <stdint.h>

/* SCALE_3D_CNTL.SCALE_PIX_EXPAND selects how sub-8-bit RGB source components
 * enter the 24-bit scaler/3D pipeline.  Clear means zero-extension; set means
 * dynamic-range correction.  The Rage family unpacker performs the latter by
 * repeating the source bit pattern into the unused low bits. */
static inline uint8_t
mach64_3d_expand_component(unsigned value, unsigned bits, int dynamic_range)
{
    unsigned result;
    unsigned remaining;

    if (!bits || bits >= 8u)
        return (uint8_t) value;

    value &= (1u << bits) - 1u;
    result = value << (8u - bits);
    if (!dynamic_range)
        return (uint8_t) result;

    remaining = 8u - bits;
    while (remaining) {
        unsigned copy = bits < remaining ? bits : remaining;
        result |= (value >> (bits - copy)) << (remaining - copy);
        remaining -= copy;
    }
    return (uint8_t) result;
}

#endif
