/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for an asynchronous serial line between two emulated
 *          devices, carried bit by bit.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#ifndef EMU_SERIAL_LINE_H
#define EMU_SERIAL_LINE_H

#define SERIAL_LINE_FRAMES 64

/* One direction of the wire. Times are in TSC units. The line rests at mark
   (1); a frame holds the cells its transmitter put on it, start bit first. */
typedef struct serial_line_t {
    struct {
        double   start;
        double   bit;
        int      cells;
        uint32_t level; /* cell levels, bit 0 = the start bit */
    } frame[SERIAL_LINE_FRAMES];
    int    head;
    int    count;

    double break_from;  /* a continuous space since this time, or < 0 */

    void (*changed)(void *priv);
    void  *changed_priv;
} serial_line_t;

/* An asynchronous receiver that samples the line on the rising edges of its
   own clock, as a USART with a 1x, 16x or 64x clock does. */
typedef struct serial_rx_t {
    serial_line_t *line;

    int    data_bits;
    int    parity;     /* 0 = none, 1 = odd, 2 = even */
    int    factor;     /* clocks per bit */
    double clk_period; /* 0 = no clock */
    double clk_rise;

    double hunt;       /* looking for a start bit from this time */
    int    enabled;

    pc_timer_t timer;

    void (*receive)(void *priv, uint8_t data, int framing, int parity, int brk);
    void  *priv;
} serial_rx_t;

extern void serial_line_init(serial_line_t *line);
extern int  serial_line_level(serial_line_t *line, double t);
/* Queue a frame of 'cells' bit cells (start bit first) from time 'start'. */
extern void serial_line_send(serial_line_t *line, double start, double bit, int cells, uint32_t level);
extern void serial_line_set_break(serial_line_t *line, double t, int on);
/* Cells for a character: start bit, data LSB first, optional parity. */
extern int  serial_line_cells(uint8_t data, int data_bits, int parity, uint32_t *level);

extern void serial_rx_init(serial_rx_t *rx, serial_line_t *line,
                           void (*receive)(void *priv, uint8_t data, int framing, int parity, int brk),
                           void *priv);
extern void serial_rx_close(serial_rx_t *rx);
extern void serial_rx_set_clock(serial_rx_t *rx, double period, double rise);
extern void serial_rx_set_format(serial_rx_t *rx, int data_bits, int parity, int factor);
extern void serial_rx_enable(serial_rx_t *rx, int enable);
/* Catch up with the line; called when it changes. */
extern void serial_rx_update(serial_rx_t *rx);

#endif /*EMU_SERIAL_LINE_H*/
