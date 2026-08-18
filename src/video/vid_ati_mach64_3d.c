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
