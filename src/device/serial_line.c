/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          An asynchronous serial line between two emulated devices.
 *
 *          The transmitter puts whole frames on the line with its own bit
 *          time; the receiver samples the line on the edges of its own
 *          clock, finding the start bit, checking it in the middle and
 *          taking each later bit one bit time apart. When both ends agree
 *          on the rate this hands over the character; when they do not,
 *          the receiver assembles whatever a real one would, framing
 *          errors included.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/serial_line.h>

void
serial_line_init(serial_line_t *line)
{
    memset(line, 0, sizeof(serial_line_t));
    line->break_from = -1.0;
}

int
serial_line_level(serial_line_t *line, double t)
{
    if ((line->break_from >= 0.0) && (t >= line->break_from))
        return 0;

    for (int i = 0; i < line->count; i++) {
        int f = (line->head - 1 - i + SERIAL_LINE_FRAMES) % SERIAL_LINE_FRAMES;

        if (t >= line->frame[f].start) {
            int cell = (int) floor((t - line->frame[f].start) / line->frame[f].bit);

            if (cell < line->frame[f].cells)
                return (line->frame[f].level >> cell) & 1;

            return 1;
        }
    }

    return 1;
}

/* The first time at or after t where the line is at space, or < 0 if the
   line stays at mark as far as is known. */
static double
serial_line_next_space(serial_line_t *line, double t)
{
    double best = -1.0;

    if (line->break_from >= 0.0)
        best = (line->break_from > t) ? line->break_from : t;

    for (int i = line->count - 1; i >= 0; i--) {
        int    f     = (line->head - 1 - i + SERIAL_LINE_FRAMES) % SERIAL_LINE_FRAMES;
        double start = line->frame[f].start;
        double bit   = line->frame[f].bit;
        int    cell;

        if ((start + (line->frame[f].cells * bit)) <= t)
            continue;

        cell = (t > start) ? (int) floor((t - start) / bit) : 0;
        for (; cell < line->frame[f].cells; cell++) {
            if (!((line->frame[f].level >> cell) & 1)) {
                double when = start + (cell * bit);

                if (when < t)
                    when = t;
                if ((best < 0.0) || (when < best))
                    best = when;
                break;
            }
        }

        if (best >= 0.0)
            break;
    }

    return best;
}

static void
serial_line_changed(serial_line_t *line)
{
    if (line->changed != NULL)
        line->changed(line->changed_priv);
}

void
serial_line_send(serial_line_t *line, double start, double bit, int cells, uint32_t level)
{
    int f = line->head;

    line->frame[f].start = start;
    line->frame[f].bit   = bit;
    line->frame[f].cells = cells;
    line->frame[f].level = level;

    line->head = (line->head + 1) % SERIAL_LINE_FRAMES;
    if (line->count < SERIAL_LINE_FRAMES)
        line->count++;

    serial_line_changed(line);
}

void
serial_line_set_break(serial_line_t *line, double t, int on)
{
    if (on && (line->break_from < 0.0))
        line->break_from = t;
    else if (!on && (line->break_from >= 0.0)) {
        /* Keep the space so far as a frame of its own. */
        double len = t - line->break_from;

        if (len > 0.0)
            serial_line_send(line, line->break_from, len, 1, 0);
        line->break_from = -1.0;
    }

    serial_line_changed(line);
}

int
serial_line_cells(uint8_t data, int data_bits, int parity, uint32_t *level)
{
    int ones  = 0;
    int cells = 1;

    *level = 0; /* start bit */
    for (int i = 0; i < data_bits; i++) {
        if ((data >> i) & 1) {
            *level |= (1 << cells);
            ones++;
        }
        cells++;
    }

    if (parity) {
        /* Odd parity makes the total of ones odd, even makes it even. */
        if ((parity == 1) ? !(ones & 1) : (ones & 1))
            *level |= (1 << cells);
        cells++;
    }

    return cells;
}

/* ---- Receiver ------------------------------------------------------------- */

static double
serial_rx_edge(serial_rx_t *rx, double t)
{
    double k = ceil(((t - rx->clk_rise) / rx->clk_period) - 1e-6);

    return rx->clk_rise + (k * rx->clk_period);
}

static void
serial_rx_timer(void *priv)
{
    serial_rx_update((serial_rx_t *) priv);
}

/* Timers fire on whole TSC ticks: round up, so that the time has really come
   when the callback looks at it. */
static void
serial_rx_schedule(serial_rx_t *rx, double when)
{
    double target = ceil(when);
    double delay  = (target > (double) tsc) ? (target - (double) tsc) : 0.0;

    timer_set_delay_u64(&rx->timer, ((uint64_t) delay) << 32);
}

void
serial_rx_update(serial_rx_t *rx)
{
    double now = (double) tsc;

    timer_disable(&rx->timer);

    if (!rx->enabled || (rx->clk_period <= 0.0) || (rx->line == NULL))
        return;

    for (;;) {
        double space = serial_line_next_space(rx->line, rx->hunt);
        double bit   = rx->factor * rx->clk_period;
        double t_start;
        double t_mid;
        double t_stop;
        int    bits;
        int    data   = 0;
        int    zeros  = 1;
        int    par_ok = 1;
        int    ones   = 0;

        if (space < 0.0)
            return; /* idle: the line tells us when something arrives */

        /* The first clock edge to see the space. */
        t_start = serial_rx_edge(rx, space);
        if (t_start > now) {
            serial_rx_schedule(rx, t_start);
            return;
        }
        if (serial_line_level(rx->line, t_start)) {
            /* Too short for the clock to see. */
            rx->hunt = t_start + (rx->clk_period * 0.5);
            continue;
        }

        /* Half a bit later the start bit must still be there. */
        t_mid = t_start + ((rx->factor >> 1) * rx->clk_period);
        if (t_mid > now) {
            serial_rx_schedule(rx, t_mid);
            return;
        }
        if (serial_line_level(rx->line, t_mid)) {
            rx->hunt = t_mid + (rx->clk_period * 0.5);
            continue;
        }

        bits   = rx->data_bits + (rx->parity ? 1 : 0);
        t_stop = t_mid + ((bits + 1) * bit);
        if (t_stop > now) {
            serial_rx_schedule(rx, t_stop);
            return;
        }

        for (int i = 0; i < rx->data_bits; i++) {
            if (serial_line_level(rx->line, t_mid + ((i + 1) * bit))) {
                data |= (1 << i);
                ones++;
                zeros = 0;
            }
        }
        if (rx->parity) {
            int p = serial_line_level(rx->line, t_mid + (bits * bit));

            ones += p;
            if (p)
                zeros = 0;
            par_ok = (rx->parity == 1) ? (ones & 1) : !(ones & 1);
        }

        {
            int stop = serial_line_level(rx->line, t_stop);

            rx->hunt = t_stop + (rx->clk_period * 0.5);
            if (rx->receive != NULL)
                rx->receive(rx->priv, (uint8_t) data, !stop, !par_ok, zeros && !stop);
        }
    }
}

static void
serial_rx_line_changed(void *priv)
{
    serial_rx_update((serial_rx_t *) priv);
}

void
serial_rx_init(serial_rx_t *rx, serial_line_t *line,
               void (*receive)(void *priv, uint8_t data, int framing, int parity, int brk), void *priv)
{
    memset(rx, 0, sizeof(serial_rx_t));

    rx->line      = line;
    rx->data_bits = 8;
    rx->factor    = 16;
    rx->receive   = receive;
    rx->priv      = priv;
    rx->hunt      = (double) tsc;

    timer_add(&rx->timer, serial_rx_timer, rx, 0);

    if (line != NULL) {
        line->changed      = serial_rx_line_changed;
        line->changed_priv = rx;
    }
}

void
serial_rx_close(serial_rx_t *rx)
{
    timer_disable(&rx->timer);

    if ((rx->line != NULL) && (rx->line->changed_priv == rx)) {
        rx->line->changed      = NULL;
        rx->line->changed_priv = NULL;
    }
}

/* A new rate takes effect from the receiver's current hunting point, so a
   character arriving across the change is taken entirely at the new rate. */
void
serial_rx_set_clock(serial_rx_t *rx, double period, double rise)
{
    serial_rx_update(rx);

    rx->clk_period = period;
    rx->clk_rise   = rise;

    serial_rx_update(rx);
}

void
serial_rx_set_format(serial_rx_t *rx, int data_bits, int parity, int factor)
{
    serial_rx_update(rx);

    rx->data_bits = data_bits;
    rx->parity    = parity;
    rx->factor    = factor;

    serial_rx_update(rx);
}

void
serial_rx_enable(serial_rx_t *rx, int enable)
{
    if (enable && !rx->enabled)
        rx->hunt = (double) tsc;

    rx->enabled = enable;
    serial_rx_update(rx);
}
