/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the Tarbell Double Density Floppy Disk Interface,
 *          an S-100 card with an FD1791/1793 and an 8257 DMA controller,
 *          after its Operation Manual and schematics (Tarbell Electronics).
 *
 *          The disk ports sit at F8h-FDh, or at 78h-7Dh with the DMA
 *          controller moved from E0h to 60h. Reading port FC (7C) holds
 *          the processor in wait states until the 1793 raises DRQ or
 *          INTRQ, which is how programmed transfers keep up with the disk.
 *          Writing it selects the drive, the side and the density.
 *
 *          Not emulated: the bootstrap PROM (SCP systems run with it
 *          jumpered off), the short wait the board adds to 1793 register
 *          reads, the interrupt jumpers, and the bus cycles DMA takes from
 *          the processor.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/timer.h>
#include "cpu.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/device.h>
#include <86box/fdc_wd179x.h>
#include <86box/fdc_tarbell.h>
#include <86box/plat_unused.h>

/* HLD fires a 74LS123 (U10, 33K and 4.7 uF) whose output is HLT: with
   K = 0.37 the head is taken as loaded 57 ms after HLD rises. */
#define TARBELL_HLT_US (0.37 * 33000.0 * 4.7e-6 * 1000000.0)

typedef struct tarbell_t {
    wd179x_t *wd;
    uint16_t  base;     /* 1793 registers, FC and FD */
    uint16_t  dma_base; /* 8257 */
    uint8_t   select;
    uint8_t   ext;      /* A16-A23 during DMA */
    int       eop;      /* port FD bit 7: 1 while a DMA transfer is going */

    /* 8257 channel registers and mode set; channel 0 serves the 1793. */
    uint16_t  addr[4];
    uint16_t  count[4];
    uint8_t   mode;
    uint8_t   dstatus;
    int       flip;
} tarbell_t;

/* ---- 8257 ---------------------------------------------------------------- */

/* One transfer for the 1793's DRQ: TC comes as the count runs out from
   zero, so a channel moves its count plus one bytes. */
static void
tarbell_dma(tarbell_t *dev)
{
    uint32_t addr;
    int      dir;

    if (!(dev->mode & 0x01) || !wd179x_get_drq(dev->wd))
        return;

    addr = ((uint32_t) dev->ext << 16) | dev->addr[0];
    dir  = dev->count[0] >> 14;

    if (dir == 1)
        mem_writeb_phys(addr & 0xfffff, wd179x_read(dev->wd, 3)); /* DMA write: to memory */
    else if (dir == 2)
        wd179x_write(dev->wd, 3, mem_readb_phys(addr & 0xfffff)); /* DMA read: from memory */
    else
        (void) wd179x_read(dev->wd, 3); /* verify */

    dev->addr[0]++;
    if ((dev->count[0] & 0x3fff) == 0) {
        dev->dstatus |= 0x01;
        dev->eop = 0;
        /* TC stop; autoload only ever reloads channel 2. */
        if (dev->mode & 0x40)
            dev->mode &= ~0x01;
    }
    dev->count[0] = (dev->count[0] & 0xc000) | ((dev->count[0] - 1) & 0x3fff);
}

static void
tarbell_drq(void *priv, int level)
{
    tarbell_t *dev = (tarbell_t *) priv;

    if (level)
        tarbell_dma(dev);
}

static uint8_t
tarbell_dma_read(uint16_t port, void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;
    int        reg = port & 0x0f;
    uint8_t    ret;

    if (reg >= 8) {
        /* Status: TC flags, cleared by reading; bit 4 is the update flag. */
        ret          = dev->dstatus;
        dev->dstatus &= 0xf0;
        return ret;
    }

    {
        uint16_t val = (reg & 1) ? dev->count[reg >> 1] : dev->addr[reg >> 1];

        ret = dev->flip ? (val >> 8) : (val & 0xff);
        dev->flip ^= 1;
    }

    return ret;
}

static void
tarbell_dma_write(uint16_t port, uint8_t val, void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;
    int        reg = port & 0x0f;
    uint16_t  *r;

    if (reg >= 8) {
        /* Mode set: also clears the first/last flip-flop. */
        dev->mode = val;
        dev->flip = 0;
        if (val & 0x01)
            dev->eop = 1;
        tarbell_dma(dev);
        return;
    }

    r = (reg & 1) ? &dev->count[reg >> 1] : &dev->addr[reg >> 1];
    if (dev->flip)
        *r = (*r & 0x00ff) | (val << 8);
    else
        *r = (*r & 0xff00) | val;
    dev->flip ^= 1;
}

/* ---- Disk ports ----------------------------------------------------------- */

static uint8_t
tarbell_read(uint16_t port, void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;
    int        reg = (port & 0xff) - (dev->base & 0xff);

    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
            return wd179x_read(dev->wd, reg);
        case 4:
            /* Bit 7 is low once the 1793 interrupts; the wait states that
               stall the read until DRQ or INTRQ come from tarbell_ready(). */
            return wd179x_get_intrq(dev->wd) ? 0x7f : 0xff;
        case 5:
            /* Bit 7 low: the DMA operation is complete. */
            return dev->eop ? 0xff : 0x7f;
        default:
            return 0xff;
    }
}

static void
tarbell_select(tarbell_t *dev, uint8_t val)
{
    /* Bit 3 double density, bits 4-5 the drive, bit 6 the side (p. 49). */
    dev->select = val;
    wd179x_set_dden(dev->wd, !(val & 0x08));
    wd179x_set_drive(dev->wd, (val >> 4) & 3, (val >> 6) & 1);
}

static void
tarbell_write(uint16_t port, uint8_t val, void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;
    int        reg = (port & 0xff) - (dev->base & 0xff);

    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
            wd179x_write(dev->wd, reg, val);
            if (reg == 3)
                tarbell_dma(dev);
            break;
        case 4:
            tarbell_select(dev, val);
            break;
        case 5:
            dev->ext = val;
            break;
        default:
            break;
    }
}

/* Port FC holds READY low until the 1793 asks for service. */
static int
tarbell_ready(uint16_t port, int out, void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;

    if (out || ((port & 0xff) != ((dev->base + 4) & 0xff)))
        return 1;

    return wd179x_get_drq(dev->wd) || wd179x_get_intrq(dev->wd);
}

/* The card decodes A0-A7 only. */
static void
tarbell_handlers(tarbell_t *dev, int set)
{
    for (int hi = 0; hi < 0x100; hi++) {
        io_handler(set, (hi << 8) | dev->base, 6,
                   tarbell_read, NULL, NULL, tarbell_write, NULL, NULL, dev);
        io_handler(set, (hi << 8) | dev->dma_base, 16,
                   tarbell_dma_read, NULL, NULL, tarbell_dma_write, NULL, NULL, dev);
    }
}

static void
tarbell_reset(void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;

    dev->mode    = 0x00;
    dev->dstatus = 0x00;
    dev->flip    = 0;
    dev->eop     = 0;
    tarbell_select(dev, 0x00);
    wd179x_reset(dev->wd);
}

static void *
tarbell_init(UNUSED(const device_t *info))
{
    tarbell_t *dev = (tarbell_t *) calloc(1, sizeof(tarbell_t));

    /* Jumpers E4-E6: F8h/E0h, or 78h/60h. */
    if (device_get_config_int("base") == 0x78) {
        dev->base     = 0x78;
        dev->dma_base = 0x60;
    } else {
        dev->base     = 0xf8;
        dev->dma_base = 0xe0;
    }

    /* The 1793 runs from 2 MHz for 8" drives. */
    dev->wd = wd179x_create(WD_FD179X, 2000000.0);
    wd179x_set_hlt_delay(dev->wd, TARBELL_HLT_US);
    wd179x_set_callbacks(dev->wd, NULL, tarbell_drq, dev);

    tarbell_handlers(dev, 1);
    cpu_set_io_ready(tarbell_ready, dev);

    tarbell_reset(dev);

    return dev;
}

static void
tarbell_close(void *priv)
{
    tarbell_t *dev = (tarbell_t *) priv;

    cpu_set_io_ready(NULL, NULL);
    tarbell_handlers(dev, 0);
    wd179x_close(dev->wd);
    free(dev);
}

static const device_config_t tarbell_config[] = {
    // clang-format off
    {
        .name           = "base",
        .description    = "Address",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0x78,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "F8h (DMA at E0h)", .value = 0xf8 },
            { .description = "78h (DMA at 60h)", .value = 0x78 },
            { .description = ""                                }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t tarbell_dd_device = {
    .name          = "Tarbell Double Density Floppy Disk Interface",
    .internal_name = "tarbell_dd",
    .flags         = 0,
    .local         = 0,
    .init          = tarbell_init,
    .close         = tarbell_close,
    .reset         = tarbell_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = tarbell_config
};
