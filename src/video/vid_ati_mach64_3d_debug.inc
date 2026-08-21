/*
 * Rage II+ overlay-history diagnostic wrapper.
 *
 * Keep the existing complete 3D diagnostic implementation byte-for-byte in
 * vid_ati_mach64_3d_debug_base.inc and layer a change-only overlay history on
 * top.  Overlay register programming is sparse compared with normal GUI/3D
 * traffic, so retaining only state transitions survives Windows shutdown
 * without turning the generic access ring into a multi-million-entry buffer.
 */
#define r3d_debug_access r3d_debug_access_base
#define r3d_debug_reset  r3d_debug_reset_base
#define r3d_debug_dump   r3d_debug_dump_base
#include "vid_ati_mach64_3d_debug_base.inc"
#undef r3d_debug_access
#undef r3d_debug_reset
#undef r3d_debug_dump

#define R3D_OVERLAY_HISTORY 4096

typedef struct r3d_overlay_snapshot_t {
    uint32_t yx_start;
    uint32_t yx_end;
    uint32_t scale_inc;
    uint32_t scale_cntl;
    uint32_t height_width;
    uint32_t scaler_off0;
    uint32_t scaler_off1;
    uint32_t scaler_pitch;
    uint32_t video_key_clr;
    uint32_t video_key_msk;
    uint32_t graphics_key_clr;
    uint32_t graphics_key_msk;
    uint32_t key_cntl;
    uint32_t legacy_off0;
    uint32_t legacy_pitch0;
    uint32_t svga_addr;
    uint32_t svga_pitch;
    uint32_t overlay_base;
    uint32_t uv_addr;
    uint8_t format;
    uint8_t svga_ena;
} r3d_overlay_snapshot_t;

typedef struct r3d_overlay_event_t {
    uint64_t seq;
    uint64_t access_seq;
    uint32_t addr;
    uint32_t value;
    uint8_t op;
    uint8_t width;
    uint8_t claimed;
    r3d_overlay_snapshot_t state;
} r3d_overlay_event_t;

typedef struct r3d_overlay_history_state_t {
    mach64_t *mach64;
    uint64_t seq;
    int have_last;
    r3d_overlay_snapshot_t last;
    r3d_overlay_event_t events[R3D_OVERLAY_HISTORY];
} r3d_overlay_history_state_t;

static r3d_overlay_history_state_t r3d_overlay_history_states[MACH64_3D_CONTEXTS];

static r3d_overlay_history_state_t *
r3d_overlay_history_find(mach64_t *m, int create)
{
    r3d_overlay_history_state_t *free_state = NULL;

    if (!m)
        return NULL;
    for (unsigned i = 0; i < MACH64_3D_CONTEXTS; i++) {
        if (r3d_overlay_history_states[i].mach64 == m)
            return &r3d_overlay_history_states[i];
        if (!r3d_overlay_history_states[i].mach64 && !free_state)
            free_state = &r3d_overlay_history_states[i];
    }
    if (!create || !free_state)
        return NULL;
    memset(free_state, 0, sizeof(*free_state));
    free_state->mach64 = m;
    return free_state;
}

static void
r3d_overlay_snapshot(mach64_t *m, r3d_overlay_snapshot_t *o)
{
    memset(o, 0, sizeof(*o));
    if (!m)
        return;

    o->yx_start = m->overlay_y_x_start;
    o->yx_end = m->overlay_y_x_end;
    o->scale_inc = m->overlay_scale_inc;
    o->scale_cntl = m->overlay_scale_cntl;
    o->height_width = m->scaler_height_width;
    o->scaler_off0 = m->scaler_buf_offset[0];
    o->scaler_off1 = m->scaler_buf_offset[1];
    o->scaler_pitch = m->scaler_buf_pitch;
    o->video_key_clr = m->overlay_video_key_clr;
    o->video_key_msk = m->overlay_video_key_msk;
    o->graphics_key_clr = m->overlay_graphics_key_clr;
    o->graphics_key_msk = m->overlay_graphics_key_msk;
    o->key_cntl = m->overlay_key_cntl;
    o->legacy_off0 = m->buf_offset[0];
    o->legacy_pitch0 = m->buf_pitch[0];
    o->svga_addr = m->svga.overlay.addr;
    o->svga_pitch = m->svga.overlay.pitch;
    o->overlay_base = m->overlay_base;
    o->uv_addr = m->overlay_uv_addr;
    o->format = (uint8_t)m->scaler_format;
    o->svga_ena = (uint8_t)!!m->svga.overlay.ena;
}

static void
r3d_overlay_history_capture(mach64_t *m, uint64_t access_seq,
                            char op, unsigned width, uint32_t addr,
                            uint32_t value, int claimed, int force)
{
    r3d_overlay_history_state_t *s;
    r3d_overlay_snapshot_t now;
    r3d_overlay_event_t *e;

    if (!m)
        return;
    s = r3d_overlay_history_find(m, 1);
    if (!s)
        return;

    r3d_overlay_snapshot(m, &now);
    if (!force && s->have_last && !memcmp(&now, &s->last, sizeof(now)))
        return;

    e = &s->events[s->seq % R3D_OVERLAY_HISTORY];
    memset(e, 0, sizeof(*e));
    e->seq = ++s->seq;
    e->access_seq = access_seq;
    e->op = (uint8_t)op;
    e->width = (uint8_t)width;
    e->claimed = (uint8_t)!!claimed;
    e->addr = addr;
    e->value = value;
    e->state = now;
    s->last = now;
    s->have_last = 1;
}

static void
r3d_overlay_history_reset(mach64_t *m)
{
    r3d_overlay_history_state_t *s = r3d_overlay_history_find(m, 1);

    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->mach64 = m;
    r3d_overlay_history_capture(m, 0, 'A', 0, 0, 0, 1, 1);
}

static void
r3d_overlay_history_append(mach64_t *m)
{
    r3d_overlay_history_state_t *s = r3d_overlay_history_find(m, 0);
    FILE *fp;
    uint64_t count;
    uint64_t start;

    if (!s || !s->seq)
        return;

    fp = fopen("rage2p_3d_debug.log", "a");
    if (!fp) {
        pclog("ATI Rage II+: unable to append overlay history to rage2p_3d_debug.log\n");
        return;
    }

    count = s->seq < R3D_OVERLAY_HISTORY ? s->seq : R3D_OVERLAY_HISTORY;
    start = s->seq - count + 1;
    fprintf(fp, "\n[overlay_history]\n");
    fprintf(fp, "# retained=%llu total=%llu\n",
            (unsigned long long)count, (unsigned long long)s->seq);
    fprintf(fp,
            "# seq access op width claimed addr value yx_start yx_end scale_inc "
            "scale_cntl height_width scaler_off0 scaler_off1 scaler_pitch format "
            "video_key_clr video_key_msk graphics_key_clr graphics_key_msk "
            "key_cntl video_fn graphics_fn cmp_mix legacy_off0 legacy_pitch0 "
            "svga_addr svga_pitch svga_ena overlay_base uv_addr\n");
    fprintf(fp,
            "# access/op/addr/value identify the trace event at which this state "
            "was first observed; F is the forced detach snapshot.\n");

    for (uint64_t seq = start; seq <= s->seq; seq++) {
        const r3d_overlay_event_t *e =
            &s->events[(seq - 1) % R3D_OVERLAY_HISTORY];
        const r3d_overlay_snapshot_t *o = &e->state;
        fprintf(fp,
                "%010llu %010llu %c %u %u %05X %08X "
                "%08X %08X %08X %08X %08X %08X %08X %08X %X "
                "%08X %08X %08X %08X %08X %X %X %X "
                "%08X %08X %08X %08X %u %08X %08X\n",
                (unsigned long long)e->seq,
                (unsigned long long)e->access_seq,
                e->op ? (char)e->op : '-', (unsigned)e->width,
                (unsigned)e->claimed, e->addr, e->value,
                o->yx_start, o->yx_end, o->scale_inc, o->scale_cntl,
                o->height_width, o->scaler_off0, o->scaler_off1,
                o->scaler_pitch, (unsigned)o->format,
                o->video_key_clr, o->video_key_msk,
                o->graphics_key_clr, o->graphics_key_msk, o->key_cntl,
                o->key_cntl & 0x7u, (o->key_cntl >> 4) & 0x7u,
                (o->key_cntl >> 8) & 0xfu,
                o->legacy_off0, o->legacy_pitch0,
                o->svga_addr, o->svga_pitch, (unsigned)o->svga_ena,
                o->overlay_base, o->uv_addr);
    }

    fclose(fp);
    pclog("ATI Rage II+: appended %llu retained overlay state changes "
          "(%llu total) to rage2p_3d_debug.log\n",
          (unsigned long long)count, (unsigned long long)s->seq);
    memset(s, 0, sizeof(*s));
}

static void
r3d_debug_reset(mach64_t *m)
{
    r3d_debug_reset_base(m);
    r3d_overlay_history_reset(m);
}

static void
r3d_debug_access(mach64_3d_ctx_t *ctx, char op, unsigned width,
                 uint32_t addr, uint32_t value, int claimed)
{
    r3d_debug_state_t *general;
    uint64_t access_seq = 0;

    r3d_debug_access_base(ctx, op, width, addr, value, claimed);
    if (!ctx || !ctx->mach64)
        return;
    general = r3d_debug_find(ctx->mach64, 0);
    if (general)
        access_seq = general->access_seq;
    r3d_overlay_history_capture(ctx->mach64, access_seq, op, width,
                                addr, value, claimed, 0);
}

static void
r3d_debug_dump(mach64_t *m)
{
    r3d_debug_state_t *general = r3d_debug_find(m, 0);
    uint64_t access_seq = general ? general->access_seq : 0;

    r3d_overlay_history_capture(m, access_seq, 'F', 0, 0, 0, 1, 1);
    r3d_debug_dump_base(m);
    r3d_overlay_history_append(m);
}
