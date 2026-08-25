#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_control.h"
#include "../../src/video/vid_ati_mach64_3d_expand.h"
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

    /* Pseudo-color texels are keyed on the low-order source index, not the
     * palette-expanded RGB value. */
    expect_int("CI8 indexed equality",
               mach64_3d_texel_key_match_index(
                   equality, 0x0000007cu, 0x000000ffu, 0x7c), 1);
    expect_int("CI8 indexed mismatch",
               mach64_3d_texel_key_match_index(
                   equality, 0x0000007cu, 0x000000ffu, 0x3c), 0);
    expect_int("CI8 masked key",
               mach64_3d_texel_key_match_index(
                   equality, 0xdead007cu, 0x000000ffu, 0x7c), 1);

    /* NEAREST_TEX_VIS selects the nearest contributor instead of the OR of
     * all contributors participating in a texture filter. */
    expect_int("all contributors inhibit",
               mach64_3d_texel_visibility_inhibits(0, 0, 1), 1);
    expect_int("nearest contributor visible",
               mach64_3d_texel_visibility_inhibits(1, 0, 1), 0);
    expect_int("nearest contributor inhibits",
               mach64_3d_texel_visibility_inhibits(1, 1, 1), 1);

    /* SCALE_PIX_EXPAND=0 zero-extends source components.  Dynamic correction
     * repeats source bits into the low positions used by the 24-bit pipeline. */
    expect_int("RGB555 zero max",
               mach64_3d_expand_component(31, 5, 0), 0xf8);
    expect_int("RGB555 dynamic max",
               mach64_3d_expand_component(31, 5, 1), 0xff);
    expect_int("RGB555 dynamic pattern",
               mach64_3d_expand_component(3, 5, 1), 0x18);
    expect_int("RGB565 green zero max",
               mach64_3d_expand_component(63, 6, 0), 0xfc);
    expect_int("RGB565 green dynamic max",
               mach64_3d_expand_component(63, 6, 1), 0xff);
    expect_int("RGB332 red dynamic pattern",
               mach64_3d_expand_component(2, 3, 1), 0x49);
    expect_int("RGB332 blue zero max",
               mach64_3d_expand_component(3, 2, 0), 0xc0);
    expect_int("RGB332 blue dynamic max",
               mach64_3d_expand_component(3, 2, 1), 0xff);
    expect_int("ARGB4444 dynamic pattern",
               mach64_3d_expand_component(10, 4, 1), 0xaa);

    /* RED_DITHER_MAX reserves the top 32 RGB8 palette entries only when
     * dithering is active. */
    expect_int("RGB8 red unrestricted",
               mach64_3d_rgb8_red_code(7, 1, 0), 7);
    expect_int("RGB8 red dither max",
               mach64_3d_rgb8_red_code(7, 1, 1), 6);
    expect_int("RGB8 red max ignored without dither",
               mach64_3d_rgb8_red_code(7, 0, 1), 7);

    /* TEX_BLEND_FCN=3 supplies mip-distance alpha specifically for the
     * alpha-blending pipeline. */
    expect_int("multipass LOD alpha",
               mach64_3d_uses_lod_alpha(1, 3), 1);
    expect_int("no LOD alpha without blending",
               mach64_3d_uses_lod_alpha(0, 3), 0);
    expect_int("no LOD alpha for nearest-map mode",
               mach64_3d_uses_lod_alpha(1, 2), 0);

    if (failures)
        return 1;
    puts("Mach64 texture/control tests passed");
    return 0;
}
