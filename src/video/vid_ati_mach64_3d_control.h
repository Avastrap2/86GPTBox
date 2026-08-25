#ifndef VID_ATI_MACH64_3D_CONTROL_H
#define VID_ATI_MACH64_3D_CONTROL_H

/* RED_DITHER_MAX is meaningful only while dithering an RGB8 destination.
 * Clamping the three-bit red field to 6 reserves indices 224..255 and leaves
 * the documented overall palette range 0..223. */
static inline unsigned
mach64_3d_rgb8_red_code(unsigned red, int dithering, int red_dither_max)
{
    red &= 7u;
    if (dithering && red_dither_max && red > 6u)
        red = 6u;
    return red;
}

/* TEX_BLEND_FCN=3 is the Rage multipass trilinear mode.  With alpha blending
 * enabled, the texture unit supplies alpha from mip-map distance rather than
 * from the ordinary interpolated/texture-map alpha path. */
static inline int
mach64_3d_uses_lod_alpha(unsigned alpha_fog_mode, unsigned tex_blend_fcn)
{
    return ((alpha_fog_mode & 3u) == 1u) &&
           ((tex_blend_fcn & 3u) == 3u);
}

#endif
