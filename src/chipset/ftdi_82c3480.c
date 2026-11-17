/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the FTDI 82C3480 chipset (82C3481 and
 *          82C3482), as used on the IBM Aptiva 2144/2168 system board
 *          type C-1.
 *
 *          No datasheet for this chipset is known, so everything here
 *          comes from disassembling the Aptiva BIOS (IBM 96G0926, dated
 *          08/15/94). The registers sit at I/O ports FC22h-FC2Fh: FC22h
 *          is an index for the register arrays at FC23h, FC24h, FC25h
 *          and FC2Eh, the others are direct registers. What the BIOS
 *          shows of them:
 *
 *          - FC23h[0-7]: DRAM rows; rows 2k and 2k+1 are the two sides
 *            of SIMM socket k. Bits 7-6 are the row type (00 = off,
 *            01 = 1 MB, 10 = 4 MB, 11 = 16 MB), bits 5-0 its start in
 *            MB. The BIOS sizes the SIMMs through these registers.
 *          - FC2Eh[0-2]: shadow RAM for C0000h, D0000h and E0000h, a
 *            nibble for each 32 KB half; FC2Fh bits 1-0: F0000h.
 *          - FC24h[0-5], FC25h[0-2], FC26h-FC2Dh: function unknown, or
 *            external cache control (FC28h, FC2Bh, FC2Ch); stored only.
 *
 * Authors: Avastrap2, <110710503+Avastrap2@users.noreply.github.com>
 *
 *          Copyright 2026 Avastrap2.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/port_92.h>
#include <86box/chipset.h>
#include <86box/plat_unused.h>

#define FTDI_ROWS      8
#define FTDI_MAPPINGS  (FTDI_ROWS * 2)
#define FTDI_REG_INDEX 0x02 /* FC22h */
#define FTDI_REG_F_SHD 0x0f /* FC2Fh */

/* A part of a DRAM row in the address space: the row's part below A0000h
   or its part from 100000h up. */
typedef struct ftdi_82c3480_seg_t {
    uint32_t base;    /* Guest address of the part. */
    uint32_t ram_off; /* Where it is in ram[]. */
} ftdi_82c3480_seg_t;

typedef struct ftdi_82c3480_t {
    uint8_t regs[16]; /* The direct registers, by port - FC20h. */
    uint8_t rows[FTDI_ROWS];
    uint8_t unk24[6];
    uint8_t unk25[3];
    uint8_t shadow[3];

    uint32_t row_size[FTDI_ROWS]; /* Fitted size in MB, 0 if absent. */
    uint32_t row_ram[FTDI_ROWS];  /* Offset of the row in ram[]. */
    uint32_t total;               /* Bytes fitted in the SIMM sockets. */
    uint32_t int_top;             /* End of the internal state from 1 MB up. */

    ftdi_82c3480_seg_t seg[FTDI_MAPPINGS];
    mem_mapping_t      map[FTDI_MAPPINGS];
} ftdi_82c3480_t;

#ifdef ENABLE_FTDI_82C3480_LOG
int ftdi_82c3480_do_log = ENABLE_FTDI_82C3480_LOG;

static void
ftdi_82c3480_log(const char *fmt, ...)
{
    va_list ap;

    if (ftdi_82c3480_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define ftdi_82c3480_log(fmt, ...)
#endif

/* The row's size in MB for each type in bits 7-6. */
static const uint32_t ftdi_82c3480_type_mb[4] = { 0, 1, 4, 16 };

/*
   The row handlers. They run only while the BIOS sizes the memory or lays
   it out, which it does in ROM code without a stack; once every row is
   where the BIOS finally puts it, the stock RAM mappings take over. Code is
   assumed never to run from these mappings, so they keep no lookup entries
   and mark writes dirty only on the page of the RAM they reach, which is
   the page the same guest address uses once the layout is the identity.
 */
static uint8_t
ftdi_82c3480_ram_readb(uint32_t addr, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;

    return ram[seg->ram_off + (addr - seg->base)];
}

static uint16_t
ftdi_82c3480_ram_readw(uint32_t addr, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;

    return *(uint16_t *) &ram[seg->ram_off + (addr - seg->base)];
}

static uint32_t
ftdi_82c3480_ram_readl(uint32_t addr, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;

    return *(uint32_t *) &ram[seg->ram_off + (addr - seg->base)];
}

static void
ftdi_82c3480_ram_writeb(uint32_t addr, uint8_t val, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;
    uint32_t                  off = seg->ram_off + (addr - seg->base);

    if (cpu_use_exec)
        mem_write_ramb_page(off, val, &pages[off >> 12]);
    else
        ram[off] = val;
}

static void
ftdi_82c3480_ram_writew(uint32_t addr, uint16_t val, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;
    uint32_t                  off = seg->ram_off + (addr - seg->base);

    if (cpu_use_exec)
        mem_write_ramw_page(off, val, &pages[off >> 12]);
    else
        *(uint16_t *) &ram[off] = val;
}

static void
ftdi_82c3480_ram_writel(uint32_t addr, uint32_t val, void *priv)
{
    const ftdi_82c3480_seg_t *seg = (ftdi_82c3480_seg_t *) priv;
    uint32_t                  off = seg->ram_off + (addr - seg->base);

    if (cpu_use_exec)
        mem_write_raml_page(off, val, &pages[off >> 12]);
    else
        *(uint32_t *) &ram[off] = val;
}

/* Memory from 1 MB up answers only where the state is internal: keep it so
   up to top, and external above. */
static void
ftdi_82c3480_set_int_top(ftdi_82c3480_t *dev, uint32_t top)
{
    if (top > dev->int_top)
        mem_set_mem_state_both(dev->int_top, top - dev->int_top, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    else if (top < dev->int_top)
        mem_set_mem_state_both(top, dev->int_top - top, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);

    dev->int_top = top;
}

static void
ftdi_82c3480_map_seg(ftdi_82c3480_t *dev, int i, uint32_t base, uint32_t end, uint32_t ram_off)
{
    dev->seg[i].base    = base;
    dev->seg[i].ram_off = ram_off;

    mem_mapping_set_exec(&dev->map[i], &ram[ram_off]);
    mem_mapping_set_addr(&dev->map[i], base, end - base);
}

static void
ftdi_82c3480_recalc_rows(ftdi_82c3480_t *dev)
{
    int      identity = 1;
    uint32_t top      = 0x00100000;

    /* The identity: every fitted row on, as large as it is, where it is in ram[]. */
    for (uint8_t r = 0; r < FTDI_ROWS; r++) {
        if (dev->row_size[r] == 0)
            continue;

        if ((ftdi_82c3480_type_mb[dev->rows[r] >> 6] != dev->row_size[r]) ||
            (((uint32_t) (dev->rows[r] & 0x3f) << 20) != dev->row_ram[r]))
            identity = 0;
    }

    for (uint8_t i = 0; i < FTDI_MAPPINGS; i++)
        mem_mapping_disable(&dev->map[i]);

    if (identity) {
        mem_mapping_set_addr(&ram_low_mapping, 0x00000000, 0x000a0000);
        if (dev->total > 0x00100000)
            mem_mapping_set_addr(&ram_high_mapping, 0x00100000, dev->total - 0x00100000);
        else
            mem_mapping_disable(&ram_high_mapping);

        if (dev->total > top)
            top = dev->total;
    } else {
        mem_mapping_disable(&ram_low_mapping);
        mem_mapping_disable(&ram_high_mapping);

        for (uint8_t r = 0; r < FTDI_ROWS; r++) {
            uint32_t type_mb = ftdi_82c3480_type_mb[dev->rows[r] >> 6];
            uint32_t base;
            uint32_t end;
            uint32_t high;

            /* An absent row never answers, so reads there float high. */
            if ((dev->row_size[r] == 0) || (type_mb == 0))
                continue;

            /*
               A row set to a larger type than it is answers only in its
               first row_size MB here. On the real DRAM the rest of the
               window aliases the row; the BIOS counts the row's size
               either way, as it looks for its own address in each MB.
             */
            base = (uint32_t) (dev->rows[r] & 0x3f) << 20;
            end  = base + (((type_mb < dev->row_size[r]) ? type_mb : dev->row_size[r]) << 20);

            /* A0000h-FFFFFh is the video memory, the ROMs and the shadow RAM. */
            if (base < 0x000a0000)
                ftdi_82c3480_map_seg(dev, r << 1, base, (end < 0x000a0000) ? end : 0x000a0000,
                                     dev->row_ram[r]);

            high = (base > 0x00100000) ? base : 0x00100000;
            if (high < end)
                ftdi_82c3480_map_seg(dev, (r << 1) + 1, high, end, dev->row_ram[r] + (high - base));

            if (end > top)
                top = end;
        }
    }

    ftdi_82c3480_set_int_top(dev, top);

    ftdi_82c3480_log("FTDI 82C3480: Rows %02X %02X %02X %02X %02X %02X %02X %02X, %s\n",
                     dev->rows[0], dev->rows[1], dev->rows[2], dev->rows[3],
                     dev->rows[4], dev->rows[5], dev->rows[6], dev->rows[7],
                     identity ? "identity" : "own mappings");

    flushmmucache();
}

/* Bit 0 of a pair: reads come from the shadow RAM; bit 1: writes go to it. */
static int
ftdi_82c3480_shflags(uint8_t pair)
{
    return ((pair & 0x01) ? MEM_READ_INTERNAL : MEM_READ_EXTANY) |
           ((pair & 0x02) ? MEM_WRITE_INTERNAL : MEM_WRITE_EXTANY);
}

/*
   Each nibble of FC2Eh is a 32 KB half of its segment. The BIOS only ever
   writes a nibble as 0h (off), 5h (read RAM, write-protected) or Fh (read
   and write RAM), so taking a nibble as a pair of bits for each 16 KB block
   is an inference.
 */
static void
ftdi_82c3480_recalc_shadow(ftdi_82c3480_t *dev)
{
    uint32_t base;
    uint8_t  pair;

    shadowbios       = 0;
    shadowbios_write = 0;

    for (uint8_t i = 0; i < 3; i++) {
        for (uint8_t j = 0; j < 4; j++) {
            base = 0x000c0000 + (i << 16) + (j << 14);
            pair = (dev->shadow[i] >> (j << 1)) & 0x03;
            mem_set_mem_state(base, 0x4000, ftdi_82c3480_shflags(pair));
            if (base >= 0x000e0000) {
                shadowbios |= (pair & 0x01);
                shadowbios_write |= !!(pair & 0x02);
            }
        }
    }

    /* The BIOS uses 10b while it copies itself to RAM, then 01b. */
    pair = dev->regs[FTDI_REG_F_SHD] & 0x03;
    mem_set_mem_state(0x000f0000, 0x10000, ftdi_82c3480_shflags(pair));
    shadowbios |= (pair & 0x01);
    shadowbios_write |= !!(pair & 0x02);

    flushmmucache();
}

static void
ftdi_82c3480_write(uint16_t port, uint8_t val, void *priv)
{
    ftdi_82c3480_t *dev = (ftdi_82c3480_t *) priv;
    uint8_t         idx = dev->regs[FTDI_REG_INDEX];

    ftdi_82c3480_log("[%04X:%08X] FTDI 82C3480: [W] %04X[%02X] = %02X\n", CS, cpu_state.pc, port, idx, val);

    switch (port) {
        case 0xfc23:
            if (idx < FTDI_ROWS) {
                dev->rows[idx] = val;
                ftdi_82c3480_recalc_rows(dev);
            }
            break;

        case 0xfc24:
            if (idx < sizeof(dev->unk24))
                dev->unk24[idx] = val;
            break;

        case 0xfc25:
            if (idx < sizeof(dev->unk25))
                dev->unk25[idx] = val;
            break;

        case 0xfc2e:
            if (idx < sizeof(dev->shadow)) {
                dev->shadow[idx] = val;
                ftdi_82c3480_recalc_shadow(dev);
            }
            break;

        case 0xfc2f:
            /*
               Bits 1-0: shadow RAM for F0000h. Bit 2 follows the CPU cache
               being turned on in CR0, bits 6-3 hold a speed setting.
             */
            dev->regs[port & 0x0f] = val;
            ftdi_82c3480_recalc_shadow(dev);
            break;

        default:
            /*
               FC22h is the index. FC28h bits 7-6 hold the size of the
               external cache the BIOS found, FC2Ch bit 4 enables it and
               FC2Bh bits 4-3 take part in its test. With these as plain
               latches the BIOS finds no external cache, as on the board,
               whose cache sockets are empty; no cache is emulated.
             */
            dev->regs[port & 0x0f] = val;
            break;
    }
}

static uint8_t
ftdi_82c3480_read(uint16_t port, void *priv)
{
    const ftdi_82c3480_t *dev = (ftdi_82c3480_t *) priv;
    uint8_t               idx = dev->regs[FTDI_REG_INDEX];
    uint8_t               ret = 0xff;

    switch (port) {
        case 0xfc23:
            if (idx < FTDI_ROWS)
                ret = dev->rows[idx];
            break;

        case 0xfc24:
            if (idx < sizeof(dev->unk24))
                ret = dev->unk24[idx];
            break;

        case 0xfc25:
            if (idx < sizeof(dev->unk25))
                ret = dev->unk25[idx];
            break;

        case 0xfc2e:
            if (idx < sizeof(dev->shadow))
                ret = dev->shadow[idx];
            break;

        default:
            ret = dev->regs[port & 0x0f];
            break;
    }

    ftdi_82c3480_log("[%04X:%08X] FTDI 82C3480: [R] %04X[%02X] = %02X\n", CS, cpu_state.pc, port, idx, ret);

    return ret;
}

/*
   The SIMMs fitted for the configured memory size: socket 0 holds the
   largest SIMM that fits, socket 1 the largest that fits in the rest, and
   whatever is left over is not fitted. 2, 8 and 32 MB SIMMs have two equal
   sides, 1, 4 and 16 MB SIMMs one. The rows get their place in ram[] in the
   order the BIOS lays them out (the larger SIMM first, socket 1 first on
   equal sizes), so that its final layout is the identity.
 */
static void
ftdi_82c3480_fit_simms(ftdi_82c3480_t *dev)
{
    static const uint32_t simm_mb[6] = { 32, 16, 8, 4, 2, 1 };
    uint32_t              left       = mem_size >> 10;
    uint32_t              simm[2]    = { 0, 0 };
    uint8_t               order[2];
    uint32_t              off = 0;

    for (uint8_t k = 0; k < 2; k++) {
        for (uint8_t i = 0; i < 6; i++) {
            if (simm_mb[i] <= left) {
                simm[k] = simm_mb[i];
                left -= simm[k];
                break;
            }
        }
    }

    order[0] = (simm[1] >= simm[0]) ? 1 : 0;
    order[1] = order[0] ^ 1;

    for (uint8_t i = 0; i < 2; i++) {
        uint8_t  k        = order[i];
        int      two_side = (simm[k] == 2) || (simm[k] == 8) || (simm[k] == 32);
        uint32_t side_mb  = two_side ? (simm[k] >> 1) : simm[k];

        if (simm[k] == 0)
            continue;

        dev->row_size[k << 1] = side_mb;
        dev->row_ram[k << 1]  = off;
        if (two_side) {
            dev->row_size[(k << 1) + 1] = side_mb;
            dev->row_ram[(k << 1) + 1]  = off + (side_mb << 20);
        }

        off += simm[k] << 20;
    }

    dev->total = off;

    ftdi_82c3480_log("FTDI 82C3480: SIMMs %i MB and %i MB, %i KB not fitted\n",
                     (int) simm[0], (int) simm[1], (int) (mem_size - (off >> 10)));
}

/* All registers 0 (an assumption: the BIOS programs every one it uses, and
   it clears the C and E shadow nibbles before first use but not D). */
static void
ftdi_82c3480_reset(void *priv)
{
    ftdi_82c3480_t *dev = (ftdi_82c3480_t *) priv;

    memset(dev->regs, 0x00, sizeof(dev->regs));
    memset(dev->rows, 0x00, sizeof(dev->rows));
    memset(dev->unk24, 0x00, sizeof(dev->unk24));
    memset(dev->unk25, 0x00, sizeof(dev->unk25));
    memset(dev->shadow, 0x00, sizeof(dev->shadow));

    ftdi_82c3480_recalc_rows(dev);
    ftdi_82c3480_recalc_shadow(dev);
}

static void
ftdi_82c3480_close(void *priv)
{
    ftdi_82c3480_t *dev = (ftdi_82c3480_t *) priv;

    free(dev);
}

static void *
ftdi_82c3480_init(UNUSED(const device_t *info))
{
    ftdi_82c3480_t *dev = (ftdi_82c3480_t *) calloc(1, sizeof(ftdi_82c3480_t));

    ftdi_82c3480_fit_simms(dev);

    /* mem_reset() left the state internal up to the configured size. */
    dev->int_top = (mem_size > 1024) ? (mem_size << 10) : 0x00100000;

    /* Added from the last row down, so that a lower row wins where rows overlap. */
    for (int r = FTDI_ROWS - 1; r >= 0; r--) {
        for (int i = (r << 1); i < ((r << 1) + 2); i++) {
            mem_mapping_add(&dev->map[i], 0, 0,
                            ftdi_82c3480_ram_readb, ftdi_82c3480_ram_readw, ftdi_82c3480_ram_readl,
                            ftdi_82c3480_ram_writeb, ftdi_82c3480_ram_writew, ftdi_82c3480_ram_writel,
                            NULL, MEM_MAPPING_INTERNAL, &dev->seg[i]);
            mem_mapping_disable(&dev->map[i]);
        }
    }

    io_sethandler(0xfc22, 0x000e,
                  ftdi_82c3480_read, NULL, NULL, ftdi_82c3480_write, NULL, NULL, dev);

    device_add(&port_92_device);

    ftdi_82c3480_reset(dev);

    return dev;
}

const device_t ftdi_82c3480_device = {
    .name          = "FTDI 82C3480",
    .internal_name = "ftdi_82c3480",
    .flags         = 0,
    .local         = 0,
    .init          = ftdi_82c3480_init,
    .close         = ftdi_82c3480_close,
    .reset         = ftdi_82c3480_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
