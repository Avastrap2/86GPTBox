#ifndef VID_ATI_MACH64_3D_INTERP_H
#define VID_ATI_MACH64_3D_INTERP_H

#include <stdint.h>

/*
 * GT/GTB texture-interpolator edge stepping follows the register definitions:
 *
 * - *_Y_INC changes the coordinate when the leading edge steps in Y.
 * - *_Y_INC2 changes *_Y_INC on a Y step.
 * - *_XY_INC2 changes *_X_INC on a Y step.
 * - *_X_INC2 changes *_X_INC on an X step.
 *
 * The RRG does not define *_XY_INC2 as changing *_Y_INC on an X step.  Keep
 * that asymmetry explicit here instead of imposing mathematical mixed-partial
 * symmetry that the fixed-function hardware does not document.
 */
static inline void
mach64_3d_tex_interp_step_y(int64_t *value, int64_t *x_inc,
                            int64_t *y_inc, int64_t y_inc2,
                            int64_t xy_inc2)
{
    *value += *y_inc;
    *y_inc += y_inc2;
    *x_inc += xy_inc2;
}

static inline void
mach64_3d_tex_interp_step_x(int64_t *value, int64_t *x_inc,
                            int64_t x_inc2)
{
    *value += *x_inc;
    *x_inc += x_inc2;
}

#endif /* VID_ATI_MACH64_3D_INTERP_H */
