#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_texture_palette.h"

static int failures;

static void
expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %08x, expected %08x\n",
                name, actual, expected);
        failures++;
    }
}

int
main(void)
{
    uint32_t palette[MACH64_3D_TEXTURE_PALETTE_ENTRIES] = { 0 };
    uint32_t rgb;

    /* Values captured from the Windows 95 Direct3D HAL Globe upload. */
    mach64_3d_texture_palette_store(palette, 0x30bda54au);
    mach64_3d_texture_palette_store(palette, 0x01181821u);

    expect_u32("entry 0x30", palette[0x30], 0x00bda54au);
    expect_u32("entry 0x01", palette[0x01], 0x00181821u);
    expect_u32("unwritten entry", palette[0x02], 0);

    rgb = mach64_3d_texture_palette_lookup(palette, 0x30);
    expect_u32("red", mach64_3d_texture_palette_red(rgb), 0xbdu);
    expect_u32("green", mach64_3d_texture_palette_green(rgb), 0xa5u);
    expect_u32("blue", mach64_3d_texture_palette_blue(rgb), 0x4au);

    if (failures)
        return 1;
    puts("Mach64 CI8 texture palette tests passed");
    return 0;
}
