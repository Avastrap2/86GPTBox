#include <stdint.h>

#include "../../src/video/vid_ati_mach64_3d_mipmap.h"

static int
check_lod(int64_t dsdx, int64_t dtdx, int64_t dsdy, int64_t dtdy,
          int expected_minifying, int expected_floor, int expected_nearest)
{
    mach64_3d_mip_lod_t lod =
        mach64_3d_mip_lod(dsdx, dtdx, dsdy, dtdy, 20, 6);

    return lod.minifying == expected_minifying &&
           lod.floor_lod == expected_floor &&
           lod.nearest_lod == expected_nearest &&
           lod.fraction >= 0 && lod.fraction <= 255;
}

static int
check_lod_fraction(int64_t rho, int expected_floor, int expected_nearest,
                   int expected_fraction)
{
    mach64_3d_mip_lod_t lod =
        mach64_3d_mip_lod(rho, 0, 0, 0, 20, 6);

    return lod.minifying == 1 &&
           lod.floor_lod == expected_floor &&
           lod.nearest_lod == expected_nearest &&
           lod.fraction == expected_fraction;
}

static int
check_filter(int minifying, unsigned blend, int bilinear,
             mach64_3d_texture_filter_t expected)
{
    return mach64_3d_texture_filter(minifying, blend, bilinear) == expected;
}

int
main(void)
{
    const int64_t texel = INT64_C(1) << 20;

#define CHECK_LOD(...) do { if (!check_lod(__VA_ARGS__)) return 1; } while (0)
    CHECK_LOD(texel / 4, 0, 0, 0, 0, 0, 0);
    CHECK_LOD(texel, 0, 0, 0, 0, 0, 0);
    CHECK_LOD(texel + texel / 4, 0, 0, 0, 1, 0, 0);
    CHECK_LOD(texel + texel / 2, 0, 0, 0, 1, 0, 1);
    CHECK_LOD(texel * 2, 0, 0, 0, 1, 1, 1);
    CHECK_LOD(texel * 3, 0, 0, 0, 1, 1, 2);
    CHECK_LOD(texel * 8, 0, 0, 0, 1, 3, 3);
    CHECK_LOD(0, 0, -(texel * 8), 0, 1, 3, 3);
    CHECK_LOD(texel * 128, 0, 0, 0, 1, 6, 6);
#undef CHECK_LOD

#define CHECK_FRACTION(...) do { if (!check_lod_fraction(__VA_ARGS__)) return 2; } while (0)
    /* LOD fractions are log2 distances within a power-of-two interval. */
    CHECK_FRACTION(texel + texel / 4, 0, 0, 82);
    CHECK_FRACTION(texel + texel / 2, 0, 1, 149);
    CHECK_FRACTION(texel + (texel * 3) / 4, 0, 1, 206);
    CHECK_FRACTION(texel * 2 + texel / 2, 1, 1, 82);
    CHECK_FRACTION(texel * 3, 1, 2, 149);

    /* Around sqrt(2), nearest-map selection flips at half a mip level. */
    CHECK_FRACTION((texel * 181) / 128, 0, 0, 127);
    CHECK_FRACTION((texel * 182) / 128, 0, 1, 129);
#undef CHECK_FRACTION

#define CHECK_FILTER(...) do { if (!check_filter(__VA_ARGS__)) return 3; } while (0)
    /* Magnification follows BILINEAR_TEX_EN, except ATI's documented
     * multipass suppression for the two 2x2 minification modes. */
    CHECK_FILTER(0, 0, 0, MACH64_3D_TEXTURE_FILTER_NEAREST);
    CHECK_FILTER(0, 0, 1, MACH64_3D_TEXTURE_FILTER_BILINEAR);
    CHECK_FILTER(0, 2, 0, MACH64_3D_TEXTURE_FILTER_NONE);
    CHECK_FILTER(0, 3, 0, MACH64_3D_TEXTURE_FILTER_NONE);
    CHECK_FILTER(0, 2, 1, MACH64_3D_TEXTURE_FILTER_BILINEAR);

    /* Minification ignores BILINEAR_TEX_EN: TEX_BLEND_FCN selects whether
     * the chosen map is sampled nearest or with a 2x2 blend. */
    CHECK_FILTER(1, 0, 1, MACH64_3D_TEXTURE_FILTER_NEAREST);
    CHECK_FILTER(1, 1, 1, MACH64_3D_TEXTURE_FILTER_NEAREST);
    CHECK_FILTER(1, 2, 0, MACH64_3D_TEXTURE_FILTER_BILINEAR);
    CHECK_FILTER(1, 3, 0, MACH64_3D_TEXTURE_FILTER_BILINEAR);
#undef CHECK_FILTER

    return 0;
}
