/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the Cromemco 4FDC Disk Controller, an S-100 card
 *          with an FD1771 and a TMS 5501 multifunction I/O chip, after its
 *          instruction manual (Cromemco, 1977), and of the PerSci 277 voice
 *          coil drives SCP systems ran on it, after the PerSci Model 277
 *          product specification and installation and maintenance manual.
 *
 *          Ports 30h-33h are the FD1771; the card complements the data bus
 *          for it, so the processor sees true values. Port 34h reads the
 *          disk flags and writes the drive control latch: with AUTO WAIT
 *          set, reading it holds the processor in wait states until the
 *          1771 raises DRQ or ends the job. Ports 00h-09h are the 5501: the
 *          serial channel, the interrupt register, the parallel port and
 *          five interval timers. Its parallel output drives the PerSci
 *          restore and fast seek lines, and its input bit 6 carries the
 *          drive's seek in progress.
 *
 *          A PerSci 277 takes step pulses into a track difference register
 *          and moves its voice coil positioner to the new track, holding
 *          Seek Complete off until the head has settled; a low RESTORE line
 *          takes it back to track 00. The head leaves its track with the
 *          first pulse and reads nothing until it has settled.
 *
 *          Not emulated: the RDOS ROM (switch 1 on, as SCP systems run the
 *          card), interrupts to the processor (the 5501 answers INTA with
 *          an 8080 restart, which an 8086 with an SCP-300F does not take),
 *          the serial receiver and anything at the far end of the line, and
 *          the PerSci remote eject and direct head load options.
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
#include "cpu.h"
#include <86box/io.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/video.h>
#include <86box/fdc_wd179x.h>
#include <86box/fdc_cromemco.h>
#include <86box/plat_unused.h>

/* Head load delay after DRIVE SELECT (HLD) is first asserted (p. 32). */
#define C4FDC_HLT_MAXI_US 48000.0
#define C4FDC_HLT_MINI_US 72000.0

/* Port 34h output. */
#define CTRL_AUTO_WAIT 0x80
#define CTRL_MOTOR     0x20
#define CTRL_MAXI      0x10

/* 5501 parallel output: the PerSci lines, active low. */
#define PAR_RESTORE   0x08
#define PAR_FAST_SEEK 0x10

/* 5501 command register. */
#define CMD_RESET 0x01
#define CMD_RS7   0x04
#define CMD_HBD   0x10

/* The PerSci 277 has 77 tracks. A one track seek settles within 10 ms and a
   00 to 76 seek is set up to settle in 95 ms (maintenance manual, p. 2-17);
   in between the time is taken as linear, which the documents do not give.
   Restore is "a low-speed head positioning operation" of unstated speed:
   it is taken as 10 ms a track, the single track figure. */
#define PERSCI_TRACKS     77
#define PERSCI_ONE_MS     10.0
#define PERSCI_FULL_MS    95.0
#define PERSCI_RESTORE_MS 10.0

typedef struct c4fdc_t c4fdc_t;

/* One positioner: a 277 holds two diskettes, left and right, on a common
   positioning actuator with one track 00 sensor (product specification,
   3.3); the 4FDC set-up puts drives A and B on one, C and D on a second
   (p. 37). */
typedef struct persci_t {
    c4fdc_t   *dev;
    int        first;     /* the drive on the left side */
    int        track;     /* where the heads are going */
    int        moving;
    int        restoring;
    int        from;
    double     start;
    double     done;
    pc_timer_t timer;
} persci_t;

typedef struct tms5501_t {
    uint8_t baud;
    uint8_t cmd;     /* latched bits 1-5 */
    uint8_t req;     /* interrupt request register */
    uint8_t mask;
    uint8_t par_out;
    int     tx_full;
    double  tx_done; /* the shift register sends until then */
    int     ore;
    int     timer_on[5];
    double  timer_end[5];
} tms5501_t;

struct c4fdc_t {
    wd179x_t *wd;
    uint8_t   control;
    int       drive;
    double    tsc_per_us;
    double    wait_until;

    int       persci; /* the 8" drives are PerSci 277s */
    int       sw3;    /* BOOT */
    int       inter7; /* DRQ to the 5501's PI7 */
    int       wait4;  /* one wait state on 1771 reads */
    int       auto_wait;

    tms5501_t tms;
    persci_t  ps[2];
};

/* ---- PerSci 277 ------------------------------------------------------------ */

static int
c4fdc_is_persci(c4fdc_t *dev, int drive)
{
    return dev->persci && (drive >= 0) && fdd_type_is_8inch(fdd_get_type(&drives[drive]));
}

static persci_t *
c4fdc_persci(c4fdc_t *dev, int drive)
{
    return &dev->ps[drive >> 1];
}

static double
persci_seek_us(int tracks)
{
    if (tracks <= 0)
        return 0.0;

    return (PERSCI_ONE_MS + ((tracks - 1) * (PERSCI_FULL_MS - PERSCI_ONE_MS) / (PERSCI_TRACKS - 2))) * 1000.0;
}

/* Both diskettes' heads are where the positioner is. */
static void
persci_place(persci_t *ps)
{
    for (int i = ps->first; i < (ps->first + 2); i++) {
        fdd_drive_t *drv = &drives[i];

        if (c4fdc_is_persci(ps->dev, i) && (fdd_current_track(drv) != ps->track))
            fdd_forced_seek(drv, ps->track - fdd_current_track(drv));
    }
}

/* The heads have settled. A restore only finds track 00 by getting there,
   so the 86Box drives move now; a seek moved them pulse by pulse. */
static void
persci_arrive(persci_t *ps)
{
    timer_disable(&ps->timer);
    ps->moving = 0;
    if (ps->restoring) {
        ps->restoring = 0;
        ps->track     = 0;
        persci_place(ps);
    }
}

static void
persci_timer(void *priv)
{
    persci_arrive((persci_t *) priv);
}

static void
persci_schedule(persci_t *ps)
{
    double now   = (double) tsc;
    double delay = (ps->done > now) ? (ceil(ps->done) - now) : 0.0;

    ps->moving = 1;
    timer_set_delay_u64(&ps->timer, ((uint64_t) delay) << 32);
}

/* Seek in progress, settled lazily if the timer has not run yet. */
static int
persci_moving(persci_t *ps)
{
    if (ps->moving && ((double) tsc >= ps->done))
        persci_arrive(ps);

    return ps->moving;
}

static void
persci_step(c4fdc_t *dev, persci_t *ps, int dir)
{
    /* Steps do nothing while RESTORE holds the positioner. */
    if (ps->restoring || !(dev->tms.par_out & PAR_RESTORE))
        return;

    if (!persci_moving(ps)) {
        ps->from  = ps->track;
        ps->start = (double) tsc;
    }

    if (((ps->track + dir) < 0) || ((ps->track + dir) > (PERSCI_TRACKS - 1)))
        return;
    ps->track += dir;
    persci_place(ps);

    ps->done = ps->start + (persci_seek_us(abs(ps->track - ps->from)) * dev->tsc_per_us);
    persci_schedule(ps);
}

static void
persci_restore(c4fdc_t *dev, persci_t *ps)
{
    /* Already settled on track 00: Seek Complete stays on. */
    if ((ps->track == 0) && !persci_moving(ps))
        return;

    ps->restoring = 1;
    ps->from      = ps->track;
    ps->start     = (double) tsc;
    ps->done      = ps->start + (ps->track * PERSCI_RESTORE_MS * 1000.0 * dev->tsc_per_us);
    persci_schedule(ps);
}

/* The drive sees its select line while the latch picks it and the 1771
   loads the head (theory of operation, p. 43). */
static persci_t *
c4fdc_selected_persci(c4fdc_t *dev)
{
    if (!c4fdc_is_persci(dev, dev->drive) || !wd179x_get_hld(dev->wd))
        return NULL;

    return c4fdc_persci(dev, dev->drive);
}

/* ---- FD1771 wiring --------------------------------------------------------- */

/* For a minifloppy the board pulls READY high (p. 43); an 8" drive gives
   its own. */
static int
c4fdc_ready(void *priv, int drive)
{
    const c4fdc_t *dev = (c4fdc_t *) priv;
    fdd_drive_t   *drv;

    if (!(dev->control & CTRL_MAXI))
        return 1;
    if (drive < 0)
        return 0;

    drv = &drives[drive];
    return (fdd_get_type(drv) != 0) && !drv->empty;
}

static void
c4fdc_step(void *priv, int drive, int dir)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    if (drive < 0)
        return;

    if (c4fdc_is_persci(dev, drive))
        persci_step(dev, c4fdc_persci(dev, drive), dir);
    else
        fdd_forced_seek(&drives[drive], dir);
}

static int
c4fdc_settled(void *priv, int drive)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    return !c4fdc_is_persci(dev, drive) || !persci_moving(c4fdc_persci(dev, drive));
}

/* End of job is the 5501's sensor input: a rising edge requests
   interrupt 2. It also ends AUTO WAIT mode (p. 30): software sets it again
   for each transfer. */
static void
c4fdc_intrq(void *priv, int level)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    if (level) {
        dev->tms.req |= 0x04;
        dev->auto_wait = 0;
    }
}

/* With jumper INTER 7, DRQ is PI7, which can request interrupt 7. */
static void
c4fdc_drq(void *priv, int level)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    if (level && dev->inter7 && (dev->tms.cmd & CMD_RS7))
        dev->tms.req |= 0x80;
}

/* ---- TMS 5501 -------------------------------------------------------------- */

/* The interval timers count on a free running 64 us clock, 8 us with HBD;
   hence the "+0, -64 us" accuracy (p. 18). */
static double
tms_tick(c4fdc_t *dev)
{
    return ((dev->tms.cmd & CMD_HBD) ? 8.0 : 64.0) * dev->tsc_per_us;
}

/* One start bit, eight data bits and one or two stop bits; the highest
   rate selected wins, none disables the channel (p. 13). */
static double
tms_char_time(c4fdc_t *dev)
{
    static const double rates[7] = { 110.0, 150.0, 300.0, 1200.0, 2400.0, 4800.0, 9600.0 };
    double              baud     = 0.0;
    int                 bits     = 1 + 8 + ((dev->tms.baud & 0x80) ? 1 : 2);

    for (int i = 6; i >= 0; i--) {
        if (dev->tms.baud & (1 << i)) {
            baud = rates[i];
            break;
        }
    }
    if (baud == 0.0)
        return -1.0;
    if (dev->tms.cmd & CMD_HBD)
        baud *= 8.0;

    return (bits / baud) * 1000000.0 * dev->tsc_per_us;
}

/* Interrupt request bits of the five timers: 0, 1, 3, 6 and 7. */
static const uint8_t tms_timer_bit[5] = { 0x01, 0x02, 0x08, 0x40, 0x80 };

static void
tms_timer_expired(c4fdc_t *dev, int n)
{
    /* Timer 5 reaches interrupt 7 unless RS7 gives it to PI7. */
    if ((n != 4) || !(dev->tms.cmd & CMD_RS7))
        dev->tms.req |= tms_timer_bit[n];
}

/* Bring the timers and the transmitter up to now. */
static void
tms_update(c4fdc_t *dev)
{
    tms5501_t *tms = &dev->tms;
    double     now = (double) tsc;

    for (int i = 0; i < 5; i++) {
        if (tms->timer_on[i] && (now >= tms->timer_end[i])) {
            tms->timer_on[i] = 0;
            tms_timer_expired(dev, i);
        }
    }

    /* A full buffer moves to the shift register once it is free; TBE then
       rises and requests interrupt 5. */
    if (tms->tx_full && (now >= tms->tx_done)) {
        double t = tms_char_time(dev);

        if (t > 0.0) {
            tms->tx_done += t;
            tms->tx_full = 0;
            tms->req |= 0x20;
        }
    }
}

static void
tms_reset_cmd(c4fdc_t *dev)
{
    tms5501_t *tms = &dev->tms;

    /* The receiver hunts again, the transmitter marks with TBE high, the
       interrupt register clears but for TBE, the timers stop (p. 15). */
    tms->ore     = 0;
    tms->tx_full = 0;
    tms->tx_done = (double) tsc;
    tms->req     = 0x20;
    for (int i = 0; i < 5; i++)
        tms->timer_on[i] = 0;
}

static uint8_t
tms_read(c4fdc_t *dev, int reg)
{
    tms5501_t *tms = &dev->tms;
    uint8_t    ret = 0xff;

    tms_update(dev);

    switch (reg) {
        case 0:
            /* As wired by the board: TBE, RDA, IPG, SBD, FBD, SRV, ORE, FME.
               Nothing drives the receiver, so its input idles at mark. */
            ret = (tms->tx_full ? 0x00 : 0x80) | ((tms->req & tms->mask) ? 0x20 : 0x00) | 0x04 |
                  (tms->ore ? 0x02 : 0x00);
            tms->ore = 0;
            break;
        case 1:
            ret = 0x00;
            break;
        case 3: {
            /* The pending interrupt of highest priority, as a restart
               instruction; reading it clears the request. */
            uint8_t pend = tms->req & tms->mask;

            ret = 0xff;
            for (int i = 0; i < 8; i++) {
                if (pend & (1 << i)) {
                    ret = 0xc7 | (i << 3);
                    tms->req &= ~(1 << i);
                    break;
                }
            }
            break;
        }
        case 4: {
            /* PI7 is DRQ with jumper INTER 7; PI6 is the PerSci's seek in
               progress, pulled high without one; J4 leaves the rest open. */
            persci_t *ps = c4fdc_selected_persci(dev);

            ret = 0x3f;
            if (!dev->inter7 || wd179x_get_drq(dev->wd))
                ret |= 0x80;
            if ((ps == NULL) || persci_moving(ps))
                ret |= 0x40;
            break;
        }
        default:
            /* 02 and 05-09 do not answer. */
            break;
    }

    return ret;
}

static void
tms_write(c4fdc_t *dev, int reg, uint8_t val)
{
    tms5501_t *tms = &dev->tms;

    tms_update(dev);

    switch (reg) {
        case 0:
            /* A byte waiting on a disabled channel goes out from now. */
            if (tms->tx_full && (tms->tx_done < (double) tsc))
                tms->tx_done = (double) tsc;
            tms->baud = val;
            break;
        case 1:
            /* With the shift register free the byte goes straight on and
               TBE comes back at once. */
            if ((double) tsc >= tms->tx_done) {
                double t = tms_char_time(dev);

                if (t > 0.0) {
                    tms->tx_done = (double) tsc + t;
                    tms->req |= 0x20;
                } else
                    tms->tx_full = 1;
            } else
                tms->tx_full = 1;
            break;
        case 2: {
            double old = tms_tick(dev);

            tms->cmd = val & 0x3e;
            if (val & CMD_RESET)
                tms_reset_cmd(dev);

            /* HBD changes the timers' clock under them. */
            if (tms_tick(dev) != old) {
                for (int i = 0; i < 5; i++) {
                    if (tms->timer_on[i])
                        tms->timer_end[i] = (double) tsc + ((tms->timer_end[i] - (double) tsc) * tms_tick(dev) / old);
                }
            }
            break;
        }
        case 3:
            tms->mask = val;
            break;
        case 4: {
            uint8_t   fell = tms->par_out & ~val;
            persci_t *ps   = c4fdc_selected_persci(dev);

            tms->par_out = val;
            /* -FAST SEEK is the 1771's TEST input (p. 17). */
            wd179x_set_test(dev->wd, !!(val & PAR_FAST_SEEK));
            if ((fell & PAR_RESTORE) && (ps != NULL))
                persci_restore(dev, ps);
            break;
        }
        default: {
            /* Timers 1-5: a new count restarts the timer without a request;
               a count of zero requests at once. */
            int n = reg - 5;

            if (val == 0) {
                tms->timer_on[n] = 0;
                tms_timer_expired(dev, n);
            } else {
                double tick = tms_tick(dev);

                tms->timer_on[n]  = 1;
                tms->timer_end[n] = (ceil((double) tsc / tick) + (val - 1)) * tick;
            }
            break;
        }
    }
}

/* ---- Disk ports ------------------------------------------------------------ */

static void
c4fdc_control(c4fdc_t *dev, uint8_t val)
{
    int maxi = !!(val & CTRL_MAXI);
    int drive = -1;

    dev->control = val;
    /* The AUTO WAIT flip-flop is held clear while EOJ is high; that it is
       a level and not an edge is inferred from the TTL, not documented. */
    dev->auto_wait = (val & CTRL_AUTO_WAIT) && !wd179x_get_intrq(dev->wd);

    /* Only one drive should be selected; the lowest one is taken. */
    for (int i = 0; i < 4; i++) {
        if (val & (1 << i)) {
            drive = i;
            break;
        }
    }
    dev->drive = drive;

    /* MAXI clocks the 1771 at 2 MHz and picks the 8" head load delay. */
    wd179x_set_clock(dev->wd, maxi ? 2000000.0 : 1000000.0);
    wd179x_set_hlt_delay(dev->wd, maxi ? C4FDC_HLT_MAXI_US : C4FDC_HLT_MINI_US);

    /* MOTOR ON reaches the minifloppies; 8" drives turn from power on. */
    for (int i = 0; i < 4; i++) {
        if (!fdd_type_is_8inch(fdd_get_type(&drives[i])))
            fdd_set_motor_enable(&drives[i], !!(val & CTRL_MOTOR));
    }

    wd179x_set_drive(dev->wd, drive, 0);
}

static uint8_t
c4fdc_read(uint16_t port, void *priv)
{
    c4fdc_t *dev = (c4fdc_t *) priv;
    int      reg = port & 0xff;

    if (reg <= 0x09)
        return tms_read(dev, reg);

    if (reg <= 0x33)
        return wd179x_read(dev->wd, reg & 3);

    /* Disk flags: DRQ, BOOT (low for switch 3 on), HEADLOAD and EOJ; the
       other bits are not assigned and read as zero here. */
    return (wd179x_get_drq(dev->wd) ? 0x80 : 0x00) | (dev->sw3 ? 0x00 : 0x40) |
           (wd179x_get_hld(dev->wd) ? 0x20 : 0x00) | (wd179x_get_intrq(dev->wd) ? 0x01 : 0x00);
}

static void
c4fdc_write(uint16_t port, uint8_t val, void *priv)
{
    c4fdc_t *dev = (c4fdc_t *) priv;
    int      reg = port & 0xff;

    if (reg <= 0x09)
        tms_write(dev, reg, val);
    else if (reg <= 0x33)
        wd179x_write(dev->wd, reg & 3, val);
    else
        c4fdc_control(dev, val);
}

/* Wait states (p. 31 and 42): with AUTO WAIT, port 34h reads until DRQ or
   end of job; one on 1771 reads at 4 MHz; and the 5501 holds the bus until
   the board's own 1 us cycle comes round. */
static int
c4fdc_io_ready(uint16_t port, int out, void *priv)
{
    c4fdc_t *dev = (c4fdc_t *) priv;
    int      reg = port & 0xff;
    double   now = (double) tsc;

    if (!out && (reg == 0x34)) {
        if (dev->auto_wait)
            return wd179x_get_drq(dev->wd) || wd179x_get_intrq(dev->wd);
        return 1;
    }

    if (dev->wait_until < 0.0) {
        if (reg <= 0x09) {
            double us = dev->tsc_per_us;

            dev->wait_until = ceil(now / us) * us;
        } else if (!out && dev->wait4 && (reg >= 0x30) && (reg <= 0x33))
            dev->wait_until = now + 1.0;
        else
            return 1;
    }

    if (now < dev->wait_until)
        return 0;

    dev->wait_until = -1.0;
    return 1;
}

/* The card decodes A0-A7 only. */
static void
c4fdc_handlers(c4fdc_t *dev, int set)
{
    for (int hi = 0; hi < 0x100; hi++) {
        io_handler(set, (hi << 8) | 0x00, 10, c4fdc_read, NULL, NULL, c4fdc_write, NULL, NULL, dev);
        io_handler(set, (hi << 8) | 0x30, 5, c4fdc_read, NULL, NULL, c4fdc_write, NULL, NULL, dev);
    }
}

static void
c4fdc_reset(void *priv)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    /* The 277 seeks track 00 when it is powered on. */
    for (int i = 0; i < 2; i++) {
        timer_disable(&dev->ps[i].timer);
        dev->ps[i].moving    = 0;
        dev->ps[i].restoring = 0;
        dev->ps[i].track     = 0;
        persci_place(&dev->ps[i]);
    }

    /* RESET clears the control latch; the 5501 outputs are taken to come
       up high, every PerSci line off. */
    dev->wait_until  = -1.0;
    dev->tms.baud    = 0x00;
    dev->tms.cmd     = 0x00;
    dev->tms.mask    = 0x00;
    dev->tms.par_out = 0xff;
    tms_reset_cmd(dev);
    wd179x_set_test(dev->wd, 1);
    c4fdc_control(dev, 0x00);
    wd179x_reset(dev->wd);
}

static void *
c4fdc_init(UNUSED(const device_t *info))
{
    c4fdc_t *dev = (c4fdc_t *) calloc(1, sizeof(c4fdc_t));

    dev->tsc_per_us = cpuclock / 1000000.0;
    dev->persci     = device_get_config_int("persci");
    dev->sw3        = device_get_config_int("sw3");
    dev->inter7     = device_get_config_int("inter7");
    dev->wait4      = device_get_config_int("wait4");

    dev->wd = wd179x_create(WD_FD1771, 2000000.0);
    wd179x_set_callbacks(dev->wd, c4fdc_intrq, c4fdc_drq, dev);
    wd179x_set_board(dev->wd, c4fdc_ready, c4fdc_step, c4fdc_settled, dev);
    /* Switch 4 grounds DINT. */
    wd179x_set_dint(dev->wd, !device_get_config_int("sw4"));

    for (int i = 0; i < 2; i++) {
        dev->ps[i].dev   = dev;
        dev->ps[i].first = i * 2;
        timer_add(&dev->ps[i].timer, persci_timer, &dev->ps[i], 0);
    }

    c4fdc_handlers(dev, 1);
    cpu_set_io_ready(c4fdc_io_ready, dev);

    c4fdc_reset(dev);

    return dev;
}

static void
c4fdc_close(void *priv)
{
    c4fdc_t *dev = (c4fdc_t *) priv;

    for (int i = 0; i < 2; i++)
        timer_disable(&dev->ps[i].timer);

    cpu_set_io_ready(NULL, NULL);
    c4fdc_handlers(dev, 0);
    wd179x_close(dev->wd);
    free(dev);
}

static const device_config_t c4fdc_config[] = {
    // clang-format off
    {
        .name           = "persci",
        .description    = "8\" drives",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "PerSci 277 (voice coil, fast seek)", .value = 1 },
            { .description = "Stepper motor (Shugart type)",       .value = 0 },
            { .description = ""                                               }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "sw3",
        .description    = "Switch 3 on (BOOT)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "sw4",
        .description    = "Switch 4 on (initialization inhibit)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "inter7",
        .description    = "Jumper INTER 7 (DRQ to PI7)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "wait4",
        .description    = "Wait state on FD1771 reads (4 MHz)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t cromemco_4fdc_device = {
    .name          = "Cromemco 4FDC Disk Controller",
    .internal_name = "cromemco_4fdc",
    .flags         = 0,
    .local         = 0,
    .init          = c4fdc_init,
    .close         = c4fdc_close,
    .reset         = c4fdc_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = c4fdc_config
};
