/*
 * ATI 3D Rage II+ DVD (Mach64 GTB/GU) integration shim.
 *
 * The mature Mach64 VT2 implementation supplies VGA, CRTC, PCI, 2D and
 * video-overlay behavior.  This shim changes the PCI/chip identity to GU and
 * inserts the GT 3D command decoder in front of the legacy Mach64 FIFO.
 */
#include "vid_ati_mach64_3d.h"

/*
 * Do not boot a GU/GTB board with the repository's VT2 option ROM.  Windows
 * display drivers consume card/memory information from the video BIOS and a
 * mixed "GU PCI identity + VT2 BIOS" configuration can select the wrong
 * accelerated-driver path.  Keep the ROM external like the other 86Box video
 * BIOS images; no copyrighted ROM data is embedded here.
 *
 * The emulated board is the 4 MiB variant, so this image must be a 4 MiB
 * Rage II+ / GTB board BIOS.  A 64 KiB dump is acceptable; Mach64 exposes the
 * first 32 KiB option-ROM window here, matching the existing VT2 mapping.
 */
#define BIOS_ROMGTB_PATH "roms/video/mach64/ati3drage2plus_4mb.bin"

extern void mach64_queue_legacy(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type);
extern uint8_t mach64_pci_read_legacy(int func, int addr, int len, void *priv);
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
 * A real GU/GTB Rage II+ board reports PCI revision 0x9a.  The VT2 core we
 * reuse normally reports 0x40 for every non-GX Mach64.  Keep the legacy PCI
 * implementation for all registers/devices, but correct the Rage II+ revision
 * once the shim has changed pci_id to 0x4755.
 */
static uint8_t
mach64rage2p_pci_read(int func, int addr, int len, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;

    if ((addr == PCI_REG_REVISION) && (mach64->pci_id == 0x4755))
        return 0x9a;

    return mach64_pci_read_legacy(func, addr, len, priv);
}

/*
 * vid_ati_mach64.c is compiled with pci_add_card renamed to this dispatcher.
 * That lets us install the revision-aware read callback without invasive
 * changes to the mature Mach64 core.  Other Mach64 variants remain bit-for-bit
 * compatible through mach64_pci_read_legacy().
 */
void
mach64_pci_add_card_dispatch(uint8_t add_type,
                             uint8_t (*read)(int func, int addr, int len, void *priv),
                             void (*write)(int func, int addr, int len, uint8_t val, void *priv),
                             void *priv, uint8_t *slot)
{
    (void) read;
    pci_add_card(add_type, mach64rage2p_pci_read, write, priv, slot);
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

static int
mach64rage2p_vbios_valid(void)
{
    FILE *fp = rom_fopen(BIOS_ROMGTB_PATH, "rb");
    long size;

    if (!fp)
        return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    size = ftell(fp);
    fclose(fp);

    /* The mapped Mach64 PCI option-ROM window is 32 KiB. */
    return size >= 0x8000;
}

static int
mach64rage2p_available(void)
{
    /* VT2 is still used as the construction backend, then its ROM is replaced. */
    return mach64vt2_available() && mach64rage2p_vbios_valid();
}

static int
mach64rage2p_install_vbios(mach64_t *mach64)
{
    if (!mach64->bios_rom.rom || mach64->bios_rom.sz < 0x8000)
        return 0;

    /*
     * Reuse the already-registered option-ROM mapping and replace only its
     * backing bytes.  rom_load_linear() writes into the supplied buffer and
     * does not create a second mem_mapping_t, so the VT2 mapping cannot remain
     * accidentally linked beside the Rage II+ ROM.
     */
    memset(mach64->bios_rom.rom, 0xff, mach64->bios_rom.sz);
    if (!rom_load_linear(BIOS_ROMGTB_PATH, 0, 0x8000, 0, mach64->bios_rom.rom))
        return 0;

    /* Do not share persistent serial-EEPROM state with the VT2 card. */
    ati_eeprom_load(&mach64->eeprom, "mach64rage2p.nvr", 1);
    return 1;
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

    if (!mach64rage2p_install_vbios(mach64)) {
        mach64_close(priv);
        return NULL;
    }

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
    .available     = mach64rage2p_available,
    .speed_changed = mach64_speed_changed,
    .force_redraw  = mach64_force_redraw,
    .config        = NULL
};
