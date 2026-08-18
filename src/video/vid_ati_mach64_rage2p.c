/*
 * ATI 3D Rage II+ DVD (Mach64 GTB/GU) integration shim.
 *
 * The mature Mach64 VT2 implementation supplies VGA, CRTC, PCI, 2D and
 * video-overlay behavior.  This shim changes the PCI/chip identity to GU,
 * supplies GTB-only POST/control register behavior and inserts the GT 3D
 * command decoder in front of the legacy Mach64 FIFO.
 */
#include "vid_ati_mach64_3d.h"

/*
 * ARS2D.bin is a complete ATI PCI option-ROM image for 1002:4755.  Its ROM
 * header declares 0x48 512-byte blocks = 36 KiB.  Allocate a 64 KiB backing
 * buffer so the ROM mask stays a power of two, but expose only the declared
 * 36 KiB image to the guest.
 */
#define BIOS_ROMGTB_PATH         "roms/video/mach64/ARS2D.bin"
#define BIOS_ROMGTB_IMAGE_SIZE   0x9000u
#define BIOS_ROMGTB_MAP_SIZE     BIOS_ROMGTB_IMAGE_SIZE
#define BIOS_ROMGTB_BACKING_SIZE 0x10000u
#define MACH64_GTB_PCI_ID        0x4755u

extern void mach64_queue_legacy(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type);
extern uint8_t mach64_pci_read_legacy(int func, int addr, int len, void *priv);
extern void mach64_pci_write_legacy(int func, int addr, int len, uint8_t val, void *priv);
extern void mach64_close(void *priv);
extern void mach64_speed_changed(void *priv);
extern void mach64_force_redraw(void *priv);
extern const device_t mach64vt2_device;
extern mach64_t *reset_state[2];

/* GTB-only non-GUI register latches implemented by vid_ati_mach64_gtb_hook.c. */
extern int mach64_gtb_cfg_readb(mach64_t *mach64, uint32_t addr, uint8_t *val);
extern int mach64_gtb_cfg_writeb(mach64_t *mach64, uint32_t addr, uint8_t val);

static uint16_t
mach64rage2p_le16(const uint8_t *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static int
mach64rage2p_contains(const uint8_t *buf, size_t len, const char *needle)
{
    const size_t needle_len = strlen(needle);

    if (!needle_len || needle_len > len)
        return 0;

    for (size_t i = 0; i <= len - needle_len; i++) {
        if (!memcmp(buf + i, needle, needle_len))
            return 1;
    }

    return 0;
}

void
mach64_queue(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type)
{
    if (!mach64_3d_write(mach64, addr, val, type))
        mach64_queue_legacy(mach64, addr, val, type);
}

/*
 * GU/GTB has its own PCI revision and an extra IOCONFIG disable bit that the
 * VT2 core does not preserve.  Keep all other PCI behavior in the legacy core.
 */
static uint8_t
mach64rage2p_pci_read(int func, int addr, int len, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;

    if (mach64->pci_id == MACH64_GTB_PCI_ID) {
        if (addr == PCI_REG_REVISION)
            return 0x9a;
        if (addr == MACH64_PCI_IOCONFIG)
            return mach64_gtb_pci_ioconfig_read(priv);
    }

    return mach64_pci_read_legacy(func, addr, len, priv);
}

/*
 * The legacy writer performs ordinary BAR/command handling.  Preserve the
 * complete GTB IOCONFIG low nibble before forwarding because legacy VT2 keeps
 * only bits 0..2.  ARS2D is 36 KiB, so correct the ROM mapping after legacy
 * code applies its fixed 32 KiB mapping.
 */
static void
mach64rage2p_pci_write(int func, int addr, int len, uint8_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;

    if (mach64->pci_id == MACH64_GTB_PCI_ID && addr == MACH64_PCI_IOCONFIG)
        mach64_gtb_pci_ioconfig_write(priv, val);

    mach64_pci_write_legacy(func, addr, len, val, priv);

    if (mach64->pci_id != MACH64_GTB_PCI_ID)
        return;

    if ((addr != PCI_REG_ROM_BAR_BYTE0) &&
        (addr != PCI_REG_ROM_BAR_BYTE1) &&
        (addr != PCI_REG_ROM_BAR_BYTE2) &&
        (addr != PCI_REG_ROM_BAR_BYTE3))
        return;

    if (mach64->pci_regs[PCI_REG_ROM_BAR_BYTE0] & 0x01) {
        uint32_t biosaddr = ((uint32_t) mach64->pci_regs[PCI_REG_ROM_BAR_BYTE2] << 16) |
                            ((uint32_t) mach64->pci_regs[PCI_REG_ROM_BAR_BYTE3] << 24);
        mem_mapping_set_addr(&mach64->bios_rom.mapping, biosaddr, BIOS_ROMGTB_MAP_SIZE);
    }
}

/*
 * vid_ati_mach64.c is compiled with pci_add_card renamed to this dispatcher.
 * The callbacks themselves remain shared with all Mach64 variants; the GTB
 * special cases are gated by pci_id after Rage II+ initialization.
 */
void
mach64_pci_add_card_dispatch(uint8_t add_type,
                             uint8_t (*read)(int func, int addr, int len, void *priv),
                             void (*write)(int func, int addr, int len, uint8_t val, void *priv),
                             void *priv, uint8_t *slot)
{
    (void) read;
    (void) write;
    pci_add_card(add_type, mach64rage2p_pci_read, mach64rage2p_pci_write, priv, slot);
}

/*
 * GTB has both non-GUI control registers absent from VT2 and GUI 3D registers.
 * Read them before falling back to the mature VT2 MMIO implementation.
 */
static uint8_t
mach64rage2p_mmio_readb(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t reg;
    uint8_t val;

    if (mach64_gtb_cfg_readb(mach64, addr, &val))
        return val;

    if (mach64_3d_read(mach64, addr, &reg))
        return (reg >> ((addr & 3) * 8)) & 0xff;

    return mach64_ext_readb(addr, priv);
}

static uint16_t
mach64rage2p_mmio_readw(uint32_t addr, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint32_t reg;
    uint8_t val;

    if (mach64_gtb_cfg_readb(mach64, addr, &val) ||
        mach64_gtb_cfg_readb(mach64, addr + 1, &val)) {
        return (uint16_t) mach64rage2p_mmio_readb(addr, priv) |
               ((uint16_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8);
    }

    if (mach64_3d_read(mach64, addr, &reg)) {
        unsigned lane = addr & 3;
        if (lane <= 2)
            return (reg >> (lane * 8)) & 0xffff;

        return (uint16_t) mach64rage2p_mmio_readb(addr, priv) |
               ((uint16_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8);
    }

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
    uint8_t val;

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_cfg_readb(mach64, addr + i, &val)) {
            return (uint32_t) mach64rage2p_mmio_readb(addr, priv) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 1, priv) << 8) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 2, priv) << 16) |
                   ((uint32_t) mach64rage2p_mmio_readb(addr + 3, priv) << 24);
        }
    }

    if (((addr & 3) == 0) && mach64_3d_read(mach64, addr, &reg))
        return reg;

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
mach64rage2p_mmio_writeb(uint32_t addr, uint8_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;

    if (mach64_gtb_cfg_writeb(mach64, addr, val))
        return;
    mach64_ext_writeb(addr, val, priv);
}

static void
mach64rage2p_mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint8_t probe;

    if (mach64_gtb_cfg_readb(mach64, addr, &probe) ||
        mach64_gtb_cfg_readb(mach64, addr + 1, &probe)) {
        mach64rage2p_mmio_writeb(addr, val & 0xff, priv);
        mach64rage2p_mmio_writeb(addr + 1, val >> 8, priv);
        return;
    }
    mach64_ext_writew(addr, val, priv);
}

static void
mach64rage2p_mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    uint8_t probe;

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_cfg_readb(mach64, addr + i, &probe)) {
            for (unsigned b = 0; b < 4; b++)
                mach64rage2p_mmio_writeb(addr + b, (val >> (b * 8)) & 0xff, priv);
            return;
        }
    }
    mach64_ext_writel(addr, val, priv);
}

static void
mach64rage2p_install_mmio_handlers(mach64_t *mach64)
{
    mem_mapping_set_handler(&mach64->mmio_mapping,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64rage2p_mmio_writeb,
                            mach64rage2p_mmio_writew,
                            mach64rage2p_mmio_writel);
    mem_mapping_set_handler(&mach64->mmio_linear_mapping,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64rage2p_mmio_writeb,
                            mach64rage2p_mmio_writew,
                            mach64rage2p_mmio_writel);
    mem_mapping_set_handler(&mach64->mmio_linear_mapping_2,
                            mach64rage2p_mmio_readb,
                            mach64rage2p_mmio_readw,
                            mach64rage2p_mmio_readl,
                            mach64rage2p_mmio_writeb,
                            mach64rage2p_mmio_writew,
                            mach64rage2p_mmio_writel);
}

/*
 * Validate the exact properties the emulated board relies on instead of merely
 * checking that a file exists.  This rejects truncated carves and unrelated
 * ATI ROMs before they can select a wrong Windows acceleration path.
 */
static int
mach64rage2p_vbios_valid(void)
{
    FILE *fp = rom_fopen(BIOS_ROMGTB_PATH, "rb");
    uint8_t *rom;
    long size;
    uint16_t pcir;
    unsigned checksum = 0;
    int valid = 0;

    if (!fp)
        return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    size = ftell(fp);
    if (size != BIOS_ROMGTB_IMAGE_SIZE || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    rom = malloc(BIOS_ROMGTB_IMAGE_SIZE);
    if (!rom) {
        fclose(fp);
        return 0;
    }

    if (fread(rom, 1, BIOS_ROMGTB_IMAGE_SIZE, fp) != BIOS_ROMGTB_IMAGE_SIZE)
        goto done;

    if (rom[0] != 0x55 || rom[1] != 0xaa)
        goto done;
    if (((uint32_t) rom[2] << 9) != BIOS_ROMGTB_IMAGE_SIZE)
        goto done;

    pcir = mach64rage2p_le16(rom + 0x18);
    if ((uint32_t) pcir + 0x18u > BIOS_ROMGTB_IMAGE_SIZE)
        goto done;
    if (memcmp(rom + pcir, "PCIR", 4))
        goto done;
    if (mach64rage2p_le16(rom + pcir + 4) != 0x1002 ||
        mach64rage2p_le16(rom + pcir + 6) != 0x4755)
        goto done;
    if (((uint32_t) mach64rage2p_le16(rom + pcir + 0x10) << 9) != BIOS_ROMGTB_IMAGE_SIZE)
        goto done;

    for (size_t i = 0; i < BIOS_ROMGTB_IMAGE_SIZE; i++)
        checksum += rom[i];
    if (checksum & 0xff)
        goto done;

    if (!mach64rage2p_contains(rom, BIOS_ROMGTB_IMAGE_SIZE, "MACH64GU") ||
        !mach64rage2p_contains(rom, BIOS_ROMGTB_IMAGE_SIZE, "SGRAM"))
        goto done;

    valid = 1;

done:
    free(rom);
    fclose(fp);
    return valid;
}

static int
mach64rage2p_available(void)
{
    return mach64rage2p_vbios_valid();
}

static int
mach64rage2p_install_vbios(mach64_t *mach64)
{
    uint8_t *new_rom;

    if (mach64->bios_rom.rom) {
        /* VT2 successfully created the original mapping; retain that mapping. */
        new_rom = realloc(mach64->bios_rom.rom, BIOS_ROMGTB_BACKING_SIZE);
        if (!new_rom)
            return 0;

        mach64->bios_rom.rom = new_rom;
        memset(new_rom, 0xff, BIOS_ROMGTB_BACKING_SIZE);
        if (!rom_load_linear(BIOS_ROMGTB_PATH, 0, BIOS_ROMGTB_IMAGE_SIZE, 0, new_rom))
            return 0;

        mach64->bios_rom.sz   = BIOS_ROMGTB_MAP_SIZE;
        mach64->bios_rom.mask = BIOS_ROMGTB_BACKING_SIZE - 1;
        mach64->bios_rom.mapping.size = BIOS_ROMGTB_MAP_SIZE;
        mem_mapping_set_exec(&mach64->bios_rom.mapping, new_rom);
        mem_mapping_disable(&mach64->bios_rom.mapping);
    } else {
        new_rom = calloc(1, BIOS_ROMGTB_BACKING_SIZE);
        if (!new_rom)
            return 0;
        memset(new_rom, 0xff, BIOS_ROMGTB_BACKING_SIZE);

        if (!rom_load_linear(BIOS_ROMGTB_PATH, 0, BIOS_ROMGTB_IMAGE_SIZE, 0, new_rom)) {
            free(new_rom);
            return 0;
        }

        mach64->bios_rom.rom  = new_rom;
        mach64->bios_rom.sz   = BIOS_ROMGTB_MAP_SIZE;
        mach64->bios_rom.mask = BIOS_ROMGTB_BACKING_SIZE - 1;

        mem_mapping_add(&mach64->bios_rom.mapping,
                        0xc0000, BIOS_ROMGTB_MAP_SIZE,
                        rom_read, rom_readw, rom_readl,
                        NULL, NULL, NULL,
                        new_rom, MEM_MAPPING_EXTERNAL | MEM_MAPPING_ROM_WS,
                        &mach64->bios_rom);
        mem_mapping_disable(&mach64->bios_rom.mapping);
    }

    ati_eeprom_load(&mach64->eeprom, "mach64rage2p_ars2d.nvr", 1);
    return 1;
}

static void *
mach64rage2p_init(const device_t *info)
{
    void *priv = mach64vt2_device.init(info);
    mach64_t *mach64 = (mach64_t *) priv;

    if (!mach64)
        return NULL;

    if (!mach64rage2p_install_vbios(mach64)) {
        mach64_close(priv);
        return NULL;
    }

    /* GU = Rage II / Rage II+ (GTB), UMC GT B2U2 chip revision. */
    mach64->pci_id         = MACH64_GTB_PCI_ID;
    mach64->config_chip_id = 0x5a004755;

    /* ARS2D identifies an SGRAM board.  GT/VT CNFG_STAT0 uses 5 for SGRAM. */
    mach64->config_stat0 = (mach64->config_stat0 & ~0x07u) | 0x05u;

    /* GTB uses a 4-bit memory-size encoding; 4 MiB is 0x7, not VT's 0x3. */
    mach64->mem_cntl = (mach64->mem_cntl & ~0x0fu) | 0x07u;

    /*
     * Attach GTB POST state before the system BIOS executes ARS2D: this installs
     * GENVS/GENENA fixed ports and a safe temporary VGA dot clock.
     */
    mach64_gtb_state_attach(mach64);
    mach64_3d_attach(mach64);
    mach64rage2p_install_mmio_handlers(mach64);

    /* Refresh VT2's reset template with the complete GTB identity and handlers. */
    if (reset_state[mach64->svga.monitor_index])
        *reset_state[mach64->svga.monitor_index] = *mach64;

    return mach64;
}

static void
mach64rage2p_close(void *priv)
{
    mach64_gtb_state_detach(priv);
    mach64_3d_detach((mach64_t *) priv);
    mach64_close(priv);
}

const device_t mach64rage2p_device = {
    .name          = "ATI 3D Rage II+ DVD (GTB/GU, 4 MB SGRAM)",
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
