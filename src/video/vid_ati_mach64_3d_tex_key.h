#ifndef VID_ATI_MACH64_3D_TEX_KEY_H
#define VID_ATI_MACH64_3D_TEX_KEY_H

#include <stdint.h>

/* GT CLR_CMP_SRC=2 selects the texel/scaler source.  A true compare result
 * inhibits the destination write.  Non-indexed texels are compared after
 * expansion to 24-bit RGB; pseudo-color sources compare their index in the
 * low eight bits, as documented by the 3D RAGE register guide. */
static inline int
mach64_3d_texel_key_compare(uint32_t cntl, uint32_t key, uint32_t mask,
                            uint32_t selected)
{
    uint32_t reference;
    unsigned fn;
    unsigned source;

    source = (cntl >> 24) & 3u;
    if (source != 2u)
        return 0;

    selected &= mask;
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

static inline int
mach64_3d_texel_key_match(uint32_t cntl, uint32_t key, uint32_t mask,
                          uint8_t red, uint8_t green, uint8_t blue)
{
    uint32_t texel = ((uint32_t) red << 16) |
                     ((uint32_t) green << 8) | blue;

    return mach64_3d_texel_key_compare(cntl, key, mask, texel);
}

static inline int
mach64_3d_texel_key_match_index(uint32_t cntl, uint32_t key, uint32_t mask,
                                uint8_t index)
{
    return mach64_3d_texel_key_compare(cntl, key, mask, (uint32_t) index);
}

static inline int
mach64_3d_texel_visibility_inhibits(int nearest_only, int nearest_inhibits,
                                    int any_inhibits)
{
    return nearest_only ? nearest_inhibits : any_inhibits;
}

#endif
