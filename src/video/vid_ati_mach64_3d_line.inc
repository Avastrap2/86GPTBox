/*
 * GT/GTB shaded Bresenham line path.
 *
 * DP_SRC foreground selector 5 is documented as Scaler/3D data.  A normal
 * Mach64 line trajectory is therefore still the geometry generator, but its
 * pixel source comes from the 3D color/Z/alpha interpolators instead of
 * DP_FRGD_CLR.  The legacy 2D renderer has no selector-5 source and currently
 * falls through to zero, which turns Direct3D line primitives black.
 *
 * Keep this path deliberately narrow: it claims only untextured 3D shading
 * lines (SCALE_3D_FCN == 3), foreground source 5, and an always-one mono
 * source.  Ordinary 2D lines, polygon-boundary lines, and scaler operations
 * remain in the mature Mach64 2D engine.
 */
#define R3D_DP_SRC_SCALE_3D      5u
#define R3D_LINE_DISABLE         0x80000000u
#define R3D_LINE_DST_BRES_SIGN   (1u << 11)
#define R3D_GTB_BRES_SIGN_AUTO   (1u << 16)

static int
r3d_is_shaded_line_command(mach64_3d_ctx_t *ctx, uint32_t cmd)
{
    mach64_t *m;
    uint32_t ctl;
    unsigned fcn;
    unsigned mono_src;
    unsigned frgd_src;

    if (!ctx || (cmd & R3D_DRAW_TRAP))
        return 0;

    m = ctx->mach64;
    if (!m || (m->dst_cntl & DST_POLYGON_EN))
        return 0;

    ctl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    fcn = (ctl >> R3D_FCN_SHIFT) & 3u;
    mono_src = (m->dp_src >> 16) & 3u;
    frgd_src = (m->dp_src >> 8) & 7u;

    return fcn == 3u && mono_src == MONO_SRC_1 &&
           frgd_src == R3D_DP_SRC_SCALE_3D;
}

/*
 * GT adds programmable zero-error tie breaking to the Bresenham walker.
 * DST_BRES_SIGN makes zero negative when automatic sign selection is disabled.
 * With BRES_SIGN_AUTO set, ATI defines zero as positive for an X-major line
 * travelling with Y_DIR=0, or a Y-major line travelling with X_DIR=0; it is
 * negative for the opposite direction.  The distinction is only one pixel at
 * a tie, but connected line primitives rely on the same endpoint convention
 * and otherwise develop the detached/offset joins visible in Boids2.
 */
static int
r3d_line_zero_negative(const mach64_t *m, int ymajor)
{
    if (!(m->dst_cntl & R3D_GTB_BRES_SIGN_AUTO))
        return !!(m->dst_cntl & R3D_LINE_DST_BRES_SIGN);

    if (ymajor)
        return !!(m->dst_cntl & DST_X_DIR);
    return !!(m->dst_cntl & DST_Y_DIR);
}

static int
r3d_draw_shaded_line(mach64_3d_ctx_t *ctx, uint32_t cmd)
{
    mach64_t *m = ctx->mach64;
    uint32_t ctl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    int df = r3d_dst_format(m);
    int dbpp = r3d_bytes_per_pixel(df);
    int len = (int) (cmd & 0x7fffu);
    int x;
    int y;
    int xdir;
    int ydir;
    int ymajor;
    int zero_negative;
    int err;
    int inc;
    int dec;
    int left;
    int right;
    int top;
    int bottom;
    uint32_t db;
    int dp;
    uint32_t zc;
    uint32_t zo;
    uint32_t zb;
    int zp;
    int z_enabled;
    int z_write;
    int af;
    rgba_t fog;
    int64_t rx;
    int64_t gx;
    int64_t bx;
    int64_t ax;
    int64_t zx;
    int64_t ry;
    int64_t gy;
    int64_t by;
    int64_t ay;
    int64_t zy;

    if (!r3d_is_shaded_line_command(ctx, cmd))
        return 0;

    /* Bit 31 loads line state without starting a Bresenham operation. */
    if (cmd & R3D_LINE_DISABLE)
        return 1;

    if (!len || !dbpp)
        return 1;

    x = r3d_sign_extend(m->dst_y_x >> 16, 13);
    y = r3d_sign_extend(m->dst_y_x, 15);
    xdir = (m->dst_cntl & DST_X_DIR) ? 1 : -1;
    ydir = (m->dst_cntl & DST_Y_DIR) ? 1 : -1;
    ymajor = !!(m->dst_cntl & DST_Y_MAJOR);
    zero_negative = r3d_line_zero_negative(m, ymajor);

    /* The register guide defines the Bresenham terms as signed 18-bit data. */
    err = r3d_sign_extend(m->dst_bres_err, 18);
    inc = r3d_sign_extend(m->dst_bres_inc, 18);
    dec = r3d_sign_extend(m->dst_bres_dec, 18);

    left = (int) (m->sc_left_right & 0x1fffu);
    right = (int) ((m->sc_left_right >> 16) & 0x1fffu);
    top = (int) (m->sc_top_bottom & 0x7fffu);
    bottom = (int) ((m->sc_top_bottom >> 16) & 0x7fffu);
    if (right < left) {
        left = 0;
        right = 0x1fff;
    }
    if (bottom < top) {
        top = 0;
        bottom = 0x7fff;
    }

    db = (m->dst_off_pitch & 0xfffffu) << 3;
    dp = (int) (((m->dst_off_pitch >> 22) & 0x3ffu) << 3);
    if (!dp)
        return 1;

    if (left < 0)
        left = 0;
    if (right >= dp)
        right = dp - 1;
    if (right < left)
        return 1;

    {
        uint32_t stride = (uint32_t) dp * (uint32_t) dbpp;
        uint32_t base = db & m->vram_mask;
        if (stride && m->vram_size > base) {
            uint32_t rows = (m->vram_size - base) / stride;
            if (rows && bottom >= (int) rows)
                bottom = (int) rows - 1;
        }
        if (bottom < top)
            return 1;
    }

    zc = ctx->regs[R3D_Z_CNTL >> 2];
    zo = ctx->regs[R3D_Z_OFF_PITCH >> 2];
    zb = (zo & 0xfffffu) << 3;
    zp = (int) (((zo >> 22) & 0x3ffu) << 3);
    z_enabled = !!((zc & R3D_Z_EN) && zp);
    z_write = !!(z_enabled && (zc & R3D_Z_MASK));
    af = (int) ((ctl >> R3D_ALPHA_FOG_SHIFT) & 3u);
    fog = (rgba_t) {
        (m->dp_frgd_clr >> 16) & 255,
        (m->dp_frgd_clr >> 8) & 255,
        m->dp_frgd_clr & 255,
        255
    };

#define LINE_RG(name, var) \
    int64_t var = (int32_t) ctx->regs[R3D_##name##_START >> 2]; \
    int64_t var##x = (int32_t) ctx->regs[R3D_##name##_X_INC >> 2]; \
    int64_t var##y = (int32_t) ctx->regs[R3D_##name##_Y_INC >> 2]
    LINE_RG(RED, rr);
    LINE_RG(GREEN, gg);
    LINE_RG(BLUE, bb);
    LINE_RG(ALPHA, aa);
    LINE_RG(Z, zz);
#undef LINE_RG

    rx = rrx; gx = ggx; bx = bbx; ax = aax; zx = zzx;
    ry = rry; gy = ggy; by = bby; ay = aay; zy = zzy;

    for (int i = 0; i < len; i++) {
        int draw = 1;

        if (i == len - 1 && !(m->dst_cntl & DST_LAST_PEL))
            draw = 0;

        if (x < left || x > right || y < top || y > bottom)
            draw = 0;

        if (draw) {
            uint32_t da = db + (uint32_t) ((y * dp + x) * dbpp);
            rgba_t shade = r3d_shade(rr, gg, bb, aa);
            rgba_t src = shade;
            uint16_t nz = 0;
            uint32_t za = 0;

            if (af == 2)
                src = r3d_lerp(fog, src, shade.a);

            if (z_enabled) {
                if (x < 0 || x >= zp || y < 0) {
                    draw = 0;
                } else {
                    za = zb + (uint32_t) ((y * zp + x) * 2);
                    nz = r3d_clamp16(zz >> 12);
                    if (!r3d_z_test(nz, r3d_vram_read16(m, za),
                                    (zc >> R3D_Z_TEST_SHIFT) & 7))
                        draw = 0;
                }
            }

            if (draw) {
                if (af == 1)
                    src = r3d_blend(ctl, src, r3d_read_dst(ctx, da, df));
                r3d_write_dst(ctx, da, df, src);
                if (z_write)
                    r3d_vram_write16(m, za, nz);
            }
        }

        if (i == len - 1)
            break;

        if (ymajor) {
            y += ydir;
            rr += ry; gg += gy; bb += by; aa += ay; zz += zy;
            if (err > 0 || (err == 0 && !zero_negative)) {
                err += dec;
                x += xdir;
                rr += rx; gg += gx; bb += bx; aa += ax; zz += zx;
            } else {
                err += inc;
            }
        } else {
            x += xdir;
            rr += rx; gg += gx; bb += bx; aa += ax; zz += zx;
            if (err > 0 || (err == 0 && !zero_negative)) {
                err += dec;
                y += ydir;
                rr += ry; gg += gy; bb += by; aa += ay; zz += zy;
            } else {
                err += inc;
            }
        }
    }

    return 1;
}
