#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_tex_key.h"

static int failures;

static void
expect_int(const char *name, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n", name, actual, expected);
        failures++;
    }
}

int
main(void)
{
    const uint32_t equality = 0x02000005u;
    const uint32_t key = 0x00f800f8u;
    const uint32_t mask = 0x00f8fcf8u;

    /* Captured HAL values: expanded RGB565 magenta masks to the key. */
    expect_int("captured magenta equality",
               mach64_3d_texel_key_match(
                   equality, key, mask, 0xff, 0x00, 0xff), 1);
    expect_int("green is not the key",
               mach64_3d_texel_key_match(
                   equality, key, mask, 0x00, 0xff, 0x00), 0);
    expect_int("2D source selector is ignored",
               mach64_3d_texel_key_match(
                   0x01000005u, key, mask, 0xff, 0x00, 0xff), 0);
    expect_int("texel inequality",
               mach64_3d_texel_key_match(
                   0x02000004u, key, mask, 0x00, 0xff, 0x00), 1);
    expect_int("disabled comparator",
               mach64_3d_texel_key_match(
                   0x02000000u, key, mask, 0xff, 0x00, 0xff), 0);

    if (failures)
        return 1;
    puts("Mach64 texture color-key tests passed");
    return 0;
}
