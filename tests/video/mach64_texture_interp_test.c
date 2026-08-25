#include <assert.h>
#include <stdint.h>

#include "../../src/video/vid_ati_mach64_3d_interp.h"

int
main(void)
{
    int64_t value = 1000;
    int64_t x_inc = 100;
    int64_t y_inc = 200;

    /* ATI documents XY_INC2 as changing X_INC on a Y step. */
    mach64_3d_tex_interp_step_y(&value, &x_inc, &y_inc, 20, 7);
    assert(value == 1200);
    assert(y_inc == 220);
    assert(x_inc == 107);

    /* An X step changes the coordinate and X_INC only.  It must not feed the
     * mixed derivative back into Y_INC. */
    mach64_3d_tex_interp_step_x(&value, &x_inc, 3);
    assert(value == 1307);
    assert(x_inc == 110);
    assert(y_inc == 220);

    return 0;
}
