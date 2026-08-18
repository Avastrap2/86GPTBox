#define R3D_GUI_STAT 0x338
#define R3D_GTB_FIFO_VISIBLE 48
#define R3D_DST_Y_X_ALIAS 0x134

static int
r3d_unique_register(uint32_t b)
{
    return ((b >= R3D_TRAIL_BRES_ERR) && (b <= R3D_Z_CNTL)) ||
           ((b >= R3D_TEX_0_OFF) && (b <= R3D_TEX_10_OFF)) ||
           (b == R3D_SCALE_3D_CNTL) ||
           ((b >= R3D_S_X_INC2) && (b <= R3D_TEX_SIZE_PITCH)) ||
           ((b >= R3D_RED_X_INC) && (b <= R3D_ALPHA_START));
}

/*
 * GUI_STAT is more than an idle flag on 3D RAGE.  The Windows 9x HAL uses
 * the live destination/scissor status while bringing up the 3D engine.  The
 * inherited VT2 reader only returned GUI_ACTIVE plus an encoded FIFO count,
 * leaving all four scissor comparison bits permanently clear.  A real GT/GTB
 * reports those comparisons even while the engine is idle.
 *
 * The original GT-A2 documentation describes a 48-entry parameter FIFO but
 * notes that that revision exposes FIFO_CNT only up to 32.  Do not carry that
 * early-GT reporting limit into the later GTB/GU path.  The Rage II+ Win9x HAL
 * issues bursts larger than 32 entries (the diagnostic trace reached 38) and
 * then polls GUI_STAT before it has emitted any 3D draw.  Reporting a permanent
 * maximum of 32 therefore leaves the HAL in an infinite reservation loop.
 * Expose the GTB parameter FIFO's full 48 entries here; VT/VT2 remain unchanged.
 */
static uint32_t
r3d_gtb_gui_stat(mach64_t *m)
{
    uint32_t used;
    uint32_t free_entries;
    uint32_t ret = 0;
    int dst_x;
    int dst_y;
    int sc_left;
    int sc_right;
    int sc_top;
    int sc_bottom;

    if (!m)
        return 0;

    used = (uint32_t) (m->fifo_write_idx - m->fifo_read_idx);
    if (used && !m->blitter_busy)
        mach64_wake_fifo_thread(m);

    free_entries = (used >= R3D_GTB_FIFO_VISIBLE) ? 0 :
                   (R3D_GTB_FIFO_VISIBLE - used);
    ret |= free_entries << 16;

    if (used || m->blitter_busy || m->accel.busy)
        ret |= 0x01; /* GUI_ACTIVE */

    dst_x = r3d_sign_extend(m->dst_y_x >> 16, 13);
    dst_y = r3d_sign_extend(m->dst_y_x, 15);
    sc_left = (int) (m->sc_left_right & 0x1fff);
    sc_right = (int) ((m->sc_left_right >> 16) & 0x1fff);
    sc_top = (int) (m->sc_top_bottom & 0x7fff);
    sc_bottom = (int) ((m->sc_top_bottom >> 16) & 0x7fff);

    if (dst_x < sc_left)
        ret |= 0x10; /* DSTX_LT_SCISSOR_LEFT */
    if (dst_x > sc_right)
        ret |= 0x20; /* DSTX_GT_SCISSOR_RIGHT */
    if (dst_y < sc_top)
        ret |= 0x40; /* DSTY_LT_SCISSOR_TOP */
    if (dst_y > sc_bottom)
        ret |= 0x80; /* DSTY_GT_SCISSOR_BOTTOM */

    return ret;
}

static unsigned
r3d_fifo_width(uint32_t type)
{
    if (type == FIFO_WRITE_DWORD)
        return 4;
    if (type == FIFO_WRITE_WORD)
        return 2;
    return 1;
}

static uint32_t
r3d_merge_write(uint32_t old, uint32_t a, uint32_t v, uint32_t type)
{
    int n = (type == FIFO_WRITE_DWORD) ? 4 : ((type == FIFO_WRITE_WORD) ? 2 : 1);

    for (int i = 0; i < n; i++) {
        unsigned s = ((a + i) & 3) * 8;
        old = (old & ~(0xffu << s)) | (((v >> (i * 8)) & 0xffu) << s);
    }
    return old;
}

static int
r3d_write_complete(uint32_t a, uint32_t type)
{
    if (type == FIFO_WRITE_DWORD)
        return (a & 3) == 0;
    if (type == FIFO_WRITE_WORD)
        return (a & 3) == 2;
    return (a & 3) == 3;
}

/*
 * GT-only registers are not yet represented as entries in the legacy Mach64
 * worker FIFO.  We therefore need an ordering barrier when crossing from an
 * ordinary/shared 2D write into GT state or a DRAW_TRAP command.  Record the
 * exact FIFO depth and host timer duration for every barrier so a stalled
 * Direct3D program can be separated from a slow rasterizer in the trace.
 */
static void
r3d_sync_legacy_fifo(mach64_3d_ctx_t *ctx)
{
    uint32_t fifo_before;
    uint32_t fifo_after;
    uint64_t start_tick;
    uint64_t end_tick;

    if (!ctx || !ctx->legacy_fifo_pending)
        return;

    fifo_before = ctx->mach64->fifo_write_idx - ctx->mach64->fifo_read_idx;
    start_tick = plat_timer_read();
    mach64_wait_fifo_idle(ctx->mach64);
    end_tick = plat_timer_read();
    fifo_after = ctx->mach64->fifo_write_idx - ctx->mach64->fifo_read_idx;
    r3d_debug_sync(ctx, start_tick, end_tick, fifo_before, fifo_after);
    ctx->legacy_fifo_pending = 0;
}

int
mach64_3d_read(mach64_t *m, uint32_t a, uint32_t *v)
{
    mach64_3d_ctx_t *ctx = r3d_find(m);
    if (!ctx || !v)
        return 0;

    a &= 0x3ff;
    uint32_t b = a & ~3u;

    /* Every Rage II+ MMIO read reaches this probe before the VT2 fallback. */
    r3d_trace_record(m, 'R', 0, a, 0, 0);
    r3d_debug_access(ctx, 'R', 0, a, 0, 0);

    /*
     * GUI_STAT is common to the 2D and 3D engines, but GTB needs the complete
     * status word.  Claim all byte/word/dword lanes here so the Rage II+ path
     * cannot fall back to the VT2 implementation that omits scissor flags.
     */
    if (b == R3D_GUI_STAT) {
        *v = r3d_gtb_gui_stat(m);
        r3d_trace_record(m, 'V', 4, b, *v, 1);
        r3d_debug_access(ctx, 'V', 4, b, *v, 1);
        return 1;
    }

    /*
     * DST_BRES_LNTH (0x120) is shared with the legacy 2D line engine.  Only
     * claim it while the shadow contains a 3D DRAW_TRAP command; otherwise
     * the established Mach64 readback remains authoritative.
     */
    if (b == R3D_LEAD_BRES_LNTH) {
        uint32_t cmd = ctx->regs[R3D_LEAD_BRES_LNTH >> 2];
        if (!(cmd & R3D_DRAW_TRAP))
            return 0;
        r3d_sync_legacy_fifo(ctx);
        *v = cmd;
        r3d_trace_record(m, 'V', 4, b, *v, 1);
        r3d_debug_access(ctx, 'V', 4, b, *v, 1);
        return 1;
    }

    if (!r3d_unique_register(b))
        return 0;

    r3d_sync_legacy_fifo(ctx);

    /* 0x144 is a 3D alias for the same lead-length register shadow. */
    if (b == R3D_LEAD_BRES_LNTH_ALIAS)
        *v = ctx->regs[R3D_LEAD_BRES_LNTH >> 2];
    else
        *v = ctx->regs[b >> 2];

    r3d_trace_record(m, 'V', 4, b, *v, 1);
    r3d_debug_access(ctx, 'V', 4, b, *v, 1);
    return 1;
}

int
mach64_3d_write(mach64_t *m, uint32_t a, uint32_t v, uint32_t type)
{
    mach64_3d_ctx_t *ctx = r3d_find(m);
    unsigned width;

    if (!ctx)
        return 0;

    a &= 0x3ff;
    uint32_t b = a & ~3u;
    width = r3d_fifo_width(type);

    /*
     * mach64_queue() sends every GUI write here before the legacy 2D FIFO.
     * Therefore W records include both 2D and 3D traffic.  C records indicate
     * writes actually consumed by the GT/GTB 3D block.
     */
    r3d_trace_record(m, 'W', width, a, v, 0);
    r3d_debug_access(ctx, 'W', width, a, v, 0);

    /*
     * 3D RAGE adds a write-only DST_Y_X alias at MM register 0_4D (byte
     * offset 0x134).  The Win9x HAL uses this alias for trapezoid setup.  The
     * inherited VT2 decoder has no 0x134 case, so dropping it pins each draw's
     * lead edge and starting Y at zero and produces the characteristic large
     * textured wedge from the top-left corner.
     */
    if (b == R3D_DST_Y_X_ALIAS) {
        r3d_sync_legacy_fifo(ctx);
        m->dst_y_x = r3d_merge_write(m->dst_y_x, a, v, type);
        r3d_trace_record(m, 'C', width, b, m->dst_y_x, 1);
        r3d_debug_access(ctx, 'C', width, b, m->dst_y_x, 1);
        return 1;
    }

    /*
     * 0x120 is shared with the legacy line engine.  Mirror every partial
     * write into the 3D shadow even before DRAW_TRAP becomes visible; real
     * drivers may write byte/word pieces in an order where bit 15 arrives
     * after the low length bits.  Non-3D writes still fall through to the
     * legacy Mach64 engine and mark that FIFO as needing a barrier before the
     * next GT-only state change.
     */
    if ((b == R3D_LEAD_BRES_LNTH) || (b == R3D_LEAD_BRES_LNTH_ALIAS)) {
        uint32_t i = R3D_LEAD_BRES_LNTH >> 2;
        uint32_t cmd = r3d_merge_write(ctx->regs[i], a, v, type);
        ctx->regs[i] = cmd;

        int is_3d = (cmd & R3D_DRAW_TRAP) || (b == R3D_LEAD_BRES_LNTH_ALIAS);
        if (!is_3d) {
            ctx->legacy_fifo_pending = 1;
            return 0;
        }

        r3d_trace_record(m, 'C', width, b, cmd, 1);
        r3d_debug_access(ctx, 'C', width, b, cmd, 1);
        r3d_sync_legacy_fifo(ctx);
        if ((cmd & R3D_DRAW_TRAP) && r3d_write_complete(a, type)) {
            r3d_trace_record(m, 'T', width, b, cmd, 1);
            r3d_debug_access(ctx, 'T', width, b, cmd, 1);
            r3d_draw_trapezoid(ctx, cmd);
        }
        return 1;
    }

    if (!r3d_unique_register(b)) {
        ctx->legacy_fifo_pending = 1;
        return 0;
    }

    r3d_sync_legacy_fifo(ctx);
    uint32_t i = b >> 2;
    ctx->regs[i] = r3d_merge_write(ctx->regs[i], a, v, type);
    r3d_trace_record(m, 'C', width, b, ctx->regs[i], 1);
    r3d_debug_access(ctx, 'C', width, b, ctx->regs[i], 1);
    return 1;
}
