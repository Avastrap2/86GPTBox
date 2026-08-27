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

/* ATI drivers duplicate the byte pointer of the smallest populated map into
 * lower TEX_n_OFFSET registers.  Follow the distinct offset chain downward
 * from the largest map and stop at the first duplicate.  Selecting a nominally
 * smaller level with the duplicated pointer would reinterpret the populated
 * map with a smaller row stride and sample unrelated texels. */
static inline int
mach64_3d_mip_lowest_populated_level(const uint32_t offsets[11],
                                     int largest_level)
{
    int level;

    if (!offsets)
        return 0;
    if (largest_level < 0)
        largest_level = 0;
    if (largest_level > 10)
        largest_level = 10;

    level = largest_level;
    while (level > 0 && offsets[level - 1] != offsets[level])
        level--;
    return level;
}

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
 * GT/GTB selects a mip map from the S/T address derivatives.  coord_shift is
 * the raw normalized-coordinate shift for one texel in the largest map.
 * fraction is the linear position between adjacent power-of-two footprints;
 * nearest_lod uses the log-space sqrt(2) boundary (106/256 of that interval).
 * max_lod is the number of distinct map transitions, not necessarily the log2
 * size of the largest map when the driver duplicates its smallest map offset.
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

    scale = UINT64_C(1) << coord_shift;
    if (rho <= scale)
        return result;

    result.minifying = 1;
    while (result.floor_lod < max_lod &&
           scale <= UINT64_MAX / 2 && rho >= scale * 2) {
        scale *= 2;
        result.floor_lod++;
    }

    if (result.floor_lod < max_lod && rho > scale) {
        uint64_t delta = rho - scale;
        result.fraction = (int) ((delta * 255 + scale / 2) / scale);
        if (result.fraction > 255)
            result.fraction = 255;
    }

    result.nearest_lod = result.floor_lod;
    if (result.fraction >= 106 && result.nearest_lod < max_lod)
        result.nearest_lod++;
    return result;
}

#endif
