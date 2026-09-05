#ifndef VID_ATI_MACH64_3D_DATA_PATH_H
#define VID_ATI_MACH64_3D_DATA_PATH_H

#include <stdint.h>

#define MACH64_3D_DP_SRC_BKGD_CLR 0u
#define MACH64_3D_DP_SRC_FRGD_CLR 1u
#define MACH64_3D_DP_SRC_SCALE_3D 5u

/* DP_MONO_SRC=0 supplies an always-one selector, so DP_FRGD_SRC chooses the
 * color entering the data path.  ATI's GT alpha-mask repair depends on
 * switching that source from DP_BKGD_CLR (first pass) to Scaler/3D data
 * (second pass).  Preserve the generated 3D color for source types that the
 * software renderer does not expose independently. */
static inline uint32_t
mach64_3d_dp_select_source(uint32_t dp_src, uint32_t background_color,
                           uint32_t foreground_color, uint32_t scale_3d_color)
{
    unsigned mono_source = (dp_src >> 16) & 3u;
    unsigned foreground_source = (dp_src >> 8) & 7u;

    if (mono_source != 0u)
        return scale_3d_color;

    switch (foreground_source) {
        case MACH64_3D_DP_SRC_BKGD_CLR:
            return background_color;
        case MACH64_3D_DP_SRC_FRGD_CLR:
            return foreground_color;
        case MACH64_3D_DP_SRC_SCALE_3D:
        default:
            return scale_3d_color;
    }
}

static inline int
mach64_3d_color_compare_inhibits(uint32_t cntl, uint32_t key, uint32_t mask,
                                 uint32_t selected)
{
    uint32_t reference = key & mask;

    selected &= mask;
    switch (cntl & 7u) {
        case 1:
            return 1;
        case 4:
            return selected != reference;
        case 5:
            return selected == reference;
        default:
            return 0;
    }
}

/* CLR_CMP_SRC=0 compares the packed destination value.  A true result keeps
 * the old destination, which is equivalent to inhibiting this color write. */
static inline int
mach64_3d_destination_compare_enabled(uint32_t cntl)
{
    unsigned function = cntl & 7u;

    return (((cntl >> 24) & 3u) == 0u) &&
           (function == 1u || function == 4u || function == 5u);
}

static inline int
mach64_3d_destination_compare_inhibits(uint32_t cntl, uint32_t key,
                                       uint32_t mask, uint32_t destination,
                                       uint32_t pixel_mask)
{
    if (!mach64_3d_destination_compare_enabled(cntl))
        return 0;

    return mach64_3d_color_compare_inhibits(
        cntl, key & pixel_mask, mask & pixel_mask,
        destination & pixel_mask);
}

#endif
