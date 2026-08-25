#ifndef VID_ATI_MACH64_3D_MIPMAP_H
#define VID_ATI_MACH64_3D_MIPMAP_H

#include <stdint.h>

typedef struct mach64_3d_mip_lod_t {
    int minifying;
    int floor_lod;
    int nearest_lod;
    int fraction;
} mach64_3d_mip_lod_t;

typedef enum mach64_3d_texture_filter_t {
    MACH64_3D_TEXTURE_FILTER_NONE = 0,
    MACH64_3D_TEXTURE_FILTER_NEAREST,
    MACH64_3D_TEXTURE_FILTER_BILINEAR
} mach64_3d_texture_filter_t;

static inline uint64_t
mach64_3d_mip_abs64(int64_t value)
{
    return value < 0 ? (uint64_t) (-value) : (uint64_t) value;
}

/*
 * GT BILINEAR_TEX_EN controls magnification only; TEX_BLEND_FCN controls
 * filtering during minification.  ATI also documents the multipass case where
 * minification mode 2 or 3 combined with BILINEAR_TEX_EN=0 suppresses pixels
 * while magnifying instead of silently falling back to pick-nearest.
 */
static inline mach64_3d_texture_filter_t
mach64_3d_texture_filter(int minifying, unsigned tex_blend_fcn,
                         int bilinear_tex_en)
{
    tex_blend_fcn &= 3u;

    if (!minifying) {
        if (!bilinear_tex_en &&
            (tex_blend_fcn == 2u || tex_blend_fcn == 3u))
            return MACH64_3D_TEXTURE_FILTER_NONE;
        return bilinear_tex_en ? MACH64_3D_TEXTURE_FILTER_BILINEAR :
                                 MACH64_3D_TEXTURE_FILTER_NEAREST;
    }

    return (tex_blend_fcn == 2u || tex_blend_fcn == 3u) ?
           MACH64_3D_TEXTURE_FILTER_BILINEAR :
           MACH64_3D_TEXTURE_FILTER_NEAREST;
}

/*
 * Convert the normalized footprint within one power-of-two mip interval to
 * the 8-bit logarithmic LOD fraction used for map selection/blending.  The
 * input bucket represents 1.0 + bucket/256.0.  A table keeps the per-pixel
 * path deterministic and avoids floating-point log2() calls.
 */
static inline int
mach64_3d_mip_log_fraction(unsigned bucket)
{
    static const uint8_t log2_fraction[256] = {
        0, 1, 3, 4, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 20, 21,
        22, 24, 25, 26, 28, 29, 30, 32, 33, 34, 36, 37, 38, 39, 41, 42,
        43, 45, 46, 47, 48, 50, 51, 52, 53, 55, 56, 57, 58, 60, 61, 62,
        63, 64, 66, 67, 68, 69, 70, 72, 73, 74, 75, 76, 77, 79, 80, 81,
        82, 83, 84, 86, 87, 88, 89, 90, 91, 92, 93, 95, 96, 97, 98, 99,
        100, 101, 102, 103, 104, 105, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116,
        117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 132, 133,
        134, 135, 136, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148,
        149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 160, 161, 162, 163,
        164, 165, 166, 167, 168, 169, 170, 171, 171, 172, 173, 174, 175, 176, 177, 178,
        179, 179, 180, 181, 182, 183, 184, 185, 186, 186, 187, 188, 189, 190, 191, 192,
        192, 193, 194, 195, 196, 197, 198, 198, 199, 200, 201, 202, 203, 203, 204, 205,
        206, 207, 208, 208, 209, 210, 211, 212, 212, 213, 214, 215, 216, 216, 217, 218,
        219, 220, 220, 221, 222, 223, 224, 224, 225, 226, 227, 227, 228, 229, 230, 230,
        231, 232, 233, 234, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243,
        243, 244, 245, 246, 246, 247, 248, 248, 249, 250, 251, 251, 252, 253, 254, 254,
    };

    if (bucket > 255u)
        bucket = 255u;
    return log2_fraction[bucket];
}

/*
 * GT/GTB selects a mip map from the S/T address derivatives.  coord_shift is
 * the raw normalized-coordinate shift for one texel in the largest map.
 *
 * The integer level is selected from the power-of-two footprint interval.
 * The fractional level is logarithmic within that interval so equal distances
 * in mip level (rather than equal raw footprint deltas) receive equal blend
 * weights.  nearest_lod therefore switches at a half mip level.
 */
static inline mach64_3d_mip_lod_t
mach64_3d_mip_lod(int64_t dsdx, int64_t dtdx,
                  int64_t dsdy, int64_t dtdy,
                  int coord_shift, int max_lod)
{
    mach64_3d_mip_lod_t result = { 0, 0, 0, 0 };
    uint64_t rho = mach64_3d_mip_abs64(dsdx);
    uint64_t value;
    uint64_t scale;
    int scale_shift;

    value = mach64_3d_mip_abs64(dtdx);
    if (value > rho)
        rho = value;
    value = mach64_3d_mip_abs64(dsdy);
    if (value > rho)
        rho = value;
    value = mach64_3d_mip_abs64(dtdy);
    if (value > rho)
        rho = value;

    if (coord_shift < 0)
        coord_shift = 0;
    if (coord_shift > 62)
        coord_shift = 62;
    if (max_lod < 0)
        max_lod = 0;

    scale_shift = coord_shift;
    scale = UINT64_C(1) << scale_shift;
    if (rho <= scale)
        return result;

    result.minifying = 1;
    while (result.floor_lod < max_lod &&
           scale <= UINT64_MAX / 2 && rho >= scale * 2) {
        scale *= 2;
        scale_shift++;
        result.floor_lod++;
    }

    if (result.floor_lod < max_lod && rho > scale) {
        uint64_t delta = rho - scale;
        uint64_t bucket;

        /*
         * scale is a power of two.  Normalize delta/scale to eight bits
         * without delta*256, which could overflow for large coordinate
         * shifts.  Round when discarding low bits.
         */
        if (scale_shift > 8) {
            unsigned shift = (unsigned) (scale_shift - 8);
            uint64_t round = UINT64_C(1) << (shift - 1);
            bucket = (delta + round) >> shift;
        } else if (scale_shift == 8) {
            bucket = delta;
        } else {
            bucket = delta << (8 - scale_shift);
        }

        if (bucket > 255)
            bucket = 255;
        result.fraction = mach64_3d_mip_log_fraction((unsigned) bucket);
    }

    result.nearest_lod = result.floor_lod;
    if (result.fraction >= 128 && result.nearest_lod < max_lod)
        result.nearest_lod++;
    return result;
}

#endif
