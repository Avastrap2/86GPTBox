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
    uint32_t ci4_low;
    uint32_t ci4_high;

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

    /*
     * ATI documents CI4 as CI8 lookup with a selected nibble and a 4-bit
     * palette bank in DP_CI4_RGB_INDEX.  For bank A and byte 0x3c, low-nibble
     * mode selects AC and high-nibble mode selects A3.
     */
    ci4_low = (0xau << MACH64_DP_CI4_RGB_INDEX_SHIFT) |
              MACH64_DP_CI4_RGB_LOW_NIBBLE;
    ci4_high = (0xau << MACH64_DP_CI4_RGB_INDEX_SHIFT) |
               MACH64_DP_CI4_RGB_HIGH_NIBBLE;
    expect_u32("CI4 low nibble bank",
               mach64_3d_texture_palette_texel_index(ci4_low, 0x3cu), 0xacu);
    expect_u32("CI4 high nibble bank",
               mach64_3d_texture_palette_texel_index(ci4_high, 0x3cu), 0xa3u);
    expect_u32("CI8 full byte",
               mach64_3d_texture_palette_texel_index(0, 0x3cu), 0x3cu);
    expect_u32("invalid dual CI4 selector stays CI8",
               mach64_3d_texture_palette_texel_index(
                   MACH64_DP_CI4_RGB_LOW_NIBBLE |
                   MACH64_DP_CI4_RGB_HIGH_NIBBLE,
                   0x3cu),
               0x3cu);

    if (failures)
        return 1;
    puts("Mach64 CI8/CI4 texture palette tests passed");
    return 0;
}
