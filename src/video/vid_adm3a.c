/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the Lear Siegler ADM-3A display terminal, after
 *          the ADM-3A Maintenance Manual (Lear Siegler).
 *
 *          The terminal is shown in the machine's window and talks to the
 *          computer over a serial line, bit by bit at its own baud rate,
 *          which it derives from its 10.8864 MHz dot clock: the standard
 *          rates come out 1.25% fast (9720 for 9600). Received characters
 *          are acted on at the next scan line, as the TTL logic does, and a
 *          clear screen keeps the receiver busy for 1.73 ms.
 *
 *          Host keys are taken by their (US) legend: each produces the
 *          code of the ADM-3A key with that legend. CTRL clears bits 6 and
 *          5 of the code and SHIFT works as on the terminal. Keys the
 *          terminal has but a PC does not are on Insert (LINE FEED), End
 *          (CLEAR), Pause (BREAK), Right Alt (REPT) and F1 (HERE IS);
 *          Backspace, Tab, the arrows and Home send the CTRL codes the
 *          ADM-3A uses for them.
 *
 *          The beeper sounds the 3.6 kHz LC2 signal for the period of a
 *          retriggerable one-shot (18K and 30 uF on a DM74123, about 157 ms)
 *          fired by a received BEL and by the cursor reaching column 71 as
 *          a character is written; a column overflow cuts it short. The
 *          answerback option sends its message of up to 32 characters for
 *          a received ENQ or the HERE IS key.
 *
 *          Not emulated: the read-back option, 202 modem turnaround and
 *          the extension port.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/keyboard.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/serial_line.h>
#include <86box/sound.h>
#include <86box/thread.h>
#include <86box/video.h>
#include <86box/vid_adm3a.h>
#include <86box/plat_unused.h>

#define ADM3A_ROM_UPPER "roms/video/adm3a/2513u.bin"
#define ADM3A_ROM_LOWER "roms/video/adm3a/2513l.bin"

#define ADM3A_CLOCK     10886400.0 /* dot clock (Figure 4-3) */
#define ADM3A_BAUD_BASE 3110400.0  /* DC1, two pulses per character time */
#define ADM3A_COLS      80
#define ADM3A_ROWS      24
#define ADM3A_DOTS      7          /* dots per character */
#define ADM3A_SCANS     9          /* scan lines per character row */
#define ADM3A_LINE_CHARS 96        /* 80 shown, 16 in retrace */
#define ADM3A_XSIZE     (ADM3A_COLS * ADM3A_DOTS)
/* The rows, then the two scan lines the bottom-line cursor is drawn on. */
#define ADM3A_YLINES    ((ADM3A_ROWS * ADM3A_SCANS) + 2)
#define ADM3A_CLEAR_US  1730.0     /* Figure 4-6 */
#define ADM3A_BOUNCE_US 5560.0     /* keyboard bounce delay */

#define ADM3A_KEY_QUEUE 64

/* Beeper (section 4.3.11, logic sheet 4): the 74123 period is
   0.28 R C (1 + 0.7 / R) with R in kilohms, for 18K and 30 uF. */
#define ADM3A_BEEP_US   (0.28 * 18.0 * 30.0 * (1.0 + (0.7 / 18.0)) * 1000.0)
#define ADM3A_BEEP_HZ   3600.0
#define ADM3A_BEEP_AMP  6000
#define ADM3A_BEEPS     16
#define ADM3A_LINE_END  71 /* the column count that sounds the beeper */
#define ADM3A_ANSWER    32

enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_CTRL,
    KEY_SHIFT,
    KEY_LOCK,
    KEY_REPT,
    KEY_BREAK,
    KEY_CLEAR,
    KEY_HERE_IS
};

typedef struct adm3a_t {
    /* Switches. */
    int    lines24;
    int    lower_case;  /* upper/lower case display option fitted and selected */
    int    cursor_ctrl;
    int    auto_nl;
    int    space_adv;
    int    hdx;
    int    clear_en;
    int    kb_lock_en;
    int    hz50;
    int    data8;
    int    bit8;
    int    parity;      /* 0 none, 1 odd, 2 even */
    int    stop2;
    double baud;

    uint8_t rom_upper[512];
    uint8_t rom_lower[512];

    /* Display memory, as the screen shows it. */
    uint8_t scr[ADM3A_ROWS][ADM3A_COLS];
    int     row;
    int     col;
    int     after_cr;   /* between a Return and the next Line Feed */
    int     esc_state;
    int     esc_row;
    int     kb_locked;

    /* Receiver: the UART holding register, read at the next scan line. */
    serial_line_t *rxd;
    serial_rx_t    rx;
    uint8_t        rx_hold;
    int            rx_full;
    double         busy_until;
    pc_timer_t     doit_timer;

    /* Transmitter. */
    serial_line_t *txd;
    double         tx_free;
    int            tx_busy;
    pc_timer_t     tx_timer;
    uint8_t        echo;
    int            echo_full;

    /* Keyboard, filled by the host and emptied on the emulation thread. */
    mutex_t   *key_lock;
    uint16_t   key_queue[ADM3A_KEY_QUEUE];
    int        key_head;
    int        key_count;
    uint8_t    key_down[512];
    int        shift;
    int        ctrl;
    int        alpha_lock;
    int        rept;
    uint16_t   rept_scan;
    uint8_t    pending[ADM3A_KEY_QUEUE];
    int        pending_count;
    pc_timer_t key_timer;
    double     last_key;

    double     tsc_per_us;
    double     line_time;
    uint64_t   frame_time;
    pc_timer_t frame_timer;

    /* Beeper: the one-shot's periods, oldest first. */
    double   beep_on[ADM3A_BEEPS];
    double   beep_off[ADM3A_BEEPS];
    int      beeps;
    double   mix_tsc;

    /* Answerback option: empty when it is not fitted. */
    uint8_t  answer[ADM3A_ANSWER];
    int      answer_len;

    uint32_t fg;
    uint32_t bg;
} adm3a_t;

/* Legends of a US PC keyboard, set 1 make codes 00h-3Fh: unshifted, shifted. */
static const char adm3a_legend[2][0x40] = {
    { 0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,   0,
      'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0,   0,   'a', 's',
      'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
      'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0 },
    { 0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,   0,
      'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,   'A', 'S',
      'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
      'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0 }
};

/* ---- Beeper ------------------------------------------------------------- */

/* Fire the one-shot; it retriggers, so a new pulse while it runs extends
   the beep. */
static void
adm3a_beep(adm3a_t *dev)
{
    double now = (double) tsc;
    double end = now + (ADM3A_BEEP_US * dev->tsc_per_us);

    if (dev->beeps && (dev->beep_off[dev->beeps - 1] > now)) {
        dev->beep_off[dev->beeps - 1] = end;
        return;
    }

    if (dev->beeps == ADM3A_BEEPS) {
        memmove(dev->beep_on, dev->beep_on + 1, (ADM3A_BEEPS - 1) * sizeof(double));
        memmove(dev->beep_off, dev->beep_off + 1, (ADM3A_BEEPS - 1) * sizeof(double));
        dev->beeps--;
    }
    dev->beep_on[dev->beeps]  = now;
    dev->beep_off[dev->beeps] = end;
    dev->beeps++;
}

/* CLRBEL: the one-shot is cleared. */
static void
adm3a_beep_clear(adm3a_t *dev)
{
    double now = (double) tsc;

    if (dev->beeps && (dev->beep_off[dev->beeps - 1] > now))
        dev->beep_off[dev->beeps - 1] = now;
}

/* The speaker gets LC2 NANDed with the one-shot's output: a square wave
   while it runs, nothing otherwise. */
static void
adm3a_get_buffer(int32_t *buffer, uint16_t len, void *priv)
{
    adm3a_t *dev  = (adm3a_t *) priv;
    double   now  = (double) tsc;
    double   from = dev->mix_tsc;
    double   half = (dev->tsc_per_us * 1000000.0) / (ADM3A_BEEP_HZ * 2.0);
    int      b    = 0;

    if ((from <= 0.0) || (from > now))
        from = now;

    for (int i = 0; (i < len) && dev->beeps; i++) {
        double t = from + (((now - from) * i) / len);

        while ((b < dev->beeps) && (dev->beep_off[b] <= t))
            b++;
        if ((b < dev->beeps) && (dev->beep_on[b] <= t)) {
            int32_t v = (((uint64_t) (t / half)) & 1) ? ADM3A_BEEP_AMP : -ADM3A_BEEP_AMP;

            buffer[2 * i] += v;
            buffer[(2 * i) + 1] += v;
        }
    }

    /* Forget the periods that are over. */
    b = 0;
    while ((b < dev->beeps) && (dev->beep_off[b] <= now))
        b++;
    if (b) {
        memmove(dev->beep_on, dev->beep_on + b, (dev->beeps - b) * sizeof(double));
        memmove(dev->beep_off, dev->beep_off + b, (dev->beeps - b) * sizeof(double));
        dev->beeps -= b;
    }

    dev->mix_tsc = now;
}

/* ---- Display ------------------------------------------------------------- */

static int
adm3a_rows(adm3a_t *dev)
{
    return dev->lines24 ? ADM3A_ROWS : (ADM3A_ROWS / 2);
}

/* The screen row a cursor row is shown on: in 12-line mode the row
   counters' low bit is held set, so only the odd rows are used. */
static int
adm3a_screen_row(adm3a_t *dev, int row)
{
    return dev->lines24 ? row : ((row * 2) + 1);
}

static int
adm3a_bottom(adm3a_t *dev)
{
    return adm3a_rows(dev) - 1;
}

static void
adm3a_scroll(adm3a_t *dev)
{
    int last = adm3a_bottom(dev);

    for (int r = 0; r < last; r++)
        memcpy(dev->scr[adm3a_screen_row(dev, r)], dev->scr[adm3a_screen_row(dev, r + 1)], ADM3A_COLS);
    memset(dev->scr[adm3a_screen_row(dev, last)], ' ', ADM3A_COLS);
}

/* Line feed: down a row, or scroll the page up from the bottom row. */
static void
adm3a_down(adm3a_t *dev)
{
    if (dev->cursor_ctrl && (dev->row < adm3a_bottom(dev)))
        dev->row++;
    else
        adm3a_scroll(dev);

    dev->after_cr = 0;
}

/* Clear screen: blank the page and put the cursor home, top left with
   cursor control and bottom left without it. */
static void
adm3a_clear(adm3a_t *dev)
{
    memset(dev->scr, ' ', sizeof(dev->scr));
    dev->row       = dev->cursor_ctrl ? 0 : adm3a_bottom(dev);
    dev->col       = 0;
    dev->after_cr  = 0;
    dev->esc_state = 0;
    dev->kb_locked = 0;

    dev->busy_until = (double) tsc + (ADM3A_CLEAR_US * dev->tsc_per_us);
}

/* Forespace after a character; past column 79 the overflow either wraps to
   a new line (Auto New Line) or keeps the cursor in the last column. */
static void
adm3a_forespace(adm3a_t *dev)
{
    if (dev->col < (ADM3A_COLS - 1)) {
        dev->col++;
        return;
    }

    /* OVERFLOW clears the beeper's one-shot, so that at high baud rates
       long lines do not keep it on. */
    adm3a_beep_clear(dev);

    if (dev->auto_nl) {
        dev->col = 0;
        adm3a_down(dev);
    }
}

static void adm3a_tx_start(adm3a_t *dev);

/* The answerback message goes out through the keyboard's code lines. */
static void
adm3a_answerback(adm3a_t *dev)
{
    for (int i = 0; (i < dev->answer_len) && (dev->pending_count < ADM3A_KEY_QUEUE); i++)
        dev->pending[dev->pending_count++] = dev->answer[i];

    adm3a_tx_start(dev);
}

/* local: the character is the terminal's own, looped back in half duplex. */
static void
adm3a_execute(adm3a_t *dev, uint8_t c, int local)
{
    c &= 0x7f;

    /* Load cursor: ESC = row column, each an offset from space. */
    if (dev->esc_state) {
        if ((dev->esc_state == 1) && (c != '=')) {
            dev->esc_state = 0;
        } else if (dev->esc_state == 1) {
            dev->esc_state = 2;
            return;
        } else if (dev->esc_state == 2) {
            dev->esc_row   = c;
            dev->esc_state = 3;
            return;
        } else {
            int r = (dev->esc_row - 0x20) & 0x1f;
            /* Bits 7 and 6 are folded so that space through 'o' give
               columns 0 through 79 (section 4.3.4). */
            int k = (c - 0x20) & 0x7f;

            dev->esc_state = 0;
            if (r > adm3a_bottom(dev))
                r = 0;
            if (k > (ADM3A_COLS - 1))
                k = dev->auto_nl ? 0 : (ADM3A_COLS - 1);
            dev->row = r;
            dev->col = k;
            return;
        }
    }

    if (c >= 0x20) {
        if (c == 0x7f)
            return; /* DEL is not displayed and does not advance */

        /* With SPACE-ADV at ADV, a space between Return and Line Feed only
           moves the cursor. */
        if ((c != ' ') || !dev->space_adv || !dev->after_cr)
            dev->scr[adm3a_screen_row(dev, dev->row)][dev->col] = c;
        adm3a_forespace(dev);
        /* LINE END: the written character brings the column count to 71,
           warning of the end of the line. */
        if (dev->col == ADM3A_LINE_END)
            adm3a_beep(dev);
        return;
    }

    switch (c) {
        case 0x05: /* ENQ: answerback; KSDLY keeps a local ENQ from it */
            if (!local && dev->answer_len)
                adm3a_answerback(dev);
            break;
        case 0x07: /* BEL */
            adm3a_beep(dev);
            break;
        case 0x08: /* BS */
            if (dev->col > 0)
                dev->col--;
            break;
        case 0x0a: /* LF */
            adm3a_down(dev);
            break;
        case 0x0b: /* VT: up line */
            if (dev->cursor_ctrl && (dev->row > 0))
                dev->row--;
            break;
        case 0x0c: /* FF: forespace */
            if (dev->cursor_ctrl)
                adm3a_forespace(dev);
            break;
        case 0x0d: /* CR */
            dev->col      = 0;
            dev->after_cr = 1;
            break;
        case 0x0e: /* SO: unlock keyboard */
            dev->kb_locked = 0;
            break;
        case 0x0f: /* SI: lock keyboard */
            if (dev->kb_lock_en)
                dev->kb_locked = 1;
            break;
        case 0x1a: /* SUB: clear screen */
            if (dev->clear_en)
                adm3a_clear(dev);
            break;
        case 0x1b: /* ESC */
            if (dev->cursor_ctrl)
                dev->esc_state = 1;
            break;
        case 0x1e: /* RS: home */
            if (dev->cursor_ctrl) {
                dev->row = 0;
                dev->col = 0;
            }
            break;
        default:
            break;
    }
}

/* ---- Receiver -------------------------------------------------------------- */

static double
adm3a_next_line(adm3a_t *dev, double t)
{
    return ceil(t / dev->line_time) * dev->line_time;
}

static void
adm3a_doit_schedule(adm3a_t *dev)
{
    double now  = (double) tsc;
    double when = now;

    if (!dev->rx_full && !dev->echo_full)
        return;

    if (dev->busy_until > when)
        when = dev->busy_until;
    /* Timers fire on whole TSC ticks: round up. */
    when = ceil(adm3a_next_line(dev, when));

    timer_set_delay_u64(&dev->doit_timer, ((uint64_t) (when - now)) << 32);
}

/* DOIT: the last character received is executed during a scan line. */
static void
adm3a_doit(void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;

    if ((double) tsc < dev->busy_until) {
        adm3a_doit_schedule(dev);
        return;
    }

    if (dev->echo_full) {
        dev->echo_full = 0;
        adm3a_execute(dev, dev->echo, 1);
    } else if (dev->rx_full) {
        dev->rx_full = 0;
        adm3a_execute(dev, dev->rx_hold, 0);
    }

    adm3a_doit_schedule(dev);
}

static void
adm3a_receive(void *priv, uint8_t data, UNUSED(int framing), UNUSED(int parity), UNUSED(int brk))
{
    adm3a_t *dev = (adm3a_t *) priv;

    /* An unread character is overwritten; IN8 is not used. */
    dev->rx_hold = data & 0x7f;
    dev->rx_full = 1;
    adm3a_doit_schedule(dev);
}

/* ---- Transmitter ----------------------------------------------------------- */

static void
adm3a_tx_done(void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;

    dev->tx_busy = 0;

    /* Half duplex: the transmitted data is ORed into the receiver. */
    if (dev->hdx) {
        dev->echo      = dev->pending[0] & 0x7f;
        dev->echo_full = 1;
        adm3a_doit_schedule(dev);
    }

    memmove(dev->pending, dev->pending + 1, --dev->pending_count);
}

static void
adm3a_tx_start(adm3a_t *dev)
{
    double   now = (double) tsc;
    double   bit;
    double   clk;
    double   start;
    uint32_t level;
    int      cells;
    uint8_t  word;

    if (dev->tx_busy || !dev->pending_count || (dev->txd == NULL))
        return;

    bit   = dev->tsc_per_us * 1000000.0 / dev->baud;
    clk   = bit / 16.0;
    start = ceil(now / clk) * clk;

    word  = dev->pending[0] & 0x7f;
    if (dev->data8 && dev->bit8)
        word |= 0x80;
    cells = serial_line_cells(word, dev->data8 ? 8 : 7, dev->parity, &level);
    serial_line_send(dev->txd, start, bit, cells, level);

    dev->tx_busy = 1;
    dev->tx_free = start + ((cells + (dev->stop2 ? 2 : 1)) * bit);
    timer_set_delay_u64(&dev->tx_timer, (uint64_t) ((dev->tx_free - now) * (double) (1ULL << 32)));
}

/* ---- Keyboard ---------------------------------------------------------------- */

static void
adm3a_host_key(uint16_t scan, int down, void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;

    thread_wait_mutex(dev->key_lock);
    if (dev->key_count < ADM3A_KEY_QUEUE) {
        dev->key_queue[(dev->key_head + dev->key_count) % ADM3A_KEY_QUEUE] = scan | (down ? 0x8000 : 0);
        dev->key_count++;
    }
    thread_release_mutex(dev->key_lock);
}

static int
adm3a_key_kind(uint16_t scan, uint8_t *code)
{
    *code = 0;

    switch (scan) {
        case 0x001:
            *code = 0x1b;
            return KEY_CHAR;
        case 0x00e:
            *code = 0x08; /* CTRL-H */
            return KEY_CHAR;
        case 0x00f:
            *code = 0x09; /* CTRL-I */
            return KEY_CHAR;
        case 0x01c:
        case 0x11c:
            *code = 0x0d;
            return KEY_CHAR;
        case 0x01d:
        case 0x11d:
            return KEY_CTRL;
        case 0x02a:
        case 0x036:
            return KEY_SHIFT;
        case 0x03a:
            return KEY_LOCK;
        case 0x138:
            return KEY_REPT;
        case 0x100:
        case 0x146:
            return KEY_BREAK;
        case 0x14f:
            return KEY_CLEAR;
        case 0x03b:
            return KEY_HERE_IS;
        case 0x147:
            *code = 0x1e; /* CTRL with the HOME key */
            return KEY_CHAR;
        case 0x148:
            *code = 0x0b; /* up: CTRL-K */
            return KEY_CHAR;
        case 0x150:
            *code = 0x0a; /* down: CTRL-J */
            return KEY_CHAR;
        case 0x14b:
            *code = 0x08; /* left: CTRL-H */
            return KEY_CHAR;
        case 0x14d:
            *code = 0x0c; /* right: CTRL-L */
            return KEY_CHAR;
        case 0x152:
            *code = 0x0a; /* LINE FEED */
            return KEY_CHAR;
        case 0x153:
            *code = 0x7f; /* SHIFT with RUB */
            return KEY_CHAR;
        case 0x135:
            *code = '/';
            return KEY_CHAR;
        default:
            break;
    }

    /* The keypad by its legends. */
    if ((scan >= 0x047) && (scan <= 0x053)) {
        static const char pad[] = "789-456+1230.";

        *code = pad[scan - 0x047];
        return KEY_CHAR;
    }

    if ((scan < 0x040) && adm3a_legend[0][scan]) {
        *code = 1; /* resolved with the modifiers */
        return KEY_CHAR;
    }

    return KEY_NONE;
}

static uint8_t
adm3a_key_code(adm3a_t *dev, uint16_t scan, uint8_t code)
{
    if ((scan < 0x040) && adm3a_legend[0][scan] && (code == 1)) {
        code = (uint8_t) adm3a_legend[dev->shift ? 1 : 0][scan];
        if (dev->alpha_lock && (code >= 'a') && (code <= 'z'))
            code -= 0x20;
    }

    /* CTRL forces bits 7 and 6 (0x40, 0x20) to zero (section 4.3.10). */
    if (dev->ctrl)
        code &= 0x1f;

    return code;
}

static void
adm3a_key_event(adm3a_t *dev, uint16_t scan, int down)
{
    uint8_t code;
    int     kind = adm3a_key_kind(scan, &code);

    if (scan >= 0x200)
        return;

    /* Host auto-repeat sends more presses; the ADM-3A only repeats with REPT. */
    if (dev->key_down[scan] == down)
        return;
    dev->key_down[scan] = down;

    switch (kind) {
        case KEY_CTRL:
            dev->ctrl = dev->key_down[0x01d] || dev->key_down[0x11d];
            return;
        case KEY_SHIFT:
            dev->shift = dev->key_down[0x02a] || dev->key_down[0x036];
            return;
        case KEY_LOCK:
            if (down)
                dev->alpha_lock ^= 1;
            return;
        case KEY_REPT:
            dev->rept = down;
            return;
        case KEY_BREAK:
            /* The transmit line is held at space while the key is down. */
            if (dev->txd != NULL)
                serial_line_set_break(dev->txd, (double) tsc, down);
            return;
        case KEY_CLEAR:
            /* SHIFT with CLEAR: a local clear, which also unlocks the keyboard. */
            if (down && dev->shift)
                adm3a_clear(dev);
            return;
        case KEY_HERE_IS:
            /* Without the answerback option the key does nothing. */
            if (down && !dev->kb_locked && dev->answer_len)
                adm3a_answerback(dev);
            return;
        case KEY_CHAR:
            if (!down) {
                if (dev->rept_scan == scan)
                    dev->rept_scan = 0;
                return;
            }
            if (dev->kb_locked)
                return;
            if (dev->pending_count < ADM3A_KEY_QUEUE)
                dev->pending[dev->pending_count++] = adm3a_key_code(dev, scan, code);
            dev->rept_scan = scan;
            dev->last_key  = (double) tsc;
            adm3a_tx_start(dev);
            return;
        default:
            return;
    }
}

/* The keyboard logic takes one key per bounce delay; REPT repeats the held
   key at the power line rate divided by 4.8 (12.5 per second at 60 Hz). */
static void
adm3a_key_poll(void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;
    uint16_t ev  = 0;
    int      have;
    double   rept = (dev->hz50 ? 100000.0 : 80000.0) * dev->tsc_per_us;

    thread_wait_mutex(dev->key_lock);
    have = (dev->key_count > 0);
    if (have) {
        ev            = dev->key_queue[dev->key_head];
        dev->key_head = (dev->key_head + 1) % ADM3A_KEY_QUEUE;
        dev->key_count--;
    }
    thread_release_mutex(dev->key_lock);

    if (have)
        adm3a_key_event(dev, ev & 0x1ff, !!(ev & 0x8000));
    else if (dev->rept && dev->rept_scan && !dev->kb_locked &&
             (((double) tsc - dev->last_key) >= rept) && !dev->tx_busy && !dev->pending_count) {
        uint8_t code;

        adm3a_key_kind(dev->rept_scan, &code);
        dev->pending[dev->pending_count++] = adm3a_key_code(dev, dev->rept_scan, code);
        dev->last_key = (double) tsc;
        adm3a_tx_start(dev);
    }

    adm3a_tx_start(dev);
    timer_advance_u64(&dev->key_timer, (uint64_t) (ADM3A_BOUNCE_US * TIMER_USEC));
}

/* ---- Video ---------------------------------------------------------------- */

static void
adm3a_render(adm3a_t *dev)
{
    int crow = adm3a_screen_row(dev, dev->row);

    for (int y = 0; y < ADM3A_YLINES; y++) {
        uint32_t *p0 = &buffer32->line[y * 2][0];
        uint32_t *p1 = &buffer32->line[(y * 2) + 1][0];
        int       row  = y / ADM3A_SCANS;
        int       scan = y % ADM3A_SCANS;

        for (int col = 0; col < ADM3A_COLS; col++) {
            uint8_t dots = 0x00;
            int     inv  = 0;

            if (row < ADM3A_ROWS) {
                uint8_t c = dev->scr[row][col];

                /* Dot rows 1 to 7 hold the character, 0 and 8 the spacing. */
                if ((scan >= 1) && (scan <= 7)) {
                    if ((c >= 0x60) && dev->lower_case)
                        dots = dev->rom_lower[((0x7f - c) << 3) | scan];
                    else
                        dots = dev->rom_upper[((c & 0x3f) << 3) | scan];
                    dots &= 0x1f;
                }

                /* 12-line mode blanks the even rows. */
                if (!dev->lines24 && !(row & 1))
                    dots = 0x00;

                if (dev->cursor_ctrl && (row == crow) && (col == dev->col))
                    inv = 1;
            } else if (!dev->cursor_ctrl && (col == dev->col)) {
                /* The double underline, on the two lines below the page. */
                dots = 0x1f;
            }

            for (int d = 0; d < ADM3A_DOTS; d++) {
                int      on  = (d < 5) ? ((dots >> (4 - d)) & 1) : 0;
                uint32_t pix = (on ^ inv) ? dev->fg : dev->bg;

                p0[(col * ADM3A_DOTS) + d] = pix;
                p1[(col * ADM3A_DOTS) + d] = pix;
            }
        }
    }
}

static void
adm3a_poll(void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;
    int      ys  = ADM3A_YLINES * 2;

    timer_advance_u64(&dev->frame_timer, dev->frame_time);

    if ((xsize != ADM3A_XSIZE) || (ysize != ys) || video_force_resize_get()) {
        xsize = ADM3A_XSIZE;
        ysize = ys;
        set_screen_size(xsize, ysize);

        if (video_force_resize_get())
            video_force_resize_set(0);
    }

    video_wait_for_buffer();
    adm3a_render(dev);
    video_blit_memtoscreen(0, 0, xsize, ysize);

    frames++;
    video_res_x = ADM3A_COLS;
    video_res_y = adm3a_rows(dev);
    video_bpp   = 0;
}

static void
adm3a_timebase(adm3a_t *dev)
{
    int frame_rows = ADM3A_ROWS + (dev->hz50 ? 12 : 6);

    dev->tsc_per_us = cpuclock / 1000000.0;
    dev->line_time  = (ADM3A_LINE_CHARS * ADM3A_DOTS) * (cpuclock / ADM3A_CLOCK);
    dev->frame_time = (uint64_t) (dev->line_time * frame_rows * ADM3A_SCANS * (double) (1ULL << 32));

    if (dev->rxd != NULL)
        serial_rx_set_clock(&dev->rx, dev->tsc_per_us * 1000000.0 / (dev->baud * 16.0), 0.0);
}

/* ---- Device ---------------------------------------------------------------- */

void
adm3a_attach(void *priv, serial_line_t *tx, serial_line_t *rx)
{
    adm3a_t *dev = (adm3a_t *) priv;

    dev->txd = tx;
    dev->rxd = rx;

    serial_rx_init(&dev->rx, rx, adm3a_receive, dev);
    serial_rx_set_format(&dev->rx, dev->data8 ? 8 : 7, dev->parity, 16);
    adm3a_timebase(dev);
    serial_rx_enable(&dev->rx, 1);
}

static int
adm3a_load_rom(const char *fn, uint8_t *buf)
{
    FILE *fp = rom_fopen(fn, "rb");
    int   ret;

    if (fp == NULL)
        return 0;

    ret = (fread(buf, 1, 512, fp) == 512);
    fclose(fp);

    return ret;
}

/* Rates from the DC1 pulse stream (Figure 4-15): divided by 5, then by
   powers of two, and by 32 for the doubled 16x clock; 1800 and 110 baud
   use their own divider chains. */
static double
adm3a_baud(int sel)
{
    switch (sel) {
        case 110:
            return ADM3A_BAUD_BASE / 5.0 / 16.0 / 11.0 / 32.0;
        case 1800:
            return ADM3A_BAUD_BASE / 6.0 / 9.0 / 32.0;
        default:
            for (int k = 0; k <= 8; k++) {
                if ((19200 >> k) == sel)
                    return ADM3A_BAUD_BASE / 5.0 / (double) (1 << k) / 32.0;
            }
            if (sel == 75)
                return ADM3A_BAUD_BASE / 5.0 / 256.0 / 32.0;
            return ADM3A_BAUD_BASE / 5.0 / 2.0 / 32.0;
    }
}

static void *
adm3a_init(UNUSED(const device_t *info))
{
    adm3a_t *dev = (adm3a_t *) calloc(1, sizeof(adm3a_t));
    int      format;

    if (!adm3a_load_rom(ADM3A_ROM_UPPER, dev->rom_upper)) {
        free(dev);
        return NULL;
    }

    dev->lines24     = device_get_config_int("lines24");
    dev->lower_case  = device_get_config_int("lower_case") && adm3a_load_rom(ADM3A_ROM_LOWER, dev->rom_lower);
    dev->cursor_ctrl = device_get_config_int("cursor_control");
    dev->auto_nl     = device_get_config_int("auto_new_line");
    dev->space_adv   = device_get_config_int("space_adv");
    dev->hdx         = device_get_config_int("half_duplex");
    dev->clear_en    = device_get_config_int("clear_screen");
    dev->kb_lock_en  = device_get_config_int("kb_lock");
    dev->hz50        = device_get_config_int("hz50");
    dev->baud        = adm3a_baud(device_get_config_int("baud"));

    /* The answerback PROM's message; ^X stands for CTRL-X, ^^ for ^. */
    {
        const char *msg = device_get_config_string("answerback");

        for (int i = 0; (msg != NULL) && msg[i] && (dev->answer_len < ADM3A_ANSWER); i++) {
            uint8_t c = (uint8_t) msg[i];

            if ((c == '^') && msg[i + 1]) {
                c = (uint8_t) msg[++i];
                if (c != '^')
                    c &= 0x1f;
            }
            dev->answer[dev->answer_len++] = c & 0x7f;
        }
    }

    format     = device_get_config_int("format");
    dev->data8 = !!(format & 0x10);
    dev->bit8  = !!(format & 0x20);
    dev->parity = format & 0x03;
    dev->stop2 = !!(format & 0x40);

    dev->fg = 0xffffff;
    dev->bg = 0x000000;

    dev->key_lock = thread_create_mutex();

    video_inform(VIDEO_FLAG_TYPE_NONE, NULL);
    overscan_x = overscan_y = 0;

    adm3a_timebase(dev);

    /* Power on clears the screen. */
    adm3a_clear(dev);
    dev->busy_until = 0.0;

    timer_add(&dev->doit_timer, adm3a_doit, dev, 0);
    timer_add(&dev->tx_timer, adm3a_tx_done, dev, 0);
    timer_add(&dev->key_timer, adm3a_key_poll, dev, 0);
    timer_set_delay_u64(&dev->key_timer, (uint64_t) (ADM3A_BOUNCE_US * TIMER_USEC));
    timer_add(&dev->frame_timer, adm3a_poll, dev, 0);
    timer_set_delay_u64(&dev->frame_timer, dev->frame_time);

    keyboard_set_input_handler(adm3a_host_key, dev);
    sound_add_handler(adm3a_get_buffer, dev);

    return dev;
}

static void
adm3a_close(void *priv)
{
    adm3a_t *dev = (adm3a_t *) priv;

    keyboard_set_input_handler(NULL, NULL);

    if (dev->rxd != NULL)
        serial_rx_close(&dev->rx);

    thread_close_mutex(dev->key_lock);
    free(dev);
}

static void
adm3a_speed_changed(void *priv)
{
    adm3a_timebase((adm3a_t *) priv);
}

static int
adm3a_available(void)
{
    return rom_present(ADM3A_ROM_UPPER);
}

static const device_config_t adm3a_config[] = {
    // clang-format off
    {
        .name           = "baud",
        .description    = "Baud rate",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 9600,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "75",    .value = 75    },
            { .description = "110",   .value = 110   },
            { .description = "150",   .value = 150   },
            { .description = "300",   .value = 300   },
            { .description = "600",   .value = 600   },
            { .description = "1200",  .value = 1200  },
            { .description = "1800",  .value = 1800  },
            { .description = "2400",  .value = 2400  },
            { .description = "4800",  .value = 4800  },
            { .description = "9600",  .value = 9600  },
            { .description = "19200", .value = 19200 },
            { .description = ""                      }
        },
        .bios           = { { 0 } }
    },
    {
        /* DATA-7/8 (0x10), BIT 8-0/1 (0x20), PARITY-INH and ODD/EVEN (0x03),
           STOP-1/2 (0x40). */
        .name           = "format",
        .description    = "Word structure",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 0x50,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "7 data bits, even parity, 1 stop bit",  .value = 0x02 },
            { .description = "7 data bits, odd parity, 1 stop bit",   .value = 0x01 },
            { .description = "7 data bits, no parity, 2 stop bits",   .value = 0x40 },
            { .description = "8 data bits (8th = 0), 1 stop bit",     .value = 0x10 },
            { .description = "8 data bits (8th = 0), 2 stop bits",    .value = 0x50 },
            { .description = "8 data bits (8th = 1), 1 stop bit",     .value = 0x30 },
            { .description = "8 data bits (8th = 1), 2 stop bits",    .value = 0x70 },
            { .description = ""                                                     }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "lines24",
        .description    = "24-line display option",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "lower_case",
        .description    = "Upper/lower case display option",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "cursor_control",
        .description    = "Cursor control (reverse block cursor)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "auto_new_line",
        .description    = "Auto new line",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "space_adv",
        .description    = "Non-destructive space after Return (SPACE-ADV)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "half_duplex",
        .description    = "Half duplex",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "clear_screen",
        .description    = "Clear screen code (CTRL-Z)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "kb_lock",
        .description    = "Keyboard lock code (CTRL-O)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "hz50",
        .description    = "50 Hz refresh",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        /* The answerback option board's PROM, up to 32 characters; empty
           when the option is not fitted. ^M stands for CTRL-M and so on. */
        .name           = "answerback",
        .description    = "Answerback message (^M = CTRL-M)",
        .type           = CONFIG_STRING,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "",
        .description    = "",
        .type           = CONFIG_END
    }
    // clang-format on
};

const device_t adm3a_device = {
    .name          = "Lear Siegler ADM-3A",
    .internal_name = "adm3a",
    .flags         = 0,
    .local         = 0,
    .init          = adm3a_init,
    .close         = adm3a_close,
    .reset         = NULL,
    .available     = adm3a_available,
    .speed_changed = adm3a_speed_changed,
    .force_redraw  = NULL,
    .config        = adm3a_config
};
