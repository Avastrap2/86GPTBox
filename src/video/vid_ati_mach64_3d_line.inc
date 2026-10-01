/*
 * GT/GTB 3D Bresenham line path.
 *
 * DP_SRC foreground selector 5 is documented as Scaler/3D data.  A normal
 * Mach64 line trajectory is therefore still the geometry generator, but its
 * pixel source comes from the 3D engine instead of DP_FRGD_CLR: the color/Z/
 * alpha interpolators (SCALE_3D_FCN 3) or the texture mapper (SCALE_3D_FCN 2).
 * The legacy 2D renderer has no selector-5 source and falls through to zero,
 * which turns Direct3D line primitives black.  The Windows 95 HAL draws
 * wireframe triangle edges as such lines, textured whenever a texture is set.
 *
 * Keep this path deliberately narrow: it claims only 3D shading or texture
 * lines, foreground source 5, and an always-one mono source.  Ordinary 2D
 * lines, polygon-boundary lines, and scaler operations remain in the mature
 * Mach64 2D engine.
 */
#define R3D_DP_SRC_SCALE_3D      5u
#define R3D_LINE_DISABLE         0x80000000u
#define R3D_LINE_DST_BRES_SIGN   (1u << 11)
#define R3D_GTB_BRES_SIGN_AUTO   (1u << 16)

static int
r3d_is_3d_line_command(mach64_3d_ctx_t *ctx, uint32_t cmd)
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

    return (fcn == 2u || fcn == 3u) && mono_src == MONO_SRC_1 &&
           frgd_src == R3D_DP_SRC_SCALE_3D;
}

/*
 * GT adds programmable zero-error tie breaking to the Bresenham walker.
 * DST_BRES_SIGN makes zero negative when automatic sign selection is disabled.
 * With BRES_SIGN_AUTO set, ATI defines zero as positive for an X-major line
 * travelling with Y_DIR=0, or a Y-major line travelling with X_DIR=0; it is
 * negative for the opposite direction.  The distinction is only one pixel at
 * a tie, but connected line primitives rely on the same endpoint convention
 * and otherwise develop detached/offset joins.
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

/* The interpolators of one line: colour, alpha and Z in their physical
 * fields as trapezoids decode them, and S/T with their quadratic terms. */
typedef struct r3d_line_interp_t {
    int64_t r, g, b, a, z, s, t;
    int64_t rx, gx, bx, ax, zx;
    int64_t ry, gy, by, ay, zy;
    int64_t sxi, syi, sx2, sy2, sxy2;
    int64_t txi, tyi, tx2, ty2, txy2;
} r3d_line_interp_t;

/* X_INC is defined for one step in DST_X_DIR and Y_INC for one step in
 * DST_Y_DIR, so a trajectory step adds them without a sign of its own; the
 * second-order S/T terms advance as on a trapezoid's leading edge. */
static void
r3d_line_step_x(r3d_line_interp_t *p)
{
    p->r += p->rx; p->g += p->gx; p->b += p->bx; p->a += p->ax; p->z += p->zx;
    p->s += p->sxi; p->sxi += p->sx2; p->syi += p->sxy2;
    p->t += p->txi; p->txi += p->tx2; p->tyi += p->txy2;
}

static void
r3d_line_step_y(r3d_line_interp_t *p)
{
    p->r += p->ry; p->g += p->gy; p->b += p->by; p->a += p->ay; p->z += p->zy;
    p->s += p->syi; p->syi += p->sy2; p->sxi += p->sxy2;
    p->t += p->tyi; p->tyi += p->ty2; p->txi += p->txy2;
}

static int
r3d_draw_3d_line(mach64_3d_ctx_t *ctx, uint32_t cmd)
{
    mach64_t *m = ctx->mach64;
    uint32_t ctl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    int tex = ((ctl >> R3D_FCN_SHIFT) & 3u) == 2u;
    int df = r3d_dst_format(m);
    int dbpp = r3d_bytes_per_pixel(df);
    int tf = r3d_tex_format(m);
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
    int destination_compare;
    uint32_t compare_pixel_mask;
    rgba_t fog;
    r3d_line_interp_t p;
    r3d_tex_state_t ts;

    if (!r3d_is_3d_line_command(ctx, cmd))
        return 0;

    /* Bit 31 loads line state without starting a Bresenham operation. */
    if (cmd & R3D_LINE_DISABLE)
        return 1;

    /* As for a trapezoid, texture mapping without a texel format draws nothing. */
    if (!len || !dbpp || (tex && !r3d_bytes_per_pixel(tf)))
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

    left = r3d_sign_extend(m->sc_left_right, 13);
    right = r3d_sign_extend(m->sc_left_right >> 16, 13);
    top = r3d_sign_extend(m->sc_top_bottom, 15);
    bottom = r3d_sign_extend(m->sc_top_bottom >> 16, 15);

    db = (m->dst_off_pitch & 0xfffffu) << 3;
    dp = (int) (((m->dst_off_pitch >> 22) & 0x3ffu) << 3);
    if (!dp)
        return 1;

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right >= dp)
        right = dp - 1;

    {
        uint32_t stride = (uint32_t) dp * (uint32_t) dbpp;
        uint32_t base = db & m->vram_mask;
        if (stride && m->vram_size > base) {
            uint32_t rows = (m->vram_size - base) / stride;
            if (rows && bottom >= (int) rows)
                bottom = (int) rows - 1;
        }
    }

    zc = ctx->regs[R3D_Z_CNTL >> 2];
    zo = ctx->regs[R3D_Z_OFF_PITCH >> 2];
    zb = (zo & 0xfffffu) << 3;
    zp = (int) (((zo >> 22) & 0x3ffu) << 3);
    z_enabled = !!((zc & R3D_Z_EN) && zp);
    z_write = !!(z_enabled && (zc & R3D_Z_MASK));
    af = (int) ((ctl >> R3D_ALPHA_FOG_SHIFT) & 3u);
    destination_compare = mach64_3d_destination_compare_enabled(m->clr_cmp_cntl);
    compare_pixel_mask = dbpp == 1 ? 0xffu : (dbpp == 2 ? 0xffffu : 0xffffffffu);
    fog = (rgba_t) {
        (m->dp_frgd_clr >> 16) & 255,
        (m->dp_frgd_clr >> 8) & 255,
        m->dp_frgd_clr & 255,
        255
    };

#define LINE_RG(name, var) \
    p.var = mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_START >> 2]); \
    p.var##x = mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_X_INC >> 2]); \
    p.var##y = mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_Y_INC >> 2])
    LINE_RG(RED, r);
    LINE_RG(GREEN, g);
    LINE_RG(BLUE, b);
    LINE_RG(ALPHA, a);
#undef LINE_RG
    p.z = mach64_3d_s16_12_decode(ctx->regs[R3D_Z_START >> 2]);
    p.zx = mach64_3d_s16_12_decode(ctx->regs[R3D_Z_X_INC >> 2]);
    p.zy = mach64_3d_s16_12_decode(ctx->regs[R3D_Z_Y_INC >> 2]);
    p.s = r3d_tex_dynamic_decode(ctx, R3D_S_START, ctx->regs[R3D_S_START >> 2]);
    p.sxi = r3d_tex_dynamic_decode(ctx, R3D_S_XINC_START, ctx->regs[R3D_S_XINC_START >> 2]);
    p.syi = r3d_tex_dynamic_decode(ctx, R3D_S_Y_INC, ctx->regs[R3D_S_Y_INC >> 2]);
    p.sx2 = mach64_3d_s10_16_decode(ctx->regs[R3D_S_X_INC2 >> 2]);
    p.sy2 = mach64_3d_s10_16_decode(ctx->regs[R3D_S_Y_INC2 >> 2]);
    p.sxy2 = mach64_3d_s10_16_decode(ctx->regs[R3D_S_XY_INC2 >> 2]);
    p.t = r3d_tex_dynamic_decode(ctx, R3D_T_START, ctx->regs[R3D_T_START >> 2]);
    p.txi = r3d_tex_dynamic_decode(ctx, R3D_T_XINC_START, ctx->regs[R3D_T_XINC_START >> 2]);
    p.tyi = r3d_tex_dynamic_decode(ctx, R3D_T_Y_INC, ctx->regs[R3D_T_Y_INC >> 2]);
    p.tx2 = mach64_3d_s10_16_decode(ctx->regs[R3D_T_X_INC2 >> 2]);
    p.ty2 = mach64_3d_s10_16_decode(ctx->regs[R3D_T_Y_INC2 >> 2]);
    p.txy2 = mach64_3d_s10_16_decode(ctx->regs[R3D_T_XY_INC2 >> 2]);
    if (tex)
        r3d_tex_state_init(ctx, tf, !!(ctl & R3D_BILINEAR_TEX_EN), &ts);

    for (int i = 0; i < len; i++) {
        int draw = 1;

        if (i == len - 1 && !(m->dst_cntl & DST_LAST_PEL))
            draw = 0;

        if (x < left || x > right || y < top || y > bottom)
            draw = 0;

        if (draw) {
            uint32_t da = db + (uint32_t) ((y * dp + x) * dbpp);
            rgba_t shade = {
                mach64_3d_s8_12_color(p.r),
                mach64_3d_s8_12_color(p.g),
                mach64_3d_s8_12_color(p.b),
                mach64_3d_s8_12_color(p.a)
            };
            rgba_t src = shade;
            uint16_t nz = 0;
            uint32_t za = 0;

            if (tex) {
                int texture_draw = 1;
                int texture_key_inhibit = 0;
                rgba_t texel = r3d_sample_texture_cached(ctx, &ts, ctl, p.s, p.t, p.sxi, p.txi,
                                                         p.syi, p.tyi, &texture_draw,
                                                         &texture_key_inhibit);

                if (!texture_draw || texture_key_inhibit || !r3d_apply_tex_alpha(ctl, &texel, shade.a))
                    draw = 0;
                else
                    src = r3d_texture_light(ctl, texel, shade);
            }

            if (draw && af == 2)
                src = r3d_lerp(fog, src, shade.a);

            if (draw && z_enabled) {
                if (x < 0 || x >= zp || y < 0) {
                    draw = 0;
                } else {
                    za = zb + (uint32_t) ((y * zp + x) * 2);
                    nz = mach64_3d_s16_12_depth(p.z);
                    if (!r3d_z_test(nz, r3d_vram_read16(m, za),
                                    (zc >> R3D_Z_TEST_SHIFT) & 7))
                        draw = 0;
                }
            }

            if (draw) {
                if (af == 1)
                    src = r3d_blend(ctl, src, r3d_read_dst(ctx, da, df));
                if (destination_compare &&
                    mach64_3d_destination_compare_inhibits(m->clr_cmp_cntl, m->clr_cmp_clr,
                                                           m->clr_cmp_mask,
                                                           r3d_read_dst_raw(ctx, da, df),
                                                           compare_pixel_mask))
                    draw = 0;
            }

            if (draw) {
                r3d_write_dst(ctx, da, df, src);
                if (z_write)
                    r3d_vram_write16(m, za, nz);
            }
        }

        if (i == len - 1)
            break;

        /*
         * The GT 3D HAL programs the Bresenham terms in accumulator form:
         * INC is accumulated for every major-axis step, while DEC is applied
         * only when that accumulation crosses the diagonal threshold and the
         * minor axis advances.  Treating INC and DEC as mutually exclusive
         * compresses steep line slopes toward the major axis.
         */
        if (ymajor) {
            y += ydir;
            r3d_line_step_y(&p);
        } else {
            x += xdir;
            r3d_line_step_x(&p);
        }

        err += inc;
        if (err > 0 || (err == 0 && !zero_negative)) {
            err += dec;
            if (ymajor) {
                x += xdir;
                r3d_line_step_x(&p);
            } else {
                y += ydir;
                r3d_line_step_y(&p);
            }
        }
    }

    /*
     * Mach64 line trajectories are live state.  After completion DST_X/Y rest
     * on the final trajectory pixel even when DST_LAST_PEL suppresses that
     * pixel's write.  Win9x can chain connected line segments from this endpoint;
     * leaving the original start latched makes later segments detach or float.
     */
    m->dst_y_x = ((uint32_t) (x & 0x1fff) << 16) |
                 ((uint32_t) y & 0x7fff);

    return 1;
}
