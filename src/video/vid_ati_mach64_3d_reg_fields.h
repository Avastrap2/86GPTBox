#ifndef VID_ATI_MACH64_3D_REG_FIELDS_H
#define VID_ATI_MACH64_3D_REG_FIELDS_H

#include <stdint.h>

/*
 * 3D RAGE GT/GTB trajectory/interpolation registers do not always consume all
 * 32 bits of the MMIO DWORD.  Keep values in the raw accumulator scale used by
 * the renderer, but discard reserved bits and sign-extend from the documented
 * physical field sign bit.
 */
static inline int32_t
mach64_3d_signed_field_decode(uint32_t raw, unsigned lsb, unsigned width)
{
    uint32_t mask = (UINT32_C(1) << width) - 1u;
    uint32_t sign = UINT32_C(1) << (width - 1);
    uint32_t field = (raw >> lsb) & mask;
    int32_t value = (int32_t) ((field ^ sign) - sign);

    return (int32_t) ((int64_t) value * (INT64_C(1) << lsb));
}

static inline uint32_t
mach64_3d_field_encode(int64_t value, uint32_t mask)
{
    return (uint32_t) value & mask;
}

/* Leading/trailing Bresenham ERR/INC/DEC: signed 18-bit in bits 17:0. */
static inline int32_t mach64_3d_s18_decode(uint32_t raw)
{
    return mach64_3d_signed_field_decode(raw, 0, 18);
}
static inline uint32_t mach64_3d_s18_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x0003ffff));
}

/* S/T second derivatives: signed S.10.16 in bits 26:0. */
static inline int32_t mach64_3d_s10_16_decode(uint32_t raw)
{
    return mach64_3d_signed_field_decode(raw, 0, 27);
}
static inline uint32_t mach64_3d_s10_16_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x07ffffff));
}

/* S/T first derivatives: signed S.11.16 in bits 27:0. */
static inline int32_t mach64_3d_s11_16_decode(uint32_t raw)
{
    return mach64_3d_signed_field_decode(raw, 0, 28);
}
static inline uint32_t mach64_3d_s11_16_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x0fffffff));
}

/* S/T START: unsigned 10.11 in bits 25:5, kept in the raw 16-fractional scale. */
static inline int32_t mach64_3d_u10_11_decode(uint32_t raw)
{
    return (int32_t) (raw & UINT32_C(0x03ffffe0));
}
static inline uint32_t mach64_3d_u10_11_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x03ffffe0));
}

/* RGB/alpha: signed S.8.12 in bits 24:4 (raw renderer scale has 16 frac bits). */
static inline int32_t mach64_3d_s8_12_decode(uint32_t raw)
{
    return mach64_3d_signed_field_decode(raw, 4, 21);
}
static inline uint32_t mach64_3d_s8_12_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x01fffff0));
}

/* Z: signed S.16.12 in bits 28:0. */
static inline int32_t mach64_3d_s16_12_decode(uint32_t raw)
{
    return mach64_3d_signed_field_decode(raw, 0, 29);
}
static inline uint32_t mach64_3d_s16_12_encode(int64_t value)
{
    return mach64_3d_field_encode(value, UINT32_C(0x1fffffff));
}

#endif
