/* ATI 3D RAGE / Rage II+ software renderer. */
#include "vid_ati_mach64_3d.h"

/* Keep the original attach/detach bodies from part1, then wrap them below so
 * the diagnostic state can be reset and dumped without changing the renderer's
 * public ABI. */
#define mach64_3d_attach mach64_3d_attach_base
#define mach64_3d_detach mach64_3d_detach_base
#include "vid_ati_mach64_3d_part1.inc"
#undef mach64_3d_attach
#undef mach64_3d_detach

#include "vid_ati_mach64_3d_debug.inc"
#include "vid_ati_mach64_3d_part2.inc"
#include "vid_ati_mach64_3d_line.inc"
#include "vid_ati_mach64_3d_part3.inc"

/*
 * Shaded-line commands can occur millions of register accesses away from the
 * beginning/end access windows in rage2p_3d_debug.log.  Keep an independent
 * recent-history ring for the actual Bresenham kicks so a bad LINESTRIP can be
 * diagnosed without per-pixel file I/O or relying on the generic access ring.
 */
#define R3D_LINE_DEBUG_ENTRIES 8192

typedef struct r3d_line_debug_entry_t {
    uint64_t seq;
    uint64_t access_seq;
    uint32_t cmd_addr;
    uint32_t cmd;
    uint32_t dst_cntl;
    uint32_t dst_y_x_begin;
    uint32_t dst_y_x_end;
    uint32_t bres_err;
    uint32_t bres_inc;
    uint32_t bres_dec;
    uint32_t dp_src;
    uint32_t scale_3d_cntl;
    uint32_t dst_off_pitch;
    uint32_t sc_left_right;
    uint32_t sc_top_bottom;
    uint32_t z_cntl;
    uint32_t z_off_pitch;
    uint32_t dp_pix_width;
    uint32_t write_mask;
    uint32_t fifo_entries;
    int x_begin;
    int y_begin;
    int x_end;
    int y_end;
    int length;
    int x_dir;
    int y_dir;
    int y_major;
    int zero_negative;
    int last_pel;
    int line_disable;
    int claimed;
} r3d_line_debug_entry_t;

typedef struct r3d_line_debug_state_t {
    mach64_t *mach64;
    uint64_t seq;
    r3d_line_debug_entry_t entries[R3D_LINE_DEBUG_ENTRIES];
} r3d_line_debug_state_t;

static r3d_line_debug_state_t r3d_line_debug_states[MACH64_3D_CONTEXTS];

static r3d_line_debug_state_t *
r3d_line_debug_find(mach64_t *m, int create)
{
    r3d_line_debug_state_t *free_state = NULL;

    if (!m)
        return NULL;
    for (unsigned i = 0; i < MACH64_3D_CONTEXTS; i++) {
        if (r3d_line_debug_states[i].mach64 == m)
            return &r3d_line_debug_states[i];
        if (!r3d_line_debug_states[i].mach64 && !free_state)
            free_state = &r3d_line_debug_states[i];
    }
    if (!create || !free_state)
        return NULL;
    memset(free_state, 0, sizeof(*free_state));
    free_state->mach64 = m;
    return free_state;
}

static void
r3d_line_debug_reset(mach64_t *m)
{
    r3d_line_debug_state_t *s = r3d_line_debug_find(m, 1);

    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->mach64 = m;
}

static r3d_line_debug_entry_t *
r3d_line_debug_begin(mach64_3d_ctx_t *ctx, uint32_t cmd_addr, uint32_t cmd)
{
    mach64_t *m;
    r3d_line_debug_state_t *s;
    r3d_line_debug_entry_t *e;
    r3d_debug_state_t *general;

    if (!ctx || !ctx->mach64)
        return NULL;
    m = ctx->mach64;
    s = r3d_line_debug_find(m, 1);
    if (!s)
        return NULL;

    e = &s->entries[s->seq % R3D_LINE_DEBUG_ENTRIES];
    memset(e, 0, sizeof(*e));
    e->seq = ++s->seq;
    general = r3d_debug_find(m, 0);
    e->access_seq = general ? general->access_seq : 0;
    e->cmd_addr = cmd_addr;
    e->cmd = cmd;
    e->dst_cntl = m->dst_cntl;
    e->dst_y_x_begin = m->dst_y_x;
    e->bres_err = m->dst_bres_err;
    e->bres_inc = m->dst_bres_inc;
    e->bres_dec = m->dst_bres_dec;
    e->dp_src = m->dp_src;
    e->scale_3d_cntl = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    e->dst_off_pitch = m->dst_off_pitch;
    e->sc_left_right = m->sc_left_right;
    e->sc_top_bottom = m->sc_top_bottom;
    e->z_cntl = ctx->regs[R3D_Z_CNTL >> 2];
    e->z_off_pitch = ctx->regs[R3D_Z_OFF_PITCH >> 2];
    e->dp_pix_width = m->dp_pix_width;
    e->write_mask = m->write_mask;
    e->fifo_entries = m->fifo_write_idx - m->fifo_read_idx;
    e->x_begin = r3d_sign_extend(m->dst_y_x >> 16, 13);
    e->y_begin = r3d_sign_extend(m->dst_y_x, 15);
    e->x_end = e->x_begin;
    e->y_end = e->y_begin;
    e->length = (int) (cmd & 0x7fffu);
    e->x_dir = (m->dst_cntl & DST_X_DIR) ? 1 : -1;
    e->y_dir = (m->dst_cntl & DST_Y_DIR) ? 1 : -1;
    e->y_major = !!(m->dst_cntl & DST_Y_MAJOR);
    e->zero_negative = r3d_line_zero_negative(m, e->y_major);
    e->last_pel = !!(m->dst_cntl & DST_LAST_PEL);
    e->line_disable = !!(cmd & R3D_LINE_DISABLE);
    return e;
}

static void
r3d_line_debug_end(mach64_3d_ctx_t *ctx, r3d_line_debug_entry_t *e,
                   int claimed)
{
    if (!ctx || !ctx->mach64 || !e)
        return;

    e->dst_y_x_end = ctx->mach64->dst_y_x;
    e->x_end = r3d_sign_extend(ctx->mach64->dst_y_x >> 16, 13);
    e->y_end = r3d_sign_extend(ctx->mach64->dst_y_x, 15);
    e->claimed = claimed;
}

static void
r3d_line_debug_dump(mach64_t *m)
{
    r3d_line_debug_state_t *s = r3d_line_debug_find(m, 0);
    FILE *fp;
    uint64_t count;
    uint64_t start;

    if (!s || !s->seq)
        return;

    fp = fopen("rage2p_line_debug.log", "w");
    if (!fp) {
        pclog("ATI Rage II+: unable to create rage2p_line_debug.log\n");
        return;
    }

    fprintf(fp, "# Rage II+ shaded Bresenham line diagnostic v1\n");
    fprintf(fp, "# total=%llu retained=%u\n",
            (unsigned long long)s->seq,
            (unsigned)(s->seq < R3D_LINE_DEBUG_ENTRIES ?
                       s->seq : R3D_LINE_DEBUG_ENTRIES));
    fprintf(fp,
            "# seq access addr cmd len disable claimed dst_cntl "
            "x0 y0 x1 y1 xdir ydir ymajor zero_neg lastpel "
            "err inc dec dp_src ctl dst_y_x0 dst_y_x1 dst_off_pitch "
            "sc_lr sc_tb z_cntl z_off dp_pix write_mask fifo\n");

    count = s->seq < R3D_LINE_DEBUG_ENTRIES ?
            s->seq : R3D_LINE_DEBUG_ENTRIES;
    start = s->seq - count + 1;
    for (uint64_t seq = start; seq <= s->seq; seq++) {
        const r3d_line_debug_entry_t *e =
            &s->entries[(seq - 1) % R3D_LINE_DEBUG_ENTRIES];
        fprintf(fp,
                "%010llu %010llu %04X %08X %d %d %d %08X "
                "%d %d %d %d %d %d %d %d %d "
                "%08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X %u\n",
                (unsigned long long)e->seq,
                (unsigned long long)e->access_seq,
                e->cmd_addr, e->cmd, e->length, e->line_disable, e->claimed,
                e->dst_cntl,
                e->x_begin, e->y_begin, e->x_end, e->y_end,
                e->x_dir, e->y_dir, e->y_major, e->zero_negative,
                e->last_pel,
                e->bres_err, e->bres_inc, e->bres_dec,
                e->dp_src, e->scale_3d_cntl,
                e->dst_y_x_begin, e->dst_y_x_end, e->dst_off_pitch,
                e->sc_left_right, e->sc_top_bottom,
                e->z_cntl, e->z_off_pitch, e->dp_pix_width,
                e->write_mask, e->fifo_entries);
    }

    fclose(fp);
    pclog("ATI Rage II+: wrote rage2p_line_debug.log (%llu line commands)\n",
          (unsigned long long)count);
    memset(s, 0, sizeof(*s));
}

/*
 * Keep the established part4 register decoder as the base implementation, then
 * wrap its shared DST_BRES_LNTH entry points below.  GT/GTB 3D lines use the
 * normal Bresenham trajectory register but may select Scaler/3D data as the
 * foreground pixel source; that source does not exist in the legacy 2D blitter.
 */
#define mach64_3d_write mach64_3d_write_base
#include "vid_ati_mach64_3d_part4.inc"
#undef mach64_3d_write

int
mach64_3d_write(mach64_t *m, uint32_t a, uint32_t v, uint32_t type)
{
    mach64_3d_ctx_t *ctx = r3d_find(m);

    if (ctx) {
        uint32_t aa = a & 0x3ffu;
        uint32_t b = aa & ~3u;

        /*
         * 3D RAGE exposes the lead/Bresenham length register at both MM
         * offsets 0_48 and 0_51 (byte offsets 0x120 and 0x144).  part4 already
         * mirrors both addresses into the same shadow for trapezoids.  Shaded
         * line commands must be intercepted at both aliases too; otherwise a
         * line written through 0x144 is consumed as GT-only state without ever
         * starting the line walker, leaving holes in connected LINESTRIPs.
         */
        if (b == R3D_LEAD_BRES_LNTH || b == R3D_LEAD_BRES_LNTH_ALIAS) {
            uint32_t i = R3D_LEAD_BRES_LNTH >> 2;
            uint32_t cmd = r3d_merge_write(ctx->regs[i], aa, v, type);

            /*
             * For byte/word command programming, let incomplete writes keep
             * following the ordinary shared-register path.  The final write is
             * claimed only after the full command selects the 3D shaded-line
             * source.  This prevents the legacy 2D line engine from consuming
             * source selector 5 and substituting black.
             */
            if (r3d_write_complete(aa, type) &&
                r3d_is_shaded_line_command(ctx, cmd)) {
                unsigned width = r3d_fifo_width(type);
                r3d_line_debug_entry_t *line_dbg;
                int line_claimed;

                r3d_trace_record(m, 'W', width, aa, v, 0);
                r3d_debug_access(ctx, 'W', width, aa, v, 0);

                ctx->regs[i] = cmd;
                m->dst_bres_lnth = r3d_merge_write(m->dst_bres_lnth,
                                                    aa, v, type);

                r3d_sync_legacy_fifo(ctx);
                r3d_trace_record(m, 'C', width, b, cmd, 1);
                r3d_debug_access(ctx, 'C', width, b, cmd, 1);
                r3d_trace_record(m, 'L', width, b, cmd, 1);
                r3d_debug_access(ctx, 'L', width, b, cmd, 1);
                line_dbg = r3d_line_debug_begin(ctx, b, cmd);
                line_claimed = r3d_draw_shaded_line(ctx, cmd);
                r3d_line_debug_end(ctx, line_dbg, line_claimed);
                return 1;
            }
        }
    }

    return mach64_3d_write_base(m, a, v, type);
}

void
mach64_3d_attach(mach64_t *mach64)
{
    mach64_3d_attach_base(mach64);
    r3d_debug_reset(mach64);
    r3d_line_debug_reset(mach64);
}

void
mach64_3d_detach(mach64_t *mach64)
{
    r3d_line_debug_dump(mach64);
    r3d_debug_dump(mach64);
    mach64_3d_detach_base(mach64);
}

/*
 * GTB control-register compatibility lives in vid_ati_mach64_gtb_hook.c.
 * The original helpers intentionally know only the 0x00..0xff control-file
 * offsets.  The Rage II+ MMIO shim, however, also sees GUI offsets 0x100..0x3ff.
 * Do not let those GUI addresses alias a control register with the same low
 * byte (e.g. 0x2d4 DP_MIX -> 0xd4 CUSTOM_MACRO_CNTL, 0x2d8 DP_SRC -> 0xd8,
 * or 0x1b4 SRC_CNTL -> bank-select readback).
 */
extern int mach64_gtb_cfg_readb(mach64_t *mach64, uint32_t addr, uint8_t *val);
extern int mach64_gtb_cfg_writeb(mach64_t *mach64, uint32_t addr, uint8_t val);

int
mach64_gtb_cfg_readb_block0(mach64_t *mach64, uint32_t addr, uint8_t *val)
{
    if (addr & 0x300u)
        return 0;
    return mach64_gtb_cfg_readb(mach64, addr, val);
}

int
mach64_gtb_cfg_writeb_block0(mach64_t *mach64, uint32_t addr, uint8_t val)
{
    if (addr & 0x300u)
        return 0;
    return mach64_gtb_cfg_writeb(mach64, addr, val);
}

/*
 * Rage II+ reuses the mature VT2 initializer, but it must not enter the core as
 * MACH64_VT2.  The core has one hardware-visible VT/VT2 special case in
 * CONFIG_CNTL aperture-base readback; GT/GTB belongs to the modern integrated
 * layout.  Keep a persistent copy of the device descriptor because svga/device
 * initialization is allowed to retain descriptor pointers beyond this call.
 */
extern const device_t mach64vt2_device;
static device_t mach64gtb_core_info;

static void *
mach64gtb_core_init(const device_t *info)
{
    mach64gtb_core_info = *info;
    mach64gtb_core_info.local = (mach64gtb_core_info.local & ~0xffu) | MACH64_GTB;
    return mach64vt2_device.init(&mach64gtb_core_info);
}

const device_t mach64gtb_core_device = {
    .name = "Mach64 GTB core proxy",
    .internal_name = "mach64_gtb_core_proxy",
    .init = mach64gtb_core_init
};

/*
 * The Rage II+ integration layer owns GTB/3D reads but forwards ordinary
 * Mach64 register reads to the mature legacy reader.  Trace that final leg as
 * lower-case 'r'; previous traces stopped at the last PIO chip-ID read and
 * could not show a subsequent FIFO_STAT/GUI_STAT MMIO polling loop.
 */
uint8_t
mach64_ext_readb_gtb_trace(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint8_t ret = mach64_ext_readb(addr, priv);

    if (mach64 && mach64->pci_id == 0x4755)
        mach64_3d_trace_external(mach64, 'r', 1, addr, ret, 0);
    return ret;
}

uint16_t
mach64_ext_readw_gtb_trace(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint16_t ret = mach64_ext_readw(addr, priv);

    if (mach64 && mach64->pci_id == 0x4755)
        mach64_3d_trace_external(mach64, 'r', 2, addr, ret, 0);
    return ret;
}

uint32_t
mach64_ext_readl_gtb_trace(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t ret = mach64_ext_readl(addr, priv);

    if (mach64 && mach64->pci_id == 0x4755)
        mach64_3d_trace_external(mach64, 'r', 4, addr, ret, 0);
    return ret;
}

/*
 * The Mach64 block-decoded PIO register file occupies 0x100 bytes.  The VT2
 * PCI writer historically masks BAR1 with 0xfc00 while processing the upper
 * BAR bytes, which is a 1 KiB alignment and discards address bits 8 and 9.
 *
 * ARS2D can POST through sparse I/O, so that bug is mostly invisible until a
 * Windows driver switches the GTB to its PCI block-I/O BAR.  At that point the
 * guest can program CRTC registers at the PCI-reported base while 86Box has
 * the handler installed at a different base; the draw-engine MMIO can still
 * be reached, leaving an especially confusing black packed-pixel mode.
 *
 * Keep the mature VT2 PCI implementation for all side effects, but preserve
 * the exact GTB BAR1 value requested by the guest at the correct 0x100-byte
 * granularity and remap after the legacy writer has run.
 */
extern void mach64_pci_write_gtb_legacy_dispatch(int func, int addr, int len,
                                                  uint8_t val, void *priv);

void
mach64_pci_write_gtb_bar_dispatch(int func, int addr, int len,
                                  uint8_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t requested_bar1 = mach64 ? mach64->block_decoded_io : 0;
    int bar1_write = 0;

    if (mach64 && mach64->pci_id == 0x4755) {
        switch (addr) {
            case PCI_REG_BAR1_BYTE0:
                bar1_write = 1;
                break;
            case PCI_REG_BAR1_BYTE1:
                requested_bar1 = (requested_bar1 & 0xffff00ffu) |
                                 ((uint32_t) val << 8);
                bar1_write = 1;
                break;
            case PCI_REG_BAR1_BYTE2:
                requested_bar1 = (requested_bar1 & 0xff00ffffu) |
                                 ((uint32_t) val << 16);
                bar1_write = 1;
                break;
            case PCI_REG_BAR1_BYTE3:
                requested_bar1 = (requested_bar1 & 0x00ffffffu) |
                                 ((uint32_t) val << 24);
                bar1_write = 1;
                break;
            default:
                break;
        }
        requested_bar1 &= 0xffffff00u;
    }

    mach64_pci_write_gtb_legacy_dispatch(func, addr, len, val, priv);

    if (!mach64 || mach64->pci_id != 0x4755 || !bar1_write)
        return;

    mach64->block_decoded_io = requested_bar1;

    /*
     * Trace convention: op 'P', addr 0x1014..0x1017 (PCI BAR1 bytes), value
     * is the reconstructed 256-byte-aligned BAR1 after that byte write.
     */
    mach64_3d_trace_external(mach64, 'P', 1, 0x1000u + (uint32_t) addr,
                             requested_bar1, 1);

    /*
     * Replaying COMMAND is the established compatibility hook's safe way to
     * tear down/reinstall the active I/O handlers.  Do it only when I/O space
     * is currently enabled: during PCI BAR sizing probes the guest commonly
     * disables I/O decoding first, so there is deliberately no live handler
     * to remap at that point.
     */
    if (mach64->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_IO)
        mach64_pci_write_gtb_legacy_dispatch(func, PCI_REG_COMMAND, 1,
                                              mach64->pci_regs[PCI_REG_COMMAND], priv);
}
