#ifndef VID_ATI_MACH64_3D_TEX_KEY_H
#define VID_ATI_MACH64_3D_TEX_KEY_H

#include <stdint.h>

/* GT CLR_CMP_SRC=2 selects the expanded 24-bit texel/scaler source.  A true
 * compare result inhibits the destination write.  CI8 pseudo-color is a
 * separate indexed compare path and is deliberately excluded by the caller. */
static inline int
mach64_3d_texel_key_match(uint32_t cntl, uint32_t key, uint32_t mask,
                          uint8_t red, uint8_t green, uint8_t blue)
{
    uint32_t texel;
    uint32_t selected;
    uint32_t reference;
    unsigned fn;
    unsigned source;

    source = (cntl >> 24) & 3u;
    if (source != 2u)
        return 0;

    texel = ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    selected = texel & mask;
    reference = key & mask;
    fn = cntl & 7u;

    switch (fn) {
        case 1:
            return 1;
        case 4:
            return selected != reference;
        case 5:
            return selected == reference;
        default:
            return 0;
    }
}

#endif
