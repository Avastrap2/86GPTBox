#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_reg_fields.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "fail line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int
main(void)
{
    /* Real driver/log style values: high reserved bits must not affect fields. */
    CHECK(mach64_3d_u10_11_decode(UINT32_C(0xfc08c853)) ==
          (int32_t) UINT32_C(0x0008c840));
    CHECK(mach64_3d_u10_11_decode(UINT32_C(0x03f98ab4)) ==
          (int32_t) UINT32_C(0x03f98aa0));

    CHECK(mach64_3d_s10_16_decode(UINT32_C(0xffffff6e)) == -146);
    CHECK(mach64_3d_s10_16_decode(UINT32_C(0xfffffdb8)) == -584);
    CHECK(mach64_3d_s11_16_decode(UINT32_C(0xffdb3460)) == -2411424);

    /* S.8.12 lives at bits 24:4: high reserved and low four bits vanish. */
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0x00ff000f)) ==
          (int32_t) UINT32_C(0x00ff0000));
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0xfe000010)) == 16);
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0x01fffff0)) == -16);

    /* Z signs from bit 28, not bit 31. */
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0xffffffff)) == -1);
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0x10000000)) ==
          -(INT32_C(1) << 28));
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0xe0001000)) == 4096);

    CHECK(mach64_3d_s10_16_encode(-146) == UINT32_C(0x03ffff6e));
    CHECK(mach64_3d_s11_16_encode(-146) == UINT32_C(0x07ffff6e));
    CHECK(mach64_3d_s8_12_encode(-16) == UINT32_C(0x01fffff0));
    CHECK(mach64_3d_s16_12_encode(-1) == UINT32_C(0x1fffffff));
    return 0;
}
