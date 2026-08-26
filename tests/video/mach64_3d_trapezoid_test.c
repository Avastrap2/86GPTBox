#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_trapezoid.h"

static int failures;

static void
expect_span(const char *name, int lead, int trail, int fill_lr,
            int sc_left, int sc_right,
            int expected_valid, int expected_first, int expected_end)
{
    int first = -1;
    int end = -1;
    int valid = mach64_3d_trapezoid_clip_span(
        lead, trail, fill_lr, sc_left, sc_right, &first, &end);

    if (valid != expected_valid ||
        (valid && (first != expected_first || end != expected_end))) {
        fprintf(stderr,
                "%s: got valid=%d span=[%d,%d), expected valid=%d span=[%d,%d)\n",
                name, valid, first, end,
                expected_valid, expected_first, expected_end);
        failures++;
    }
}

int
main(void)
{
    /* Leading is included and trailing is excluded in either fill direction. */
    expect_span("left-to-right ownership", 10, 13, 1,
                0, 31, 1, 10, 13);
    expect_span("right-to-left ownership", 13, 10, 0,
                0, 31, 1, 11, 14);

    /* Adjacent lead/lead boundaries retain both neighboring pixels. */
    expect_span("left triangle leading edge", 95, 86, 0,
                0, 127, 1, 87, 96);
    expect_span("right triangle leading edge", 96, 106, 1,
                0, 127, 1, 96, 106);

    /* An identical trailing/leading shared edge has exactly one owner. */
    expect_span("left triangle trailing edge", 1, 21, 1,
                0, 127, 1, 1, 21);
    expect_span("right triangle leading edge", 21, 54, 1,
                0, 127, 1, 21, 54);

    /* A collapsed lead/trail pair has zero horizontal coverage. */
    expect_span("collapsed span", 10, 10, 1,
                0, 31, 0, 0, 0);
    expect_span("collapsed reverse span", 10, 10, 0,
                0, 31, 0, 0, 0);

    /* Scissor coordinates remain inclusive. */
    expect_span("left-to-right clipped", 5, 40, 1,
                8, 31, 1, 8, 32);
    expect_span("right-to-left clipped", 40, 5, 0,
                8, 31, 1, 8, 32);
    expect_span("crossed left-to-right", 13, 10, 1,
                0, 31, 0, 0, 0);
    expect_span("crossed right-to-left", 10, 13, 0,
                0, 31, 0, 0, 0);

    if (failures)
        return 1;
    puts("Mach64 3D trapezoid edge-ownership tests passed");
    return 0;
}
