#ifndef VIDEO_VID_ATI_MACH64_3D_TEXTURE_PALETTE_H
#define VIDEO_VID_ATI_MACH64_3D_TEXTURE_PALETTE_H

#include <stdint.h>

#define MACH64_3D_TEXTURE_PALETTE_ENTRIES 256

static inline uint8_t
mach64_3d_texture_palette_index(uint32_t value)
{
    return (uint8_t) (value >> 24);
}

static inline uint32_t
mach64_3d_texture_palette_rgb(uint32_t value)
{
    return value & 0x00ffffffu;
}

static inline uint8_t
mach64_3d_texture_palette_red(uint32_t rgb)
{
    return (uint8_t) (rgb >> 16);
}

static inline uint8_t
mach64_3d_texture_palette_green(uint32_t rgb)
{
    return (uint8_t) (rgb >> 8);
}

static inline uint8_t
mach64_3d_texture_palette_blue(uint32_t rgb)
{
    return (uint8_t) rgb;
}

static inline void
mach64_3d_texture_palette_store(
    uint32_t palette[MACH64_3D_TEXTURE_PALETTE_ENTRIES], uint32_t value)
{
    palette[mach64_3d_texture_palette_index(value)] =
        mach64_3d_texture_palette_rgb(value);
}

static inline uint32_t
mach64_3d_texture_palette_lookup(
    const uint32_t palette[MACH64_3D_TEXTURE_PALETTE_ENTRIES], uint8_t index)
{
    return palette[index];
}

#endif
