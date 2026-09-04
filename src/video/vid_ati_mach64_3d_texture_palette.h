#ifndef VIDEO_VID_ATI_MACH64_3D_TEXTURE_PALETTE_H
#define VIDEO_VID_ATI_MACH64_3D_TEXTURE_PALETTE_H

#include <stdint.h>

#define MACH64_3D_TEXTURE_PALETTE_ENTRIES 256

/*
 * Rage II/II+ CI4 textures use the CI8 scaler/texture datapath.  DP_PIX_WIDTH
 * selects a 16-entry palette bank in bits 23:20 and then selects whether each
 * byte contributes its low or high nibble.  With neither CI4 selector set the
 * byte is an ordinary CI8 index.  The two CI4 selector bits are mutually
 * exclusive in documented programming; treat the invalid both-set state as
 * CI8 rather than inventing a nibble priority.
 */
#define MACH64_DP_CI4_RGB_INDEX_SHIFT 20u
#define MACH64_DP_CI4_RGB_INDEX_MASK  0x0fu
#define MACH64_DP_CI4_RGB_LOW_NIBBLE  (1u << 26)
#define MACH64_DP_CI4_RGB_HIGH_NIBBLE (1u << 27)

static inline uint8_t
mach64_3d_texture_palette_texel_index(uint32_t dp_pix_width, uint8_t texel)
{
    uint32_t ci4_select = dp_pix_width &
                          (MACH64_DP_CI4_RGB_LOW_NIBBLE |
                           MACH64_DP_CI4_RGB_HIGH_NIBBLE);
    uint8_t bank;
    uint8_t index;

    if (ci4_select != MACH64_DP_CI4_RGB_LOW_NIBBLE &&
        ci4_select != MACH64_DP_CI4_RGB_HIGH_NIBBLE)
        return texel;

    bank = (uint8_t) ((dp_pix_width >> MACH64_DP_CI4_RGB_INDEX_SHIFT) &
                      MACH64_DP_CI4_RGB_INDEX_MASK);
    index = ci4_select == MACH64_DP_CI4_RGB_HIGH_NIBBLE ?
            (uint8_t) (texel >> 4) : (uint8_t) (texel & 0x0f);
    return (uint8_t) ((bank << 4) | index);
}

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
