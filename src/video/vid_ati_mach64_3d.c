/* ATI 3D RAGE / Rage II+ software renderer. */
#include "vid_ati_mach64_3d_part1.inc"
#include "vid_ati_mach64_3d_part2.inc"
#include "vid_ati_mach64_3d_part3.inc"
#include "vid_ati_mach64_3d_part4.inc"

/*
 * GTB control-register compatibility lives in vid_ati_mach64_gtb_hook.c.
 * The original helpers intentionally know only the 0x00..0xff control-file
 * offsets.  The Rage II+ MMIO shim, however, also sees GUI offsets 0x100..0x3ff.
 * Do not let those GUI addresses alias a control register with the same low
 * byte (e.g. 0x2d4 DP_MIX -> 0xd4 CUSTOM_MACRO_CNTL, 0x2d8 DP_SRC -> 0xd8,
 * or 0x1b4 SRC_CNTL -> 0xb4 bank-select readback).
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
    mach64_3d_trace_external(mach64, 'P', 1, 0x1000u + (uint32_t) addr,
                             requested_bar1, 1);

    /*
     * Replaying COMMAND is the established compatibility hook's safe way to
     * tear down/reinstall the active I/O handlers.  It also leaves the BAR
     * temporarily unmapped during normal PCI sizing probes when I/O decoding
     * is disabled by the guest.
     */
    mach64_pci_write_gtb_legacy_dispatch(func, PCI_REG_COMMAND, 1,
                                          mach64->pci_regs[PCI_REG_COMMAND], priv);
}
