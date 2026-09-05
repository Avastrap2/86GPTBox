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
check_filter(int minifying, unsigned blend, int bilinear,
             mach64_3d_texture_filter_t expected)
{
    return mach64_3d_texture_filter(minifying, blend, bilinear) == expected;
}

static int
check_lowest_level(const uint32_t offsets[11], int largest,
                   int expected_lowest)
{
    return mach64_3d_mip_lowest_populated_level(offsets, largest) ==
           expected_lowest;
}

int
main(void)
{
    const int64_t texel = INT64_C(1) << 20;
    const uint32_t complete_chain[11] = {
        0x1180, 0x1160, 0x1120, 0x10a0, 0x1000, 0x0800, 0x0000
    };
    const uint32_t final_reality_chain[11] = {
        0x2151c0, 0x2151c0, 0x2151c0, 0x2151c0,
        0x2151c0, 0x214980, 0x212940
    };

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

    /* Final Reality uploads 64x64, 32x32, and 16x16 maps, then repeats the
     * 16x16 byte pointer for nominally smaller levels. */
    if (!check_lowest_level(complete_chain, 6, 0))
        return 1;
    if (!check_lowest_level(final_reality_chain, 6, 4))
        return 1;
    {
        mach64_3d_mip_lod_t lod = mach64_3d_mip_lod(
            texel * 128, 0, 0, 0, 20, 2);
        if (!lod.minifying || lod.floor_lod != 2 || lod.nearest_lod != 2)
            return 1;
    }

#define CHECK_FILTER(...) do { if (!check_filter(__VA_ARGS__)) return 1; } while (0)
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
