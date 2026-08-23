#ifndef VID_ATI_MACH64_3D_MIPMAP_H
#define VID_ATI_MACH64_3D_MIPMAP_H

#include <stdint.h>

typedef struct mach64_3d_mip_lod_t {
    int minifying;
    int floor_lod;
    int nearest_lod;
    int fraction;
} mach64_3d_mip_lod_t;

static inline uint64_t
mach64_3d_mip_abs64(int64_t value)
{
    return value < 0 ? (uint64_t) (-value) : (uint64_t) value;
}

/*
 * GT/GTB selects a mip map from the S/T address derivatives.  coord_shift is
 * the raw normalized-coordinate shift for one texel in the largest map.
 * fraction is the linear position between adjacent power-of-two footprints;
 * nearest_lod uses the log-space sqrt(2) boundary (106/256 of that interval).
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
