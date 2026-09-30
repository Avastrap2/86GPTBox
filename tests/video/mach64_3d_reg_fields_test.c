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

    /* Lead/trail Bresenham terms are signed 18-bit physical fields. */
    CHECK(mach64_3d_s18_decode(UINT32_C(0xffffffff)) == -1);
    CHECK(mach64_3d_s18_decode(UINT32_C(0x0001ffff)) == 131071);
    CHECK(mach64_3d_s18_decode(UINT32_C(0x00020000)) == -131072);
    CHECK(mach64_3d_s18_encode(-332) == UINT32_C(0x0003feb4));

    /* S.8.12 lives at bits 24:4: high reserved and low four bits vanish. */
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0x00ff000f)) ==
          (int32_t) UINT32_C(0x00ff0000));
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0xfe000010)) == 16);
    CHECK(mach64_3d_s8_12_decode(UINT32_C(0x01fffff0)) == -16);

    /* Color conversion retains the physical signed field, not an unsigned
     * 8-bit wrap or a clamp of the unbounded host interpolation sum. */
    CHECK(mach64_3d_s8_12_color(-16) == 0);
    CHECK(mach64_3d_s8_12_color(INT64_C(0x00ffffff)) == 255);
    CHECK(mach64_3d_s8_12_color(INT64_C(0x01000000)) == 0);
    CHECK(mach64_3d_s8_12_color(INT64_C(0x01ffffff)) == 0);
    CHECK(mach64_3d_s8_12_color(INT64_C(0x02000000)) == 0);
    CHECK(mach64_3d_s8_12_color((INT64_C(197) - 512) * 65536) == 197);
    CHECK(mach64_3d_s8_12_color((INT64_C(123) + 512) * 65536) == 123);
    CHECK(mach64_3d_s8_12_color(INT64_MIN) == 0);
    CHECK(mach64_3d_s8_12_color(INT64_MAX) == 0);
    static const int32_t periods[] = { -1073741824, -4, -1, 0, 1, 4, 1073741824 };
    static const uint32_t fractions[] = { 0, 1, 15, 16, 32768, 65535 };
    for (unsigned p = 0; p < sizeof(periods) / sizeof(periods[0]); p++) {
        for (unsigned integer = 0; integer < 512; integer++) {
            for (unsigned f = 0; f < sizeof(fractions) / sizeof(fractions[0]); f++) {
                int64_t value = (int64_t) periods[p] * INT64_C(0x02000000) +
                                (int64_t) integer * 65536 + fractions[f];
                int expected = integer < 256 ? (int) integer : 0;
                int32_t decoded = mach64_3d_s8_12_decode((uint32_t) value);
                CHECK(mach64_3d_s8_12_color(value) == expected);
                CHECK(mach64_3d_s8_12_color(value) == (decoded < 0 ? 0 : decoded / 65536));
            }
        }
    }

    /* Z signs from bit 28, not bit 31. */
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0xffffffff)) == -1);
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0x10000000)) ==
          -(INT32_C(1) << 28));
    CHECK(mach64_3d_s16_12_decode(UINT32_C(0xe0001000)) == 4096);

    CHECK(mach64_3d_s16_12_depth(-1) == 0);
    CHECK(mach64_3d_s16_12_depth(INT64_C(0x0fffffff)) == 65535);
    CHECK(mach64_3d_s16_12_depth(INT64_C(0x10000000)) == 0);
    CHECK(mach64_3d_s16_12_depth(INT64_C(0x1fffffff)) == 0);
    CHECK(mach64_3d_s16_12_depth((INT64_C(48443) + 131072) * 4096) == 48443);
    CHECK(mach64_3d_s16_12_depth((INT64_C(23823) - 131072) * 4096) == 23823);
    CHECK(mach64_3d_s16_12_depth(INT64_MIN) == 0);
    CHECK(mach64_3d_s16_12_depth(INT64_MAX) == 0);
    for (unsigned p = 0; p < sizeof(periods) / sizeof(periods[0]); p++) {
        for (unsigned integer = 0; integer < 131072; integer++) {
            for (unsigned fractional = 0; fractional <= 4095; fractional += 4095) {
                int64_t value = (int64_t) periods[p] * INT64_C(0x20000000) +
                                (int64_t) integer * 4096 + fractional;
                unsigned expected = integer < 65536 ? integer : 0;
                CHECK(mach64_3d_s16_12_depth(value) == expected);
            }
        }
    }

    CHECK(mach64_3d_s10_16_encode(-146) == UINT32_C(0x07ffff6e));
    CHECK(mach64_3d_s11_16_encode(-146) == UINT32_C(0x0fffff6e));
    CHECK(mach64_3d_s8_12_encode(-16) == UINT32_C(0x01fffff0));
    CHECK(mach64_3d_s16_12_encode(-1) == UINT32_C(0x1fffffff));
    return 0;
}
