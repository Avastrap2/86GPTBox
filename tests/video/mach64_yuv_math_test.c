#include <stdint.h>
#include <stdio.h>

#include "../../src/video/vid_ati_mach64_3d_yuv_math.h"

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
expect_rgb(const char *name, mach64_yuv_rgb_t actual,
           int r, int g, int b)
{
    if (actual.r != r || actual.g != g || actual.b != b) {
        fprintf(stderr, "%s: expected %d,%d,%d got %u,%u,%u\n",
                name, r, g, b, actual.r, actual.g, actual.b);
        failures++;
    }
}

int
main(void)
{
    const uint32_t yuyv = 0xDC963C28u; /* Y0=40 U=60 Y1=150 V=220 */

    expect_int("YUYV Y0", mach64_yuyv_y(yuyv, 0), 40);
    expect_int("YUYV Y1", mach64_yuyv_y(yuyv, 1), 150);
    expect_int("YUYV U", mach64_yuyv_u(yuyv), 60);
    expect_int("YUYV V", mach64_yuyv_v(yuyv), 220);

    expect_int("unsigned neutral U", mach64_yuv_chroma(0x80, 0), 0);
    expect_int("unsigned zero byte", mach64_yuv_chroma(0x00, 0), -128);
    expect_int("signed neutral U", mach64_yuv_chroma(0x00, 1), 0);
    expect_int("signed negative U", mach64_yuv_chroma(0x80, 1), -128);
    expect_int("signed positive U", mach64_yuv_chroma(0x7f, 1), 127);

    expect_rgb("unsigned neutral grey",
               mach64_yuv_to_rgb(100, 128, 128, 0), 100, 100, 100);
    expect_rgb("APPLE signed neutral grey",
               mach64_yuv_to_rgb(100, 0, 0, 1), 100, 100, 100);

    return failures ? 1 : 0;
}
