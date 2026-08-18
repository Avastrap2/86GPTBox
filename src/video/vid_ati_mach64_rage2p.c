/*
 * ATI 3D Rage II+ DVD (Mach64 GTB/GU) integration shim.
 *
 * The mature Mach64 VT2 implementation supplies VGA, CRTC, PCI, 2D and
 * video-overlay behavior.  This shim changes the PCI/chip identity to GU and
 * inserts the GT 3D command decoder in front of the legacy Mach64 FIFO.
 */
#include "vid_ati_mach64_3d.h"

extern void mach64_queue_legacy(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type);
extern void mach64_close(void *priv);
extern void mach64_speed_changed(void *priv);
extern void mach64_force_redraw(void *priv);
extern int mach64vt2_available(void);
extern const device_t mach64vt2_device;

void
mach64_queue(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type)
{
    if (!mach64_3d_write(mach64, addr, val, type))
        mach64_queue_legacy(mach64, addr, val, type);
}

/*
 * The original VT2 MMIO reader has no cases for GT/GTB-only 3D registers and
 * therefore returns zero for them.  Windows MACH64.DRV probes/read-backs part
 * of this state, so keep the mature VT2 reader for ordinary registers but
 * source GT-only register values from the software 3D block.
 */
static uint8_t
mach64rage2p_mmio_readb(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t reg;

    if (mach64_3d_read(mach64, addr, &reg))
        return (reg >> ((addr & 3) * 8)) & 0xff;

    return mach64_ext_readb(addr, priv);
}

static uint16_t
mach64rage2p_mmio_readw(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t reg;

    /* Fast path for an aligned/contained GT register read. */
    if (mach64_3d_read(mach64, addr, &reg)) {
        unsigned lane = addr & 3;
        if (lane <= 2)
            return (reg >> (lane * 8)) & 0xffff;

        return (uint16_t) mach64rage2p_mmio_readb(addr, priv) |
               ((uint16_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8);
    }

    /* Catch the rare unaligned read that crosses into a GT-only register. */
    if (mach64_3d_read(mach64, addr + 1, &reg))
        return (uint16_t) mach64rage2p_mmio_readb(addr, priv) |
               ((uint16_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8);

    return mach64_ext_readw(addr, priv);
}

static uint32_t
mach64rage2p_mmio_readl(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t reg;

    if (((addr & 3) == 0) && mach64_3d_read(mach64, addr, &reg))
        return reg;

    /* Preserve the exact legacy DWORD behavior unless any lane is GT-only. */
    for (unsigned i = 0; i < 4; i++) {
        if (mach64_3d_read(mach64, addr + i, &reg)) {
            return (uint32_t) mach64rage2p_mmio_readb(addr, priv) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 2, priv) << 16) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 3, priv) << 24);
        }
    }

    return mach64_ext_readl(addr, priv);
}

static void
mach64rage2p_install_mmio_readback(mach64_t *mach64)
{
    /*
     * mach64_updatemapping() moves these mappings but does not replace their
     * handlers, so installing the wrappers once after VT2 initialization is
     * sufficient for the legacy BF000 aperture and both linear-MMIO tails.
     */
    mem_mapping_set_handler(&mach64->mmio_mapping,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64_ext_writeb,
                            mach64_ext_writew,
                            mach64_ext_writel);
    mem_mapping_set_handler(&mach64->mmio_linear_mapping,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64_ext_writeb,
                            mach64_ext_writew,
                            mach64_ext_writel);
    mem_mapping_set_handler(&mach64->mmio_linear_mapping_2,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64_ext_writeb,
                            mach64_ext_writew,
                            mach64_ext_writel);
}

static void *
mach64rage2p_init(const device_t *info)
{
    /*
     * Force the mature VT2 core to allocate 4 MiB, then expose the GTB/GU
     * identity used by ATI 3D Rage II+ boards.  GT/GTB are members of the
     * same Mach64 register family; the separate 3D module handles registers
     * which are aliases/reserved on VT2.
     */
    void *priv = mach64vt2_device.init(info);
    mach64_t *mach64 = (mach64_t *) priv;

    if (!mach64)
        return NULL;

    /* GU = Rage II / Rage II+ (GTB).  Pick a known UMC GT B2U2 revision. */
    mach64->pci_id = 0x4755;
    mach64->config_chip_id = 0x5a004755;

    /* GTB uses a 4-bit memory-size encoding; 4 MiB is 0x7, not VT's 0x3. */
    mach64->mem_cntl = (mach64->mem_cntl & ~0x0fu) | 0x07u;

    mach64_3d_attach(mach64);
    mach64rage2p_install_mmio_readback(mach64);
    return mach64;
}

static void
mach64rage2p_close(void *priv)
{
    mach64_3d_detach((mach64_t *) priv);
    mach64_close(priv);
}

const device_t mach64rage2p_device = {
    .name          = "ATI 3D Rage II+ DVD (GTB/GU, 4 MB)",
    .internal_name = "mach64_rage2p",
    .flags         = DEVICE_PCI,
    .local         = MACH64_VT2 | (1 << 20),
    .init          = mach64rage2p_init,
    .close         = mach64rage2p_close,
    .reset         = NULL,
    .available     = mach64vt2_available,
    .speed_changed = mach64_speed_changed,
    .force_redraw  = mach64_force_redraw,
    .config        = NULL
};
