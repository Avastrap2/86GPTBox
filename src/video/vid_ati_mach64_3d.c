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
 * byte (e.g. 0x2d4 DP_MIX -> 0xd4 CUSTOM_MACRO_CNTL).
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
 * vid_ati_mach64.c is force-compiled with svga_recalctimings redirected here.
 * This keeps the actual SVGA timing implementation unchanged while adding a
 * compact Rage II+ mode-set marker after every CRTC/PLL-related recalculation.
 * It captures PIO-driven mode changes that never pass through the custom MMIO
 * wrapper, which is essential for diagnosing Windows 9x high-colour modes.
 */
void
mach64_svga_recalctimings_dispatch(void *priv)
{
    svga_t *svga = (svga_t *) priv;
    mach64_t *mach64;

    svga_recalctimings(svga);
    if (!svga)
        return;

    mach64 = (mach64_t *) svga->priv;
    if (mach64 && mach64->pci_id == 0x4755)
        mach64_3d_trace_external(mach64, 'S', 4, 0x41c,
                                 mach64->crtc_gen_cntl, 0);
}
