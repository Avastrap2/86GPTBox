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

    return 0;
}
