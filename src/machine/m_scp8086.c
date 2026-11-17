/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the Seattle Computer Products 8086 system, an
 *          S-100 machine that is not IBM PC compatible: an SCP-200B 8086
 *          CPU card, an SCP-300F CPU Support card, SCP-110 static memory
 *          and a Tarbell double density or Cromemco 4FDC floppy controller,
 *          with a serial terminal as the console.
 *
 *          The CPU Support card holds the 8086 Monitor EPROM, a master and
 *          a slave 8259A, an Am9513 timer whose counter 5 clocks the 8251A
 *          serial port, a parallel input and output port and an eight-way
 *          sense switch, all in one block of sixteen ports (Instruction
 *          Manual, Model SCP300F Rev. F; System Support Card Errata).
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
#include <86box/machine.h>
#include <86box/rom.h>
#include <86box/pic.h>
#include <86box/serial_line.h>
#include <86box/serial.h>
#include <86box/lpt.h>
#include <86box/am9513.h>
#include <86box/fdc_tarbell.h>
#include <86box/fdc_cromemco.h>
#include <86box/video.h>
#include <86box/vid_adm3a.h>
#include <86box/plat_unused.h>

#define SCP_BASE      0xf0 /* switch S1 positions 1-4 all closed */
#define SCP_ROM_ADDR  0xff800
#define SCP_ROM_SIZE  0x800

/* Slave 8259A inputs (p. 7, with the Rev. F parallel swap in the errata). */
#define SCP_IRQ_OUT2  (1 << 8)
#define SCP_IRQ_RXRDY (1 << 9)
#define SCP_IRQ_PIN   (1 << 10)
#define SCP_IRQ_OUT3  (1 << 12)
#define SCP_IRQ_TXRDY (1 << 13)
#define SCP_IRQ_POUT  (1 << 14)
#define SCP_IRQ_OUT4  (1 << 15)

enum {
    SCP_CONSOLE_ADM3A = 0,
    SCP_CONSOLE_COM1
};

enum {
    SCP_FDC_TARBELL = 0,
    SCP_FDC_4FDC
};

enum {
    SCP_WAIT_NONE = 0,
    SCP_WAIT_8MHZ,
    SCP_WAIT_ALWAYS
};

typedef struct scp_t {
    uint8_t  rom[SCP_ROM_SIZE];
    int      rom_enabled;
    uint8_t  sense;
    int      wait_mode;
    int      waits;
    int      fdc;

    mem_mapping_t ram_hi_map;
    mem_mapping_t top_map;

    am9513_t *stc;
    void     *usart;
    void     *term;
    int       console;

    serial_line_t to_term;
    serial_line_t from_term;

    /* Parallel ports. */
    uint8_t pout;
    int     dav;  /* output data available, DAV active */
    uint8_t pin;
    int     inrdy;

    uint8_t irq_state[16];
} scp_t;

static scp_t *scp;

/* ---- Interrupts ------------------------------------------------------------ */

static void
scp_irq(scp_t *dev, int irq, int level)
{
    int bit = 0;

    while (!((irq >> bit) & 1))
        bit++;

    if (level)
        picintlevel(irq, &dev->irq_state[bit]);
    else
        picintclevel(irq, &dev->irq_state[bit]);
}

static void
scp_out2(void *priv, int level)
{
    scp_irq((scp_t *) priv, SCP_IRQ_OUT2, level);
}

static void
scp_out3(void *priv, int level)
{
    scp_irq((scp_t *) priv, SCP_IRQ_OUT3, level);
}

static void
scp_out4(void *priv, int level)
{
    scp_irq((scp_t *) priv, SCP_IRQ_OUT4, level);
}

static void
scp_rxrdy(void *priv, int level)
{
    scp_irq((scp_t *) priv, SCP_IRQ_RXRDY, level);
}

static void
scp_txrdy(void *priv, int level)
{
    scp_irq((scp_t *) priv, SCP_IRQ_TXRDY, level);
}

static void
scp_parallel_irqs(scp_t *dev)
{
    scp_irq(dev, SCP_IRQ_PIN, dev->inrdy);
    scp_irq(dev, SCP_IRQ_POUT, !dev->dav);
}

/* ---- Serial clock -------------------------------------------------------------- */

/* Counter 5 provides TxC and RxC for the 8251A. */
static void
scp_usart_clock(void *priv)
{
    scp_t *dev = (scp_t *) priv;
    double period;
    double rise;

    if (!am9513_get_out_clock(dev->stc, 5, &period, &rise))
        period = rise = 0.0;

    if (dev->console == SCP_CONSOLE_ADM3A)
        i8251_set_clock_edges(dev->usart, period, rise);
    else
        i8251_set_clock(dev->usart, (period > 0.0) ? (cpuclock / period) : 0.0);
}

/* ---- Wait states ------------------------------------------------------------- */

/* S1-7 and S1-8: one wait state on every access to the card, always or only
   when the 8086 card reports 8 MHz on pin 98 (p. 4, p. 45). */
static void
scp_wait(scp_t *dev)
{
    if (dev->waits)
        sub_cycles(1);
}

/* ---- I/O ---------------------------------------------------------------------- */

static uint8_t
scp_read(uint16_t port, void *priv)
{
    scp_t  *dev = (scp_t *) priv;
    uint8_t ret = 0xff;

    scp_wait(dev);

    switch (port & 0x0f) {
        case 0x0:
        case 0x1:
            ret = pic_read(port, &pic);
            break;
        case 0x2:
        case 0x3:
            ret = pic_read(port, &pic2);
            break;
        case 0x4:
        case 0x5:
            ret = am9513_read(port, dev->stc);
            break;
        case 0x6:
        case 0x7:
            /* The USART's C/D input is A0. */
            ret = i8251_read((port & 1) << 1, dev->usart);
            break;
        case 0xc:
            /* Reading the input latch makes RDY ready for the next byte. */
            ret        = dev->pin;
            dev->inrdy = 0;
            scp_parallel_irqs(dev);
            break;
        case 0xd:
            /* Nothing drives the open inputs: INSTB and ACK are terminated
               high, so the input latch is open and ACK never falls. */
            ret = (dev->dav ? 0x00 : 0x01) | (dev->inrdy ? 0x02 : 0x00) | 0x04 | 0x08;
            break;
        case 0xe:
            /* Any access turns the EPROM off until reset. */
            dev->rom_enabled = 0;
            break;
        case 0xf:
            ret = dev->sense;
            break;
        default:
            break;
    }

    return ret;
}

static void
scp_write(uint16_t port, uint8_t val, void *priv)
{
    scp_t *dev = (scp_t *) priv;

    scp_wait(dev);

    switch (port & 0x0f) {
        case 0x0:
        case 0x1:
            pic_write(port, val, &pic);
            break;
        case 0x2:
        case 0x3:
            pic_write(port, val, &pic2);
            break;
        case 0x4:
        case 0x5:
            am9513_write(port, val, dev->stc);
            break;
        case 0x6:
        case 0x7:
            i8251_write((port & 1) << 1, val, dev->usart);
            break;
        case 0xc:
            /* DAV goes active; only a falling ACK takes it away again. */
            dev->pout = val;
            dev->dav  = 1;
            scp_parallel_irqs(dev);
            break;
        case 0xe:
            /* Any access turns the EPROM off until reset. */
            dev->rom_enabled = 0;
            break;
        default:
            break;
    }
}

/* ---- Memory -------------------------------------------------------------------- */

/* The top 4K page: RAM up to FF7FFh, and above it the EPROM while enabled.
   With PHANTOM jumpered the card disables a RAM board there. */
static uint8_t
scp_top_read(uint32_t addr, void *priv)
{
    scp_t *dev = (scp_t *) priv;

    addr &= 0xfffff;

    if (dev->rom_enabled && (addr >= SCP_ROM_ADDR)) {
        scp_wait(dev);
        return dev->rom[addr & (SCP_ROM_SIZE - 1)];
    }

    if (addr < (mem_size << 10))
        return ram[addr];

    return 0xff;
}

static uint16_t
scp_top_readw(uint32_t addr, void *priv)
{
    return scp_top_read(addr, priv) | (scp_top_read(addr + 1, priv) << 8);
}

static void
scp_top_write(uint32_t addr, uint8_t val, void *priv)
{
    scp_t *dev = (scp_t *) priv;

    addr &= 0xfffff;

    if (dev->rom_enabled && (addr >= SCP_ROM_ADDR))
        return;

    if (addr < (mem_size << 10))
        ram[addr] = val;
}

static void
scp_top_writew(uint32_t addr, uint16_t val, void *priv)
{
    scp_top_write(addr, val & 0xff, priv);
    scp_top_write(addr + 1, val >> 8, priv);
}

/* ---- Device --------------------------------------------------------------------- */

static void
scp_speed(scp_t *dev)
{
    dev->waits = (dev->wait_mode == SCP_WAIT_ALWAYS) ||
                 ((dev->wait_mode == SCP_WAIT_8MHZ) && (cpu_s->rspeed >= 8000000));
}

static void
scp_speed_changed(void *priv)
{
    scp_t *dev = (scp_t *) priv;

    scp_speed(dev);
    scp_usart_clock(dev);
}

static void
scp_reset(void *priv)
{
    scp_t *dev = (scp_t *) priv;

    /* RESET turns the EPROM back on. */
    dev->rom_enabled = 1;
    dev->dav         = 0;
    dev->inrdy       = 0;
    scp_parallel_irqs(dev);
}

static void
scp_close(void *priv)
{
    scp_t *dev = (scp_t *) priv;

    cpu_set_io_ready(NULL, NULL);
    free(dev);
    scp = NULL;
}

static const device_config_t scp8086_config[] = {
    // clang-format off
    {
        .name           = "bios",
        .description    = "8086 Monitor",
        .type           = CONFIG_BIOS,
        .default_string = "mon86_15t",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = {
            {
                .name          = "Monitor 1.5T (Tarbell, 1980)",
                .internal_name = "mon86_15t",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = SCP_ROM_SIZE,
                .files         = { "roms/machines/scp8086/mon86_v1.5a_tdd.bin", "" }
            },
            {
                .name          = "Monitor 1.5 (Tarbell DD, 1982)",
                .internal_name = "mon86_15tdd",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = SCP_ROM_SIZE,
                .files         = { "roms/machines/scp8086/mon86_v1.5_tdd.bin", "" }
            },
            {
                .name          = "Monitor 1.6 (Tarbell DD)",
                .internal_name = "mon86_16tdd",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = SCP_ROM_SIZE,
                .files         = { "roms/machines/scp8086/mon86_v16_tdd.bin", "" }
            },
            {
                .name          = "Monitor 1.5 (Cromemco 4FDC)",
                .internal_name = "mon86_15c",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = SCP_ROM_SIZE,
                .files         = { "roms/machines/scp8086/mon86_v1.5_4fdc.bin", "" }
            },
            {
                .name          = "Monitor 1.5 (\"Seattle 8MHz Old\")",
                .internal_name = "mon86_15old",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = SCP_ROM_SIZE,
                .files         = { "roms/machines/scp8086/mon86_sn1902.bin", "" }
            },
            { .files_no = 0 }
        }
    },
    {
        /* The Monitor's B command and the 86-DOS disk drivers are built
           for one controller; the Monitor must match the card. */
        .name           = "fdc",
        .description    = "Disk controller",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = SCP_FDC_TARBELL,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Tarbell Double Density", .value = SCP_FDC_TARBELL },
            { .description = "Cromemco 4FDC",          .value = SCP_FDC_4FDC    },
            { .description = ""                                                 }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "console",
        .description    = "Console terminal",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = SCP_CONSOLE_ADM3A,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Lear Siegler ADM-3A",   .value = SCP_CONSOLE_ADM3A },
            { .description = "COM1 host serial port", .value = SCP_CONSOLE_COM1  },
            { .description = ""                                                  }
        },
        .bios           = { { 0 } }
    },
    {
        /* Switch S2, read at BASE+15: closed reads as one. The 8086
           Monitor boots the disk at once when position 1 is closed. */
        .name           = "sense",
        .description    = "Sense switches (S2)",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 0x00,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "All open (enter the Monitor)", .value = 0x00 },
            { .description = "1 closed (auto boot)",         .value = 0x01 },
            { .description = ""                                            }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "wait",
        .description    = "Wait state (S1-7, S1-8)",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = SCP_WAIT_8MHZ,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "None",          .value = SCP_WAIT_NONE   },
            { .description = "At 8 MHz only", .value = SCP_WAIT_8MHZ   },
            { .description = "Always",        .value = SCP_WAIT_ALWAYS },
            { .description = ""                                        }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t scp8086_device = {
    .name          = "Seattle Computer Products 8086",
    .internal_name = "scp8086",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = scp_close,
    .reset         = scp_reset,
    .available     = NULL,
    .speed_changed = scp_speed_changed,
    .force_redraw  = NULL,
    .config        = scp8086_config
};

static int
scp_load_rom(scp_t *dev, const char *fn)
{
    FILE *fp = rom_fopen(fn, "rb");
    int   ret;

    if (fp == NULL)
        return 0;

    ret = (fread(dev->rom, 1, SCP_ROM_SIZE, fp) == SCP_ROM_SIZE);
    fclose(fp);

    return ret;
}

int
machine_scp8086_init(const machine_t *model)
{
    scp_t      *dev;
    const char *fn;
    uint32_t    top = mem_size << 10;

    device_context(model->device);
    fn = device_get_bios_file(model->device, device_get_config_bios("bios"), 0);
    device_context_restore();

    if (bios_only)
        return (fn != NULL) && rom_present(fn);

    dev = (scp_t *) calloc(1, sizeof(scp_t));
    if ((fn == NULL) || !scp_load_rom(dev, fn)) {
        free(dev);
        return 0;
    }
    scp = dev;

    device_context(model->device);
    dev->console   = device_get_config_int("console");
    dev->sense     = (uint8_t) device_get_config_int("sense");
    dev->wait_mode = device_get_config_int("wait");
    dev->fdc       = device_get_config_int("fdc");
    device_context_restore();

    device_add_ex(model->device, dev);

    /* SCP-110 boards fill memory from zero, decoding all 20 address lines;
       above 640K the RAM is mapped here rather than left to the PC layout. */
    if (top > 0xa0000) {
        uint32_t hi = (top > 0xff000) ? 0xff000 : top;

        mem_set_mem_state_both(0xa0000, hi - 0xa0000, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
        mem_mapping_add(&dev->ram_hi_map, 0xa0000, hi - 0xa0000,
                        mem_read_ram, mem_read_ramw, mem_read_raml,
                        mem_write_ram, mem_write_ramw, mem_write_raml,
                        NULL, MEM_MAPPING_INTERNAL, NULL);
    }

    dev->rom_enabled = 1;
    mem_mapping_add(&dev->top_map, 0xff000, 0x1000,
                    scp_top_read, scp_top_readw, NULL,
                    scp_top_write, scp_top_writew, NULL,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_set_mem_state_both(0xff000, 0x1000, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);

    /* No PC-style ports on the S-100 bus. */
    serial_set_next_inst(SERIAL_MAX - 1);
    lpt_set_next_inst(PARALLEL_MAX - 1);

    pic_init_scp300f();

    dev->stc = device_add(&am9513_device);
    am9513_set_out_callback(dev->stc, 2, scp_out2, dev);
    am9513_set_out_callback(dev->stc, 3, scp_out3, dev);
    am9513_set_out_callback(dev->stc, 4, scp_out4, dev);
    am9513_set_clock_callback(dev->stc, 5, scp_usart_clock, dev);

    /* With the console on COM1, the USART talks to the host port's device. */
    dev->usart = device_add(&i8251_device);
    if (dev->console == SCP_CONSOLE_ADM3A) {
        serial_line_init(&dev->to_term);
        serial_line_init(&dev->from_term);
        i8251_attach_line(dev->usart, &dev->to_term, &dev->from_term);
        i8251_set_ready_callbacks(dev->usart, scp_rxrdy, scp_txrdy, dev);

        /* Without its character ROM the terminal cannot be added. */
        if (device_available(&adm3a_device)) {
            dev->term = device_add(&adm3a_device);
            if (dev->term != NULL)
                adm3a_attach(dev->term, &dev->from_term, &dev->to_term);
        }
    } else
        i8251_set_irq(dev->usart, SCP_IRQ_RXRDY);
    scp_usart_clock(dev);

    /* With its I-O jumper in the normal position the SCP-200B copies A0-A7
       onto A8-A15, and the cards decode the low byte only. */
    for (int hi = 0; hi < 0x100; hi++) {
        io_sethandler((hi << 8) | SCP_BASE, 0x0008, scp_read, NULL, NULL, scp_write, NULL, NULL, dev);
        io_sethandler((hi << 8) | (SCP_BASE + 0x0c), 0x0004, scp_read, NULL, NULL, scp_write, NULL, NULL, dev);
    }

    device_add((dev->fdc == SCP_FDC_4FDC) ? &cromemco_4fdc_device : &tarbell_dd_device);

    scp_speed(dev);
    scp_reset(dev);

    return 1;
}
