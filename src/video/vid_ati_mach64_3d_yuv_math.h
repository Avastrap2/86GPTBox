#ifndef VIDEO_VID_ATI_MACH64_3D_YUV_MATH_H
#define VIDEO_VID_ATI_MACH64_3D_YUV_MATH_H

#include <stdint.h>

typedef struct mach64_yuv_rgb_t {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} mach64_yuv_rgb_t;

static inline uint8_t
mach64_yuv_clamp8(int value)
{
    return (uint8_t) (value < 0 ? 0 : (value > 255 ? 255 : value));
}

/*
 * APPLE_YUV_MODE changes U/V from the normal unsigned representation centered
 * on 128 to signed two's-complement values centered on zero.
 */
static inline int
mach64_yuv_chroma(uint8_t raw, int signed_uv)
{
    return signed_uv ? (int) (int8_t) raw : (int) raw - 128;
}

static inline mach64_yuv_rgb_t
mach64_yuv_centered_to_rgb(int y, int u, int v)
{
    mach64_yuv_rgb_t rgb;

    rgb.r = mach64_yuv_clamp8(y + ((359 * v) >> 8));
    rgb.g = mach64_yuv_clamp8(y - ((88 * u + 183 * v) >> 8));
    rgb.b = mach64_yuv_clamp8(y + ((454 * u) >> 8));
    return rgb;
}

static inline mach64_yuv_rgb_t
mach64_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, int signed_uv)
{
    return mach64_yuv_centered_to_rgb((int) y,
                                      mach64_yuv_chroma(u, signed_uv),
                                      mach64_yuv_chroma(v, signed_uv));
}

/* GT format 11 is packed YUYV: bytes are Y0, U, Y1, V. */
static inline uint8_t
mach64_yuyv_y(uint32_t packed, unsigned pixel)
{
    return (uint8_t) (packed >> ((pixel & 1u) ? 16u : 0u));
}

static inline uint8_t
mach64_yuyv_u(uint32_t packed)
{
    return (uint8_t) (packed >> 8);
}

static inline uint8_t
mach64_yuyv_v(uint32_t packed)
{
    return (uint8_t) (packed >> 24);
}

#endif
