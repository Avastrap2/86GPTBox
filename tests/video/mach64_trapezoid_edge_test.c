#include <assert.h>

#include "../../src/video/vid_ati_mach64_3d_trapezoid.h"

int
main(void)
{
    int first_a;
    int end_a;
    int first_b;
    int end_b;

    /* A GT/GTB trapezoid owns [left, right), so adjacent spans meet without
     * sharing the boundary pixel. */
    assert(mach64_3d_trapezoid_clip_span(10, 20, 0, 100,
                                         &first_a, &end_a));
    assert(first_a == 10);
    assert(end_a == 20);

    assert(mach64_3d_trapezoid_clip_span(20, 30, 0, 100,
                                         &first_b, &end_b));
    assert(first_b == 20);
    assert(end_b == 30);
    assert(end_a == first_b);

    /* The scissor rectangle itself remains inclusive at SC_RIGHT. */
    assert(mach64_3d_trapezoid_clip_span(10, 20, 0, 15,
                                         &first_a, &end_a));
    assert(first_a == 10);
    assert(end_a == 16);

    /* A zero-width edge pair has no owned pixel. */
    assert(!mach64_3d_trapezoid_clip_span(10, 10, 0, 100,
                                          &first_a, &end_a));

    return 0;
}
