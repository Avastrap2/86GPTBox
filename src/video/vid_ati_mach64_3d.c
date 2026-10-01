/* ATI 3D RAGE / Rage II+ software renderer. */
#include "vid_ati_mach64_3d.h"

#include "vid_ati_mach64_3d_part1.inc"
#include "vid_ati_mach64_3d_part2.inc"
#include "vid_ati_mach64_3d_part3.inc"
#include "vid_ati_mach64_3d_line.inc"

/*
 * Keep the established part4 register decoder as the base implementation, then
 * wrap its shared DST_BRES_LNTH entry points below.  GT/GTB 3D lines use the
 * normal Bresenham trajectory register but may select Scaler/3D data as the
 * foreground pixel source; that source does not exist in the legacy 2D blitter.
 */
#define mach64_3d_write mach64_3d_write_base
#include "vid_ati_mach64_3d_part4.inc"
#undef mach64_3d_write

#include "vid_ati_mach64_3d_scaler.inc"

int
mach64_3d_write(mach64_t *m, uint32_t a, uint32_t v, uint32_t type)
{
    mach64_3d_ctx_t *ctx = r3d_find(m);

    if (ctx) {
        uint32_t aa = a & 0x3ffu;
        uint32_t b = aa & ~3u;

        if (r3d_try_scaler_destination_write(ctx, aa, v, type))
            return 1;

        /*
         * 3D RAGE exposes the lead/Bresenham length register at both MM
         * offsets 0_48 and 0_51 (byte offsets 0x120 and 0x144).  part4 already
         * mirrors both addresses into the same shadow for trapezoids.  3D line
         * commands must be intercepted at both aliases too; otherwise a
         * line written through 0x144 is consumed as GT-only state without ever
         * starting the line walker, leaving holes in connected LINESTRIPs.
         */
        if (b == R3D_LEAD_BRES_LNTH || b == R3D_LEAD_BRES_LNTH_ALIAS) {
            uint32_t i = R3D_LEAD_BRES_LNTH >> 2;
            uint32_t cmd = r3d_merge_write(ctx->regs[i], aa, v, type);
            unsigned fcn = (ctx->regs[R3D_SCALE_3D_CNTL >> 2] >> R3D_FCN_SHIFT) & 3u;

            /* DP_SRC and DST_CNTL may still be queued in the legacy FIFO.
             * Resolve that state before deciding whether this drawing line
             * belongs to the 3D or legacy path. Testing first can either
             * drop a 2D line or send a 3D line to the selector-5 fallback.
             * Ordinary 2D mode and non-drawing preloads need no extra wait. */
            if (r3d_write_complete(aa, type) &&
                !(cmd & (R3D_DRAW_TRAP | R3D_LINE_DISABLE)) &&
                (fcn == 2u || fcn == 3u))
                r3d_sync_legacy_fifo(ctx);

            /*
             * RRG-G02700, DST_BRES_LNTH (4-46): TRAIL_X is loaded for
             * line commands (bit 15 clear), including LINE_DIS preloads
             * which deliberately do not draw.  A trapezoid loads it only
             * when bit 31 is set; otherwise it keeps the live edge from
             * the preceding command.  Loading solely in the trapezoid
             * walker loses a no-draw preload and reuses a stale edge.
             * Handle both aliases here, before either the 3D line
             * interception or the legacy/shared-register fallback.
             * This shadow belongs to the synchronous 3D front end; keep
             * shared-register ordering at the existing draw barriers rather
             * than adding a FIFO wait to every ordinary 2D line write.
             */
            if (r3d_write_complete(aa, type) &&
                (!(cmd & R3D_DRAW_TRAP) || (cmd & R3D_LINE_DISABLE))) {
                ctx->trail_x = r3d_sign_extend(cmd >> 16, 13);
                ctx->trail_valid = 1;
            }

            /*
             * For byte/word command programming, let incomplete writes keep
             * following the ordinary shared-register path.  The final write is
             * claimed only after the full command selects the 3D line source.
             * This prevents the legacy 2D line engine from consuming source
             * selector 5 and substituting black.
             */
            if (r3d_write_complete(aa, type) &&
                r3d_is_3d_line_command(ctx, cmd)) {
                ctx->regs[i] = cmd;
                m->dst_bres_lnth = r3d_merge_write(m->dst_bres_lnth,
                                                    aa, v, type);

                r3d_sync_legacy_fifo(ctx);
                r3d_draw_3d_line(ctx, cmd);
                return 1;
            }
        }
    }

    return mach64_3d_write_base(m, a, v, type);
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

/* Forward ordinary GTB Mach64 register reads to the mature legacy reader. */
uint8_t
mach64_ext_readb_gtb_dispatch(uint32_t addr, void *priv)
{
    return mach64_ext_readb(addr, priv);
}

uint16_t
mach64_ext_readw_gtb_dispatch(uint32_t addr, void *priv)
{
    return mach64_ext_readw(addr, priv);
}

uint32_t
mach64_ext_readl_gtb_dispatch(uint32_t addr, void *priv)
{
    return mach64_ext_readl(addr, priv);
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
