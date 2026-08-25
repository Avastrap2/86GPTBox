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

    /* S.10.16 is sign + 10 integer + 16 fraction = 27 physical bits. */
    CHECK(mach64_3d_s10_16_decode(UINT32_C(0x024119b0)) == 37820848);
    CHECK(mach64_3d_s10_16_decode(UINT32_C(0x0489c338)) == -58080456);
    CHECK(mach64_3d_s10_16_decode(UINT32_C(0x04000000)) ==
          -(INT32_C(1) << 26));

    /* S.11.16 is sign + 11 integer + 16 fraction = 28 physical bits. */
    CHECK(mach64_3d_s11_16_decode(UINT32_C(0xf9a69d88)) == -106521208);
    CHECK(mach64_3d_s11_16_decode(UINT32_C(0x0aa4ac18)) == -89871336);
    CHECK(mach64_3d_s11_16_decode(UINT32_C(0x08000000)) ==
          -(INT32_C(1) << 27));

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

    CHECK(mach64_3d_s10_16_encode(-146) == UINT32_C(0x07ffff6e));
    CHECK(mach64_3d_s11_16_encode(-146) == UINT32_C(0x0fffff6e));
    CHECK(mach64_3d_s8_12_encode(-16) == UINT32_C(0x01fffff0));
    CHECK(mach64_3d_s16_12_encode(-1) == UINT32_C(0x1fffffff));
    return 0;
}
