/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the Intel 8251A UART.
 *
 *          Based on Intel 8251 emulation in MAME
 *          (src/devices/machine/i8251.cpp),
 *          copyright-holders: smf, Robbbert.
 *
 *          Only the asynchronous mode is modelled; the synchronous mode's
 *          sync characters are accepted and discarded. Transmit and receive
 *          timing comes from the TxC/RxC rate supplied by the machine, which
 *          on the Tandy 2000 is an 8253 channel.
 *
 *          A machine may instead wire the USART to another emulated device
 *          through a serial line (the SCP CPU Support card's terminal). The
 *          transmitter and receiver then work bit by bit on the TxC/RxC
 *          edges, and the status register reports framing, overrun, parity
 *          and break the way the line produced them.
 *
 * Authors: WNT50
 *
 *          Copyright 2026 WNT50.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/pic.h>
#include <86box/char.h>
#include <86box/serial.h>
#include <86box/serial_line.h>
#include <86box/plat_unused.h>

#ifdef ENABLE_I8251_LOG
int i8251_do_log = ENABLE_I8251_LOG;

static void
i8251_log(const char *fmt, ...)
{
    va_list ap;

    if (i8251_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define i8251_log(fmt, ...)
#endif

typedef struct i8251_t {
    uint8_t mode;
    uint8_t command;
    uint8_t tx_data;
    uint8_t rx_data;

    int expect_mode; /* the next control write is a mode word */
    int rx_ready;
    int txrdy;
    int txe;

    int irq_mask; /* IRQ line as a PIC bit mask, 0 = not connected */

    double clock; /* TxC/RxC in Hz, 0 = not programmed */

    char_port_t char_port;

    pc_timer_t rx_timer;
    pc_timer_t tx_timer;
    pc_timer_t tx_done_timer;

    /* Line mode: wired bit by bit to another emulated device. */
    int            line_mode;
    serial_line_t *tx_line;
    serial_rx_t    rx;
    double         clk_period; /* TxC/RxC period in TSC units, 0 = stopped */
    double         clk_rise;
    uint8_t        errors;     /* PE, OE and FE, in their status positions */
    int            brkdet;
    int            space_chars; /* consecutive characters of all space */
    int            cts;
    int            dsr;
    int            tx_full;    /* the buffer holds a character */
    int            tx_shift;   /* the shift register is sending */
    double         tx_end;     /* when the shift register empties */

    void (*rxrdy_cb)(void *priv, int level);
    void (*txrdy_cb)(void *priv, int level);
    void  *ready_priv;
    int    rxrdy_pin;
    int    txrdy_pin;
} i8251_t;

static void i8251_line_update(i8251_t *dev);
static void i8251_line_tx_done(i8251_t *dev);

static void
i8251_update_irq(i8251_t *dev)
{
    /* The ready flags stay visible in the status register regardless of the
       enables; only the interrupt output is gated by them, so that an idle
       transmitter cannot hold the line into the 8259 forever. */
    int active = (dev->rx_ready && (dev->command & 0x04)) ||
                 (dev->txrdy && (dev->command & 0x01));

    if (dev->irq_mask == 0)
        return;

    if (active)
        picint(dev->irq_mask);
    else
        picintc(dev->irq_mask);
}

static int
i8251_baud_factor(const i8251_t *dev)
{
    switch (dev->mode & 0x03) {
        case 0x01:
            return 1;
        case 0x03:
            return 64;
        default:
            /* 00 selects synchronous mode, which the clock is not scaled for. */
            return 16;
    }
}

static int
i8251_data_bits(const i8251_t *dev)
{
    return 5 + ((dev->mode >> 2) & 0x03);
}

static int
i8251_stop_bits(const i8251_t *dev)
{
    switch ((dev->mode >> 6) & 0x03) {
        case 0x02:
            return 2; /* 1.5 stop bits, rounded up for the host backend */
        case 0x03:
            return 2;
        default:
            return 1;
    }
}

static int
i8251_char_bits(const i8251_t *dev)
{
    return 1 + i8251_data_bits(dev) + i8251_stop_bits(dev) + ((dev->mode & 0x10) ? 1 : 0);
}

static double
i8251_bit_period(const i8251_t *dev)
{
    double hz = dev->clock;

    /* Keep the port usable while the guest has not programmed its clock yet. */
    if (hz < 1.0)
        hz = 153600.0;

    return ((double) i8251_baud_factor(dev) * 1000000.0) / hz;
}

static void
i8251_update_backend(i8251_t *dev)
{
    char_device_t *cd = &dev->char_port.chardev;

    if (!cd->port_config)
        return;

    double hz = dev->clock;
    if (hz < 1.0)
        hz = 153600.0;

    dev->char_port.com.baud      = (uint32_t) ((hz / (double) i8251_baud_factor(dev)) + 0.5);
    if (dev->mode & 0x10)
        dev->char_port.com.parity = (dev->mode & 0x20) ? CHAR_COM_PARITY_EVEN : CHAR_COM_PARITY_ODD;
    else
        dev->char_port.com.parity = 0;
    dev->char_port.com.data_bits = (uint8_t) i8251_data_bits(dev);
    dev->char_port.com.stop_bits = (uint8_t) i8251_stop_bits(dev);

    cd->port_config(cd->priv);
}

static void
i8251_update_lines(i8251_t *dev)
{
    char_device_t *cd = &dev->char_port.chardev;
    uint32_t       flags = 0;

    if (!cd->control)
        return;

    if (dev->command & 0x02)
        flags |= CHAR_COM_DTR;
    if (dev->command & 0x20)
        flags |= CHAR_COM_RTS;
    if (dev->command & 0x08)
        flags |= CHAR_COM_BREAK;

    cd->control(flags, cd->priv);
}

static void
i8251_reset_state(i8251_t *dev)
{
    dev->mode        = 0x00;
    dev->command     = 0x00;
    dev->expect_mode = 1;
    dev->rx_ready    = 0;
    dev->txrdy       = 1;
    dev->txe         = 1;

    timer_disable(&dev->tx_timer);
    timer_disable(&dev->tx_done_timer);

    i8251_update_irq(dev);
}

static void
i8251_tx_done(void *priv)
{
    i8251_t *dev = (i8251_t *) priv;

    if (dev->line_mode) {
        i8251_line_tx_done(dev);
        return;
    }

    dev->txe = 1;
    i8251_update_irq(dev);
}

static void
i8251_tx_start(void *priv)
{
    i8251_t *dev = (i8251_t *) priv;
    int      bits = i8251_char_bits(dev) - 1;

    /* The byte has moved to the shift register, so the buffer is free again. */
    dev->txrdy = 1;
    i8251_update_irq(dev);

    if (dev->char_port.chardev.write)
        dev->char_port.chardev.write(&dev->tx_data, 1, dev->char_port.chardev.priv);

    if (bits < 1)
        bits = 1;
    timer_set_delay_u64(&dev->tx_done_timer,
                        (uint64_t) ((double) bits * i8251_bit_period(dev) * TIMER_USEC));
}

static void
i8251_rx_poll(void *priv)
{
    i8251_t *dev = (i8251_t *) priv;

    /* A wired line delivers its own characters. */
    if (dev->line_mode)
        return;

    if (!dev->rx_ready && (dev->command & 0x04) && dev->char_port.chardev.read) {
        uint8_t byte;

        if (dev->char_port.chardev.read(&byte, 1, dev->char_port.chardev.priv) == 1) {
            dev->rx_data  = byte;
            dev->rx_ready = 1;
            i8251_update_irq(dev);
        }
    }

    timer_advance_u64(&dev->rx_timer, 1000 * TIMER_USEC);
}

/* ---- Line mode ------------------------------------------------------------ */

static int
i8251_line_parity(const i8251_t *dev)
{
    if (!(dev->mode & 0x10))
        return 0;

    return (dev->mode & 0x20) ? 2 : 1;
}

static double
i8251_line_stop_bits(const i8251_t *dev)
{
    switch ((dev->mode >> 6) & 0x03) {
        case 0x02:
            return 1.5;
        case 0x03:
            return 2.0;
        default:
            return 1.0;
    }
}

/* The RxRDY pin follows the status bit, both inhibited by RxE; the TxRDY pin
   also needs TxEN and CTS, while its status bit only says the buffer is empty. */
static void
i8251_line_pins(i8251_t *dev)
{
    int rxrdy = dev->rx_ready && (dev->command & 0x04);
    int txrdy = !dev->tx_full && (dev->command & 0x01) && dev->cts;

    if ((dev->rxrdy_cb != NULL) && (rxrdy != dev->rxrdy_pin)) {
        dev->rxrdy_pin = rxrdy;
        dev->rxrdy_cb(dev->ready_priv, rxrdy);
    }

    if ((dev->txrdy_cb != NULL) && (txrdy != dev->txrdy_pin)) {
        dev->txrdy_pin = txrdy;
        dev->txrdy_cb(dev->ready_priv, txrdy);
    }
}

/* Move the buffer into the shift register and start the frame on the next
   falling TxC edge. */
static void
i8251_line_tx_kick(i8251_t *dev)
{
    double   now = (double) tsc;
    double   fall;
    double   start;
    double   bit;
    uint32_t level;
    int      cells;

    if (!dev->tx_full || dev->tx_shift || (dev->tx_line == NULL))
        return;

    if (!(dev->command & 0x01) || !dev->cts || (dev->clk_period <= 0.0) || !(dev->mode & 0x03))
        return;

    fall  = dev->clk_rise + (dev->clk_period * 0.5);
    start = fall + (ceil(((now - fall) / dev->clk_period) - 1e-6) * dev->clk_period);
    bit   = (double) i8251_baud_factor(dev) * dev->clk_period;
    cells = serial_line_cells(dev->tx_data, i8251_data_bits(dev), i8251_line_parity(dev), &level);

    serial_line_send(dev->tx_line, start, bit, cells, level);

    dev->tx_full  = 0;
    dev->txrdy    = 1;
    dev->tx_shift = 1;
    dev->txe      = 0;
    dev->tx_end   = start + ((cells + i8251_line_stop_bits(dev)) * bit);

    timer_set_delay_u64(&dev->tx_done_timer,
                        (uint64_t) ((dev->tx_end - now) * (double) (1ULL << 32)));

    i8251_line_pins(dev);
}

static void
i8251_line_tx_done(i8251_t *dev)
{
    dev->tx_shift = 0;
    dev->txe      = !dev->tx_full;

    i8251_line_tx_kick(dev);
    i8251_line_pins(dev);
}

static void
i8251_line_receive(void *priv, uint8_t data, int framing, int parity, int brk)
{
    i8251_t *dev = (i8251_t *) priv;

    /* A character the CPU has not read yet is overwritten. */
    if (dev->rx_ready)
        dev->errors |= 0x10;
    if (parity)
        dev->errors |= 0x08;
    if (framing)
        dev->errors |= 0x20;

    /* Break: RxD held at space through two whole character times. */
    if (brk) {
        if (++dev->space_chars >= 2)
            dev->brkdet = 1;
    } else
        dev->space_chars = 0;

    /* Bits beyond the character length read as zero. */
    dev->rx_data  = data & ((1 << i8251_data_bits(dev)) - 1);
    dev->rx_ready = 1;

    i8251_line_pins(dev);
}

static void
i8251_line_update(i8251_t *dev)
{
    if (!dev->line_mode)
        return;

    serial_rx_set_format(&dev->rx, i8251_data_bits(dev), i8251_line_parity(dev),
                         i8251_baud_factor(dev));
    /* Synchronous mode is not modelled: its receiver hunts for sync characters. */
    serial_rx_enable(&dev->rx, (dev->command & 0x04) && !dev->expect_mode && (dev->mode & 0x03));

    i8251_line_tx_kick(dev);
    i8251_line_pins(dev);
}

static uint8_t
i8251_line_read(i8251_t *dev, uint16_t addr)
{
    uint8_t ret;

    serial_rx_update(&dev->rx);

    if (addr & 0x0002) {
        /* Break detect falls when RxD returns to mark. */
        if (dev->brkdet && serial_line_level(dev->rx.line, (double) tsc))
            dev->brkdet = 0;

        ret = (dev->tx_full ? 0x00 : 0x01) |
              ((dev->rx_ready && (dev->command & 0x04)) ? 0x02 : 0x00) |
              ((!dev->tx_full && !dev->tx_shift) ? 0x04 : 0x00) |
              dev->errors | (dev->brkdet ? 0x40 : 0x00) | (dev->dsr ? 0x80 : 0x00);
    } else {
        ret           = dev->rx_data;
        dev->rx_ready = 0;
        i8251_line_pins(dev);
    }

    return ret;
}

static void
i8251_line_write(i8251_t *dev, uint16_t addr, uint8_t val)
{
    if (!(addr & 0x0002)) {
        /* Writing while TxRDY is low replaces the waiting character. */
        dev->tx_data = val;
        dev->tx_full = 1;
        dev->txrdy   = 0;
        dev->txe     = 0;
        i8251_line_tx_kick(dev);
        i8251_line_pins(dev);
        return;
    }

    if (dev->expect_mode) {
        dev->expect_mode = 0;
        dev->mode        = val;
        i8251_line_update(dev);
        return;
    }

    if (val & 0x40) {
        /* Internal reset: back to waiting for a mode instruction. */
        if (dev->command & 0x08)
            serial_line_set_break(dev->tx_line, (double) tsc, 0);
        dev->mode        = 0x00;
        dev->command     = 0x00;
        dev->expect_mode = 1;
        dev->rx_ready    = 0;
        dev->tx_full     = 0;
        dev->txrdy       = 1;
        dev->txe         = !dev->tx_shift;
        i8251_line_update(dev);
        return;
    }

    if ((val ^ dev->command) & 0x08)
        serial_line_set_break(dev->tx_line, (double) tsc, !!(val & 0x08));

    if (val & 0x10)
        dev->errors = 0x00;

    dev->command = val;
    i8251_line_update(dev);
}

void
i8251_attach_line(void *priv, serial_line_t *tx, serial_line_t *rx)
{
    i8251_t *dev = (i8251_t *) priv;

    dev->line_mode = 1;
    dev->tx_line   = tx;
    dev->cts       = 1;
    dev->dsr       = 1;
    dev->rxrdy_pin = -1;
    dev->txrdy_pin = -1;

    serial_rx_init(&dev->rx, rx, i8251_line_receive, dev);
    i8251_line_update(dev);
}

void
i8251_set_ready_callbacks(void *priv, void (*rxrdy)(void *priv, int level),
                          void (*txrdy)(void *priv, int level), void *cb_priv)
{
    i8251_t *dev = (i8251_t *) priv;

    dev->rxrdy_cb   = rxrdy;
    dev->txrdy_cb   = txrdy;
    dev->ready_priv = cb_priv;
    dev->rxrdy_pin  = -1;
    dev->txrdy_pin  = -1;
    i8251_line_pins(dev);
}

/* TxC and RxC as a clock of the given period with a rising edge at 'rise',
   both in TSC units; a period of 0 stops them. */
void
i8251_set_clock_edges(void *priv, double period, double rise)
{
    i8251_t *dev = (i8251_t *) priv;

    dev->clk_period = period;
    dev->clk_rise   = rise;

    serial_rx_set_clock(&dev->rx, period, rise);
    i8251_line_tx_kick(dev);
}

void
i8251_set_modem_inputs(void *priv, int cts, int dsr)
{
    i8251_t *dev = (i8251_t *) priv;

    dev->cts = cts;
    dev->dsr = dsr;
    i8251_line_tx_kick(dev);
    i8251_line_pins(dev);
}

uint8_t
i8251_read(uint16_t addr, void *priv)
{
    i8251_t *dev = (i8251_t *) priv;
    uint8_t  ret;

    if (addr & 0x0001)
        return 0xff;

    if (dev->line_mode) {
        ret = i8251_line_read(dev, addr);
        i8251_log("i8251_read(%04X) = %02X\n", addr, ret);
        return ret;
    }

    if (addr & 0x0002) {
        ret = (dev->rx_ready ? 0x02 : 0x00) |
              (dev->txrdy ? 0x01 : 0x00) |
              (dev->txe ? 0x04 : 0x00);

        /* With no backend attached the modem inputs float high. */
        if ((dev->char_port.chardev.status(dev->char_port.chardev.priv) & CHAR_COM_DSR) ||
            (!dev->char_port.chardev.status))
            ret |= 0x80;
    } else {
        ret           = dev->rx_data;
        dev->rx_ready = 0;
        i8251_update_irq(dev);
    }

    i8251_log("i8251_read(%04X) = %02X\n", addr, ret);
    return ret;
}

void
i8251_write(uint16_t addr, uint8_t val, void *priv)
{
    i8251_t *dev = (i8251_t *) priv;

    if (addr & 0x0001)
        return;

    i8251_log("i8251_write(%04X, %02X)\n", addr, val);

    if (dev->line_mode) {
        i8251_line_write(dev, addr, val);
        return;
    }

    if (!(addr & 0x0002)) {
        if (!dev->txrdy)
            return;
        dev->tx_data = val;
        dev->txrdy   = 0;
        dev->txe     = 0;
        i8251_update_irq(dev);
        timer_set_delay_u64(&dev->tx_timer,
                            (uint64_t) (i8251_bit_period(dev) * TIMER_USEC));
        return;
    }

    if (dev->expect_mode) {
        dev->expect_mode = 0;
        dev->mode        = val;
        i8251_update_backend(dev);
        return;
    }

    if (val & 0x40) {
        /* Internal reset: the next control write is taken as a mode word. */
        i8251_reset_state(dev);
        i8251_update_backend(dev);
        return;
    }

    dev->command = val;
    i8251_update_lines(dev);
    i8251_update_irq(dev);
}

void
i8251_set_irq(void *priv, int irq_mask)
{
    i8251_t *dev = (i8251_t *) priv;

    if (dev == NULL)
        return;

    if (dev->irq_mask != 0)
        picintc(dev->irq_mask);

    dev->irq_mask = irq_mask;
}

void
i8251_set_clock(void *priv, double hz)
{
    i8251_t *dev = (i8251_t *) priv;

    if (dev == NULL)
        return;

    dev->clock = hz;
    i8251_update_backend(dev);
}

static void *
i8251_init(const device_t *info)
{
    i8251_t *dev  = (i8251_t *) calloc(1, sizeof(i8251_t));
    int      port = (int) (info->local & 0xff);

    dev->expect_mode = 1;
    dev->irq_mask    = 0;
    dev->txrdy       = 1;
    dev->txe         = 1;

    timer_add(&dev->rx_timer, i8251_rx_poll, dev, 0);
    timer_add(&dev->tx_timer, i8251_tx_start, dev, 0);
    timer_add(&dev->tx_done_timer, i8251_tx_done, dev, 0);
    timer_set_delay_u64(&dev->rx_timer, 1000 * TIMER_USEC);

    if ((port < SERIAL_MAX) && com_ports[port].enabled && com_ports[port].device) {
        dev->char_port.type = CHAR_PORT_COM;
        snprintf(dev->char_port.name, sizeof(dev->char_port.name), "COM%d", port + 1);
        char_init(&dev->char_port, char_get_device(com_ports[port].device), port + 1);
    }

    return dev;
}

static void
i8251_close(void *priv)
{
    i8251_t *dev = (i8251_t *) priv;

    timer_disable(&dev->rx_timer);
    timer_disable(&dev->tx_timer);
    timer_disable(&dev->tx_done_timer);

    if (dev->irq_mask != 0)
        picintc(dev->irq_mask);

    if (dev->line_mode)
        serial_rx_close(&dev->rx);

    free(dev);
}

static void
i8251_reset(void *priv)
{
    i8251_t *dev = (i8251_t *) priv;

    i8251_reset_state(dev);
    i8251_update_lines(dev);

    if (dev->line_mode) {
        if (dev->tx_line != NULL)
            serial_line_set_break(dev->tx_line, (double) tsc, 0);
        dev->errors      = 0x00;
        dev->brkdet      = 0;
        dev->space_chars = 0;
        dev->tx_full     = 0;
        dev->tx_shift    = 0;
        i8251_line_update(dev);
    }
}

const device_t i8251_device = {
    .name          = "Intel 8251A UART",
    .internal_name = "i8251",
    .flags         = 0,
    .local         = 0,
    .init          = i8251_init,
    .close         = i8251_close,
    .reset         = i8251_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
