#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_data_path.h"

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

static void
expect_int(const char *name, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n",
                name, actual, expected);
        failures++;
    }
}

int
main(void)
{
    const uint32_t sentinel = 0xf83ef83eu;
    const uint32_t foreground = 0x12345678u;
    const uint32_t filtered_texel = 0x00abcdefu;
    const uint32_t compare_mask = 0x7fff7fffu;

    /* ATI's first repair pass uses DP_SRC=0x00000005: the always-one
     * monochrome selector chooses DP_FRGD_SRC=0, which is DP_BKGD_CLR. */
    expect_u32("repair first-pass sentinel",
               mach64_3d_dp_select_source(
                   0x00000005u, sentinel, foreground, filtered_texel),
               sentinel);

    /* The second pass uses DP_SRC=0x00000505 and restores Scaler/3D data. */
    expect_u32("repair second-pass texel",
               mach64_3d_dp_select_source(
                   0x00000505u, sentinel, foreground, filtered_texel),
               filtered_texel);
    expect_u32("foreground-color source",
               mach64_3d_dp_select_source(
                   0x00000105u, sentinel, foreground, filtered_texel),
               foreground);
    expect_u32("nontrivial mono source remains 3D",
               mach64_3d_dp_select_source(
                   0x00010505u, sentinel, foreground, filtered_texel),
               filtered_texel);

    /* CLR_CMP_CNTL=4 inhibits pixels whose existing destination is not the
     * first-pass sentinel, leaving the filtered second pass inside the mask. */
    expect_int("sentinel permits second pass",
               mach64_3d_destination_compare_inhibits(
                   0x00000004u, sentinel, compare_mask, sentinel, 0xffffu),
               0);
    expect_int("other destination inhibits second pass",
               mach64_3d_destination_compare_inhibits(
                   0x00000004u, sentinel, compare_mask, 0x001fu, 0xffffu),
               1);
    expect_int("ARGB1555 alpha bits are masked",
               mach64_3d_destination_compare_inhibits(
                   0x00000004u, sentinel, compare_mask, 0x783eu, 0xffffu),
               0);
    expect_int("equality function inhibits sentinel",
               mach64_3d_destination_compare_inhibits(
                   0x00000005u, sentinel, compare_mask, sentinel, 0xffffu),
               1);
    expect_int("texel selector is not a destination compare",
               mach64_3d_destination_compare_inhibits(
                   0x02000004u, sentinel, compare_mask, 0x001fu, 0xffffu),
               0);
    expect_int("disabled compare avoids destination read",
               mach64_3d_destination_compare_enabled(0x00000000u), 0);
    expect_int("destination inequality compare enabled",
               mach64_3d_destination_compare_enabled(0x00000004u), 1);
    expect_int("texel compare is not destination compare",
               mach64_3d_destination_compare_enabled(0x02000004u), 0);

    if (failures)
        return 1;
    puts("Mach64 3D data-path tests passed");
    return 0;
}
