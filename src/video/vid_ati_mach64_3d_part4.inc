static int
r3d_unique_register(uint32_t b)
{
    return ((b >= R3D_TRAIL_BRES_ERR) && (b <= R3D_Z_CNTL)) ||
           ((b >= R3D_TEX_0_OFF) && (b <= R3D_TEX_10_OFF)) ||
           (b == R3D_SCALE_3D_CNTL) ||
           ((b >= R3D_S_X_INC2) && (b <= R3D_TEX_SIZE_PITCH)) ||
           ((b >= R3D_RED_X_INC) && (b <= R3D_ALPHA_START));
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
 * ordinary/shared 2D write into GT state or a DRAW_TRAP command.  The previous
 * implementation drained the FIFO for every single GT register write, turning
 * a normal Direct3D state burst into dozens of thread synchronizations per
 * triangle.  One drain per 2D->3D transition preserves ordering without
 * serializing the entire 3D register file.
 */
static void
r3d_sync_legacy_fifo(mach64_3d_ctx_t *ctx)
{
    if (!ctx || !ctx->legacy_fifo_pending)
        return;

    mach64_wait_fifo_idle(ctx->mach64);
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
        r3d_sync_legacy_fifo(ctx);
        if ((cmd & R3D_DRAW_TRAP) && r3d_write_complete(a, type)) {
            r3d_trace_record(m, 'T', width, b, cmd, 1);
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
    return 1;
}