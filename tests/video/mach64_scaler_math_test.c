/* Mach64 front-end scaler fixed-point and mix regression tests. */

#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_scaler_math.h"

static int failures;

static void
expect_int(const char *name, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        failures++;
    }
}

static void
expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %08x, got %08x\n",
                name, expected, actual);
        failures++;
    }
}

int
main(void)
{
    static const int expected_2x[8] = { 0, 0, 1, 1, 2, 2, 3, 3 };
    static const uint32_t quadrant_colors[4] = {
        0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu
    };

    for (int destination = 0; destination < 8; destination++) {
        int source = mach64_scaler_accum_pixel(destination * 0x8000);
        expect_int("2x replicated source coordinate", source,
                   expected_2x[destination]);
    }

    expect_int("negative accumulator floor",
               mach64_scaler_accum_pixel(-1), -1);
    expect_int("one-to-one accumulator",
               mach64_scaler_accum_pixel(7 * 0x10000), 7);
    expect_int("half-pixel coefficient",
               (int) mach64_scaler_accum_fraction5(0x8000), 16);
    expect_int("half-pixel blend",
               mach64_scaler_lerp5(0, 255, 16), 128);

    /* Exact geometry used by the DDTEST reproduction: 32x32 to 64x64. */
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int source_x = mach64_scaler_accum_pixel(x * 0x8000);
            int source_y = mach64_scaler_accum_pixel(y * 0x8000);
            unsigned source_quadrant = (source_x >= 16 ? 1u : 0u) |
                                       (source_y >= 16 ? 2u : 0u);
            unsigned expected_quadrant = (x >= 32 ? 1u : 0u) |
                                         (y >= 32 ? 2u : 0u);

            if (quadrant_colors[source_quadrant] !=
                quadrant_colors[expected_quadrant]) {
                fprintf(stderr, "32x32 to 64x64 quadrant mismatch at %d,%d\n",
                        x, y);
                failures++;
            }
        }
    }

    expect_u32("SRCCOPY", mach64_scaler_mix(0x00ff00ffu, 0x000000ffu, 7),
               0x00ff00ffu);
    expect_u32("SRCINVERT", mach64_scaler_mix(0x00ff00ffu, 0x000000ffu, 5),
               0x00ff0000u);
    expect_u32("SRCAND", mach64_scaler_mix(0x00ff00ffu, 0x000000ffu, 12),
               0x000000ffu);
    expect_u32("BLACKNESS", mach64_scaler_mix(0xffffffffu, 0xffffffffu, 1),
               0x00000000u);
    expect_u32("WHITENESS", mach64_scaler_mix(0, 0, 2), 0xffffffffu);

    return failures ? 1 : 0;
}
