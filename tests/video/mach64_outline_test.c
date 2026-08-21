#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_outline.h"

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
    expect_int("textured outline",
               mach64_3d_textured_outline(2, 0x0000e823u), 1);
    expect_int("textured solid",
               mach64_3d_textured_outline(2, 0x0000e827u), 0);
    expect_int("untextured command",
               mach64_3d_textured_outline(3, 0x0000e823u), 0);

    expect_int("outline left edge",
               mach64_3d_outline_span_pixel(1, 8, 10, 10, 20), 1);
    expect_int("outline interior",
               mach64_3d_outline_span_pixel(1, 8, 11, 10, 20), 0);
    expect_int("outline right edge",
               mach64_3d_outline_span_pixel(1, 8, 19, 10, 20), 1);
    expect_int("horizontal edge",
               mach64_3d_outline_span_pixel(1, 1, 15, 10, 20), 1);
    expect_int("solid interior",
               mach64_3d_outline_span_pixel(0, 8, 15, 10, 20), 1);
    expect_int("outside span",
               mach64_3d_outline_span_pixel(1, 8, 20, 10, 20), 0);

    if (failures)
        return 1;
    puts("Mach64 textured outline tests passed");
    return 0;
}
