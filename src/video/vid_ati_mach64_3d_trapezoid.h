#ifndef VID_ATI_MACH64_3D_TRAPEZOID_H
#define VID_ATI_MACH64_3D_TRAPEZOID_H

/* GT/GTB span ownership follows the programmed edge roles, not absolute
 * screen-left/screen-right.  The leading edge owns its boundary pixel and the
 * trailing edge does not.  Consequently, a left-to-right fill is [lead,trail)
 * while a right-to-left fill is (trail,lead].
 *
 * The shipping HAL uses adjacent leading edges when both neighboring pixels
 * must be retained, but programs an identical lead/trail trajectory at a
 * shared edge requiring one owner.  A collapsed lead==trail span has zero
 * horizontal coverage; drawing it repeatedly produces bright vertices in
 * additive scenes and dark rings when its texture samples the border color. */
static inline int
mach64_3d_trapezoid_clip_span(int lead, int trail, int fill_left_to_right,
                              int sc_left, int sc_right,
                              int *first, int *end)
{
    int lo;
    int hi;

    if (!first || !end || sc_left > sc_right)
        return 0;

    if (lead == trail)
        return 0;

    if (fill_left_to_right) {
        if (lead > trail)
            return 0;
        lo = lead;
        hi = trail;
    } else {
        if (trail > lead)
            return 0;
        lo = trail + 1;
        hi = lead + 1;
    }

    if (lo < sc_left)
        lo = sc_left;
    if (hi > sc_right + 1)
        hi = sc_right + 1;
    if (lo >= hi)
        return 0;

    *first = lo;
    *end = hi;
    return 1;
}

#endif
