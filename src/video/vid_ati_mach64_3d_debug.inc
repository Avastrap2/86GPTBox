/*
 * Rage II+ 3D diagnostic trace.
 *
 * Keep this separate from rage2p_trace.log: the latter records the last guest
 * register accesses, while this file retains the information needed to explain
 * a slow/black Direct3D frame without logging every rasterized pixel.  Per-pixel
 * file I/O would change the timing we are trying to diagnose, so we record exact
 * draw bounds/counters and FIFO wait durations in memory and dump on detach.
 *
 * The interpolation snapshot is intentionally captured at DRAW_TRAP entry,
 * before the software renderer mutates START/increment state.  This lets a
 * captured Win9x HAL command be replayed offline against the GT/GTB equations.
 */
#define R3D_DEBUG_ACCESS_FIRST 4096
#define R3D_DEBUG_ACCESS_LAST  32768
#define R3D_DEBUG_DRAWS        8192
#define R3D_DEBUG_SYNCS        8192

typedef struct r3d_debug_access_t {
    uint64_t seq;
    uint32_t addr;
    uint32_t value;
    uint32_t fifo_entries;
    uint32_t dst_cntl;
    uint32_t dp_pix_width;
    uint32_t scale_3d_cntl;
    uint8_t op;
    uint8_t width;
    uint8_t claimed;
} r3d_debug_access_t;

typedef struct r3d_debug_draw_t {
    uint64_t seq;
    uint64_t start_tick;
    uint64_t end_tick;
    uint64_t candidate_pixels;
    uint64_t drawn_pixels;
    uint64_t alpha_rejects;
    uint64_t z_rejects;
    uint64_t texture_samples;

    uint32_t cmd;
    uint32_t ctl;
    uint32_t dst_cntl;
    uint32_t dst_y_x;
    uint32_t dst_off_pitch;
    uint32_t sc_left_right;
    uint32_t sc_top_bottom;
    uint32_t z_cntl;
    uint32_t z_off_pitch;
    uint32_t dp_pix_width;
    uint32_t write_mask;
    uint32_t tex_size_pitch;
    uint32_t tex_base;
    uint32_t fifo_entries_begin;
    uint32_t fifo_entries_end;

    /* Raw edge state at DRAW_TRAP entry. */
    uint32_t lead_bres_err;
    uint32_t lead_bres_inc;
    uint32_t lead_bres_dec;
    uint32_t trail_bres_err;
    uint32_t trail_bres_inc;
    uint32_t trail_bres_dec;

    /* Raw GT quadratic texture interpolator state at DRAW_TRAP entry. */
    uint32_t s_x_inc2;
    uint32_t s_y_inc2;
    uint32_t s_xy_inc2;
    uint32_t s_xinc_start;
    uint32_t s_y_inc;
    uint32_t s_start;
    uint32_t t_x_inc2;
    uint32_t t_y_inc2;
    uint32_t t_xy_inc2;
    uint32_t t_xinc_start;
    uint32_t t_y_inc;
    uint32_t t_start;

    /* Raw colour/Z/alpha interpolator state at DRAW_TRAP entry. */
    uint32_t red_x_inc;
    uint32_t red_y_inc;
    uint32_t red_start;
    uint32_t green_x_inc;
    uint32_t green_y_inc;
    uint32_t green_start;
    uint32_t blue_x_inc;
    uint32_t blue_y_inc;
    uint32_t blue_start;
    uint32_t z_x_inc;
    uint32_t z_y_inc;
    uint32_t z_start;
    uint32_t alpha_x_inc;
    uint32_t alpha_y_inc;
    uint32_t alpha_start;

    int lead_begin;
    int trail_begin;
    int y_begin;
    int lead_end;
    int trail_end;
    int y_end;
    int left;
    int right;
    int top;
    int bottom;
    int pitch_pixels;
    int length;
    int dst_format;
    int dst_bpp;
    int tex_format;
    int tex_bpp;
    int fcn;
    int alpha_fog;
    int bilinear;
    int z_enabled;
    int z_write;
} r3d_debug_draw_t;

typedef struct r3d_debug_sync_t {
    uint64_t seq;
    uint64_t start_tick;
    uint64_t end_tick;
    uint32_t fifo_before;
    uint32_t fifo_after;
} r3d_debug_sync_t;

typedef struct r3d_debug_state_t {
    mach64_t *mach64;
    uint64_t access_seq;
    uint64_t draw_seq;
    uint64_t sync_seq;
    uint64_t total_candidate_pixels;
    uint64_t total_drawn_pixels;
    uint64_t total_alpha_rejects;
    uint64_t total_z_rejects;
    uint64_t total_texture_samples;
    uint64_t total_draw_ticks;
    uint64_t total_sync_ticks;
    uint64_t max_draw_ticks;
    uint64_t max_sync_ticks;
    uint32_t max_fifo_entries;
    r3d_debug_access_t access_first[R3D_DEBUG_ACCESS_FIRST];
    r3d_debug_access_t access_last[R3D_DEBUG_ACCESS_LAST];
    r3d_debug_draw_t draws[R3D_DEBUG_DRAWS];
    r3d_debug_sync_t syncs[R3D_DEBUG_SYNCS];
} r3d_debug_state_t;

static r3d_debug_state_t r3d_debug_states[MACH64_3D_CONTEXTS];

static r3d_debug_state_t *
r3d_debug_find(mach64_t *m, int create)
{
    r3d_debug_state_t *free_state = NULL;

    if (!m)
        return NULL;
    for (unsigned i = 0; i < MACH64_3D_CONTEXTS; i++) {
        if (r3d_debug_states[i].mach64 == m)
            return &r3d_debug_states[i];
        if (!r3d_debug_states[i].mach64 && !free_state)
            free_state = &r3d_debug_states[i];
    }
    if (!create || !free_state)
        return NULL;
    memset(free_state, 0, sizeof(*free_state));
    free_state->mach64 = m;
    return free_state;
}

static void
r3d_debug_reset(mach64_t *m)
{
    r3d_debug_state_t *s = r3d_debug_find(m, 1);
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->mach64 = m;
}

static void
r3d_debug_access(mach64_3d_ctx_t *ctx, char op, unsigned width,
                 uint32_t addr, uint32_t value, int claimed)
{
    r3d_debug_state_t *s;
    r3d_debug_access_t e;
    uint32_t fifo_entries;

    if (!ctx || !ctx->mach64)
        return;
    s = r3d_debug_find(ctx->mach64, 1);
    if (!s)
        return;

    fifo_entries = ctx->mach64->fifo_write_idx - ctx->mach64->fifo_read_idx;
    memset(&e, 0, sizeof(e));
    e.seq = ++s->access_seq;
    e.addr = addr;
    e.value = value;
    e.fifo_entries = fifo_entries;
    e.dst_cntl = ctx->mach64->dst_cntl;
    e.dp_pix_width = ctx->mach64->dp_pix_width;
    e.scale_3d_cntl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    e.op = (uint8_t) op;
    e.width = (uint8_t) width;
    e.claimed = claimed ? 1 : 0;

    if (e.seq <= R3D_DEBUG_ACCESS_FIRST)
        s->access_first[e.seq - 1] = e;
    s->access_last[(e.seq - 1) % R3D_DEBUG_ACCESS_LAST] = e;
    if (fifo_entries > s->max_fifo_entries)
        s->max_fifo_entries = fifo_entries;
}

static void
r3d_debug_sync(mach64_3d_ctx_t *ctx, uint64_t start_tick, uint64_t end_tick,
               uint32_t fifo_before, uint32_t fifo_after)
{
    r3d_debug_state_t *s;
    r3d_debug_sync_t *e;
    uint64_t dt;

    if (!ctx || !ctx->mach64)
        return;
    s = r3d_debug_find(ctx->mach64, 1);
    if (!s)
        return;
    e = &s->syncs[s->sync_seq % R3D_DEBUG_SYNCS];
    memset(e, 0, sizeof(*e));
    e->seq = ++s->sync_seq;
    e->start_tick = start_tick;
    e->end_tick = end_tick;
    e->fifo_before = fifo_before;
    e->fifo_after = fifo_after;
    dt = end_tick - start_tick;
    s->total_sync_ticks += dt;
    if (dt > s->max_sync_ticks)
        s->max_sync_ticks = dt;
    if (fifo_before > s->max_fifo_entries)
        s->max_fifo_entries = fifo_before;
}

static void
r3d_debug_draw_begin(mach64_3d_ctx_t *ctx, r3d_debug_draw_t *d, uint32_t cmd,
                     int lead, int trail, int y, int left, int right,
                     int top, int bottom, int pitch_pixels, int length,
                     int df, int dbpp, int tf, int tbpp)
{
    mach64_t *m;
    uint32_t tsp;
    unsigned sl;

    if (!ctx || !ctx->mach64 || !d)
        return;
    m = ctx->mach64;
    memset(d, 0, sizeof(*d));

    d->start_tick = plat_timer_read();
    d->cmd = cmd;
    d->ctl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    d->dst_cntl = m->dst_cntl;
    d->dst_y_x = m->dst_y_x;
    d->dst_off_pitch = m->dst_off_pitch;
    d->sc_left_right = m->sc_left_right;
    d->sc_top_bottom = m->sc_top_bottom;
    d->z_cntl = ctx->regs[R3D_Z_CNTL >> 2];
    d->z_off_pitch = ctx->regs[R3D_Z_OFF_PITCH >> 2];
    d->dp_pix_width = m->dp_pix_width;
    d->write_mask = m->write_mask;
    d->tex_size_pitch = ctx->regs[R3D_TEX_SIZE_PITCH >> 2];

    tsp = d->tex_size_pitch;
    sl = (tsp >> 4) & 15;
    if (sl > 10)
        sl = 10;
    d->tex_base = ctx->regs[(R3D_TEX_0_OFF + (sl << 2)) >> 2];

    d->lead_bres_err = m->dst_bres_err;
    d->lead_bres_inc = m->dst_bres_inc;
    d->lead_bres_dec = m->dst_bres_dec;
    d->trail_bres_err = ctx->regs[R3D_TRAIL_BRES_ERR >> 2];
    d->trail_bres_inc = ctx->regs[R3D_TRAIL_BRES_INC >> 2];
    d->trail_bres_dec = ctx->regs[R3D_TRAIL_BRES_DEC >> 2];

#define SNAP_ST(name, lower) \
    d->lower##_x_inc2 = ctx->regs[R3D_##name##_X_INC2 >> 2]; \
    d->lower##_y_inc2 = ctx->regs[R3D_##name##_Y_INC2 >> 2]; \
    d->lower##_xy_inc2 = ctx->regs[R3D_##name##_XY_INC2 >> 2]; \
    d->lower##_xinc_start = ctx->regs[R3D_##name##_XINC_START >> 2]; \
    d->lower##_y_inc = ctx->regs[R3D_##name##_Y_INC >> 2]; \
    d->lower##_start = ctx->regs[R3D_##name##_START >> 2]
    SNAP_ST(S, s);
    SNAP_ST(T, t);
#undef SNAP_ST

#define SNAP_COLOR(name, lower) \
    d->lower##_x_inc = ctx->regs[R3D_##name##_X_INC >> 2]; \
    d->lower##_y_inc = ctx->regs[R3D_##name##_Y_INC >> 2]; \
    d->lower##_start = ctx->regs[R3D_##name##_START >> 2]
    SNAP_COLOR(RED, red);
    SNAP_COLOR(GREEN, green);
    SNAP_COLOR(BLUE, blue);
    SNAP_COLOR(Z, z);
    SNAP_COLOR(ALPHA, alpha);
#undef SNAP_COLOR

    d->fifo_entries_begin = m->fifo_write_idx - m->fifo_read_idx;
    d->lead_begin = lead;
    d->trail_begin = trail;
    d->y_begin = y;
    d->left = left;
    d->right = right;
    d->top = top;
    d->bottom = bottom;
    d->pitch_pixels = pitch_pixels;
    d->length = length;
    d->dst_format = df;
    d->dst_bpp = dbpp;
    d->tex_format = tf;
    d->tex_bpp = tbpp;
    d->fcn = (d->ctl >> R3D_FCN_SHIFT) & R3D_FCN_MASK;
    d->alpha_fog = (d->ctl >> R3D_ALPHA_FOG_SHIFT) & R3D_ALPHA_FOG_MASK;
    d->bilinear = !!(d->ctl & R3D_BILINEAR_TEX_EN);
    d->z_enabled = !!((d->z_cntl & R3D_Z_EN) &&
                      (((d->z_off_pitch >> 22) & 0x3ff) != 0));
    d->z_write = !!(d->z_enabled && (d->z_cntl & R3D_Z_MASK));
}

static void
r3d_debug_draw_end(mach64_3d_ctx_t *ctx, r3d_debug_draw_t *d,
                   int lead, int trail, int y)
{
    r3d_debug_state_t *s;
    r3d_debug_draw_t *slot;
    uint64_t dt;

    if (!ctx || !ctx->mach64 || !d)
        return;
    s = r3d_debug_find(ctx->mach64, 1);
    if (!s)
        return;

    d->end_tick = plat_timer_read();
    d->lead_end = lead;
    d->trail_end = trail;
    d->y_end = y;
    d->fifo_entries_end =
        ctx->mach64->fifo_write_idx - ctx->mach64->fifo_read_idx;
    d->seq = ++s->draw_seq;
    slot = &s->draws[(d->seq - 1) % R3D_DEBUG_DRAWS];
    *slot = *d;

    dt = d->end_tick - d->start_tick;
    s->total_draw_ticks += dt;
    s->total_candidate_pixels += d->candidate_pixels;
    s->total_drawn_pixels += d->drawn_pixels;
    s->total_alpha_rejects += d->alpha_rejects;
    s->total_z_rejects += d->z_rejects;
    s->total_texture_samples += d->texture_samples;
    if (dt > s->max_draw_ticks)
        s->max_draw_ticks = dt;
    if (d->fifo_entries_begin > s->max_fifo_entries)
        s->max_fifo_entries = d->fifo_entries_begin;
    if (d->fifo_entries_end > s->max_fifo_entries)
        s->max_fifo_entries = d->fifo_entries_end;
}

static void
r3d_debug_dump_access(FILE *fp, const r3d_debug_access_t *e)
{
    fprintf(fp, "%010llu %c %u %u %04X %08X %u %08X %08X %08X\n",
            (unsigned long long)e->seq, e->op, e->width, e->claimed,
            e->addr, e->value, e->fifo_entries, e->dst_cntl,
            e->dp_pix_width, e->scale_3d_cntl);
}

static void
r3d_debug_dump(mach64_t *m)
{
    r3d_debug_state_t *s = r3d_debug_find(m, 0);
    FILE *fp;
    uint64_t first_count;
    uint64_t last_count;
    uint64_t last_start;
    uint64_t draw_count;
    uint64_t draw_start;
    uint64_t sync_count;
    uint64_t sync_start;
    mach64_3d_ctx_t *ctx;

    if (!s || !s->access_seq)
        return;
    fp = fopen("rage2p_3d_debug.log", "w");
    if (!fp) {
        pclog("ATI Rage II+: unable to create rage2p_3d_debug.log\n");
        return;
    }

    fprintf(fp, "# Rage II+ complete 3D diagnostic trace v2\n");
    fprintf(fp, "# access=%llu draw=%llu sync=%llu max_fifo=%u\n",
            (unsigned long long)s->access_seq,
            (unsigned long long)s->draw_seq,
            (unsigned long long)s->sync_seq,
            s->max_fifo_entries);
    fprintf(fp,
            "# pixels candidate=%llu drawn=%llu alpha_reject=%llu "
            "z_reject=%llu texture_samples=%llu\n",
            (unsigned long long)s->total_candidate_pixels,
            (unsigned long long)s->total_drawn_pixels,
            (unsigned long long)s->total_alpha_rejects,
            (unsigned long long)s->total_z_rejects,
            (unsigned long long)s->total_texture_samples);
    fprintf(fp,
            "# ticks draw_total=%llu draw_max=%llu sync_total=%llu sync_max=%llu\n",
            (unsigned long long)s->total_draw_ticks,
            (unsigned long long)s->max_draw_ticks,
            (unsigned long long)s->total_sync_ticks,
            (unsigned long long)s->max_sync_ticks);
    fprintf(fp,
            "# final FIFO read=%u write=%u entries=%u blitter_busy=%d accel_busy=%d\n",
            (unsigned)m->fifo_read_idx, (unsigned)m->fifo_write_idx,
            (unsigned)(m->fifo_write_idx - m->fifo_read_idx),
            (int)m->blitter_busy, m->accel.busy);

    fprintf(fp, "\n[access_first]\n");
    fprintf(fp,
            "# seq op width claimed addr value fifo dst_cntl dp_pix_width "
            "scale_3d_cntl\n");
    first_count = s->access_seq < R3D_DEBUG_ACCESS_FIRST ?
                  s->access_seq : R3D_DEBUG_ACCESS_FIRST;
    for (uint64_t i = 0; i < first_count; i++)
        r3d_debug_dump_access(fp, &s->access_first[i]);

    fprintf(fp, "\n[access_last]\n");
    fprintf(fp,
            "# seq op width claimed addr value fifo dst_cntl dp_pix_width "
            "scale_3d_cntl\n");
    last_count = s->access_seq < R3D_DEBUG_ACCESS_LAST ?
                 s->access_seq : R3D_DEBUG_ACCESS_LAST;
    last_start = s->access_seq - last_count + 1;
    for (uint64_t seq = last_start; seq <= s->access_seq; seq++)
        r3d_debug_dump_access(
            fp, &s->access_last[(seq - 1) % R3D_DEBUG_ACCESS_LAST]);

    fprintf(fp, "\n[draws]\n");
    fprintf(fp,
            "# seq ticks candidate drawn alpha_rej z_rej tex cmd ctl dst_cntl "
            "dst_y_x dst_off_pitch sc_lr sc_tb z_cntl z_off dp_pix write_mask "
            "tex_size tex_base fifo0 fifo1 lead0 trail0 y0 lead1 trail1 y1 "
            "left right top bottom pitch len df dbpp tf tbpp fcn af bilinear "
            "zen zwrite\n");
    draw_count = s->draw_seq < R3D_DEBUG_DRAWS ?
                 s->draw_seq : R3D_DEBUG_DRAWS;
    draw_start = s->draw_seq - draw_count + 1;
    for (uint64_t seq = draw_start; seq <= s->draw_seq; seq++) {
        const r3d_debug_draw_t *d =
            &s->draws[(seq - 1) % R3D_DEBUG_DRAWS];
        fprintf(fp,
                "%010llu %llu %llu %llu %llu %llu %llu "
                "%08X %08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %u %u "
                "%d %d %d %d %d %d %d %d %d %d %d %d "
                "%d %d %d %d %d %d %d %d %d\n",
                (unsigned long long)d->seq,
                (unsigned long long)(d->end_tick - d->start_tick),
                (unsigned long long)d->candidate_pixels,
                (unsigned long long)d->drawn_pixels,
                (unsigned long long)d->alpha_rejects,
                (unsigned long long)d->z_rejects,
                (unsigned long long)d->texture_samples,
                d->cmd, d->ctl, d->dst_cntl, d->dst_y_x, d->dst_off_pitch,
                d->sc_left_right, d->sc_top_bottom, d->z_cntl, d->z_off_pitch,
                d->dp_pix_width, d->write_mask, d->tex_size_pitch, d->tex_base,
                d->fifo_entries_begin, d->fifo_entries_end,
                d->lead_begin, d->trail_begin, d->y_begin,
                d->lead_end, d->trail_end, d->y_end,
                d->left, d->right, d->top, d->bottom,
                d->pitch_pixels, d->length,
                d->dst_format, d->dst_bpp, d->tex_format, d->tex_bpp,
                d->fcn, d->alpha_fog, d->bilinear,
                d->z_enabled, d->z_write);
    }

    /*
     * Keep the legacy draw table stable and place the high-detail raw state in
     * a second table.  This makes old scripts continue to parse [draws], while
     * the new table is sufficient to replay the GTB edge and S/T equations.
     */
    fprintf(fp, "\n[interpolators]\n");
    fprintf(fp,
            "# seq lead_err lead_inc lead_dec trail_err trail_inc trail_dec "
            "sx2 sy2 sxy2 sxi sy s0 tx2 ty2 txy2 txi ty t0 "
            "rx ry r0 gx gy g0 bx by b0 zx zy z0 ax ay a0\n");
    for (uint64_t seq = draw_start; seq <= s->draw_seq; seq++) {
        const r3d_debug_draw_t *d =
            &s->draws[(seq - 1) % R3D_DEBUG_DRAWS];
        fprintf(fp,
                "%010llu "
                "%08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X\n",
                (unsigned long long)d->seq,
                d->lead_bres_err, d->lead_bres_inc, d->lead_bres_dec,
                d->trail_bres_err, d->trail_bres_inc, d->trail_bres_dec,
                d->s_x_inc2, d->s_y_inc2, d->s_xy_inc2,
                d->s_xinc_start, d->s_y_inc, d->s_start,
                d->t_x_inc2, d->t_y_inc2, d->t_xy_inc2,
                d->t_xinc_start, d->t_y_inc, d->t_start,
                d->red_x_inc, d->red_y_inc, d->red_start,
                d->green_x_inc, d->green_y_inc, d->green_start,
                d->blue_x_inc, d->blue_y_inc, d->blue_start,
                d->z_x_inc, d->z_y_inc, d->z_start,
                d->alpha_x_inc, d->alpha_y_inc, d->alpha_start);
    }

    fprintf(fp, "\n[syncs]\n");
    fprintf(fp, "# seq ticks fifo_before fifo_after\n");
    sync_count = s->sync_seq < R3D_DEBUG_SYNCS ?
                 s->sync_seq : R3D_DEBUG_SYNCS;
    sync_start = s->sync_seq - sync_count + 1;
    for (uint64_t seq = sync_start; seq <= s->sync_seq; seq++) {
        const r3d_debug_sync_t *e =
            &s->syncs[(seq - 1) % R3D_DEBUG_SYNCS];
        fprintf(fp, "%010llu %llu %u %u\n",
                (unsigned long long)e->seq,
                (unsigned long long)(e->end_tick - e->start_tick),
                e->fifo_before, e->fifo_after);
    }

    /*
     * The GT HAL uses 8-bit pseudocolor textures in the failing Flip3D path.
     * Snapshot the complete DAC lookup table so a texture-index problem can be
     * separated from S/T addressing without adding any per-pixel file I/O.
     */
    fprintf(fp, "\n[pseudocolor_palette]\n");
    fprintf(fp, "# index pallook\n");
    for (unsigned i = 0; i < 256; i++)
        fprintf(fp, "%02X %08X\n", i, m->svga.pallook[i]);

    fprintf(fp, "\n[final_3d_registers]\n");
    ctx = r3d_find(m);
    if (ctx) {
        for (unsigned i = 0; i < 256; i++)
            if (ctx->regs[i])
                fprintf(fp, "%03X %08X\n", i << 2, ctx->regs[i]);
    }

    fclose(fp);
    pclog("ATI Rage II+: wrote rage2p_3d_debug.log "
          "(access=%llu draw=%llu sync=%llu)\n",
          (unsigned long long)s->access_seq,
          (unsigned long long)s->draw_seq,
          (unsigned long long)s->sync_seq);
    memset(s, 0, sizeof(*s));
}
