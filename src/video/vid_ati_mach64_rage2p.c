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

    mach64->pci_id = 0x4755;          /* ATI PCI_CHIP_MACH64GU */
    mach64->config_chip_id = 0x00004755;
    mach64_3d_attach(mach64);
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
