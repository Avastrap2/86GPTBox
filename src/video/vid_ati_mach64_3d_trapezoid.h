#ifndef VID_ATI_MACH64_3D_TRAPEZOID_H
#define VID_ATI_MACH64_3D_TRAPEZOID_H

/*
 * GT/GTB trapezoid rasterization owns pixels in a half-open horizontal span:
 * [left_edge, right_edge).  This is distinct from the legacy polygon-fill
 * trajectory.  DST_POLYGON_RTEDGE_DIS is documented as a polygon-fill control,
 * while the Windows 95 Rage II+ HAL leaves it clear for 3D trapezoids.
 *
 * Treating a clear DST_POLYGON_RTEDGE_DIS as "include the trapezoid right edge"
 * makes adjacent alpha-blended triangles touch the same pixel once per shared
 * scan line.  With Z testing enabled but Z writes disabled, that shared pixel is
 * blended twice and appears as the characteristic seam seen in the captured HAL
 * workload.  Keeping the 3D span right-exclusive gives each shared edge a single
 * owner while preserving the inclusive scissor rectangle.
 */
static inline int
mach64_3d_trapezoid_clip_span(int left_edge, int right_edge,
                              int sc_left, int sc_right,
                              int *first, int *end)
{
    int lo;
    int hi;

    if (!first || !end || left_edge > right_edge || sc_left > sc_right)
        return 0;

    lo = left_edge < sc_left ? sc_left : left_edge;
    hi = right_edge;
    if (hi > sc_right + 1)
        hi = sc_right + 1;

    if (lo >= hi)
        return 0;

    *first = lo;
    *end = hi;
    return 1;
}

#endif /* VID_ATI_MACH64_3D_TRAPEZOID_H */
