#ifndef VID_ATI_MACH64_3D_OUTLINE_H
#define VID_ATI_MACH64_3D_OUTLINE_H

#include <stdint.h>

#define MACH64_3D_DST_Y_MAJOR 0x04u

/*
 * The Windows 9x ATI HAL clears DST_Y_MAJOR on texture-mapped DRAW_TRAP
 * commands used for D3DFILL_WIREFRAME, while filled textured triangles keep
 * it set.  Preserve the leading and trailing edge pixels on multi-scanline
 * trapezoids.  A one-scanline trapezoid is a horizontal edge, so its complete
 * span belongs to the outline.
 */
static inline int
mach64_3d_textured_outline(unsigned fcn, uint32_t dst_cntl)
{
    return fcn == 2u && !(dst_cntl & MACH64_3D_DST_Y_MAJOR);
}

static inline int
mach64_3d_outline_span_pixel(int outline, int trap_length, int x,
                             int left, int right_exclusive)
{
    if (x < left || x >= right_exclusive)
        return 0;
    if (!outline || trap_length == 1)
        return 1;
    return x == left || x == right_exclusive - 1;
}

#endif
