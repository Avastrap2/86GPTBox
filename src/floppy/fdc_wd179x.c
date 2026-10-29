/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the Western Digital FD179X floppy disk
 *          formatter/controller, after the FD179X-02 data sheet (May 1980),
 *          and of the single density FD1771 that came before it, after the
 *          FD1771-01 data sheet (Western Digital 1981 Product Handbook).
 *
 *          The chip reads and writes the disk bit cell by bit cell through
 *          the 86F engine: it finds address marks in the FM or MFM stream,
 *          checks the CRCs and times its gaps in bytes off the turning disk,
 *          as the real one does. Stepping, settling and head load times run
 *          on a timer. Drives it is not working with are not polled; when
 *          they are needed again their rotation is caught up by the time
 *          that passed.
 *
 *          The FD1771 differs in its command bits (b for the sector length
 *          code, a1 a0 for the four data marks, s for Read Track, no side
 *          compare), its status (two record type bits), its timing (10 ms
 *          head load delay, TEST for fast stepping, three index pulses
 *          to find a field, head unloaded at the third index pulse when
 *          idle) and in loading the sector number on Read Address.
 *
 *          Not emulated: the READY transition interrupts of Force Interrupt
 *          (I0, I1), write precompensation, and Write Track at a density
 *          other than the track's own.
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
#include <86box/fdd.h>
#include <86box/fdd_86f.h>
#include <86box/video.h>
#include <86box/fdc_wd179x.h>
#include <86box/plat_unused.h>

extern volatile int fdcinited;

#ifdef ENABLE_WD179X_LOG
int wd179x_do_log = ENABLE_WD179X_LOG;

static void
wd179x_log(const char *fmt, ...)
{
    va_list ap;

    if (wd179x_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define wd179x_log(fmt, ...)
#endif

#define ST_BUSY    0x01
#define ST_INDEX   0x02 /* Type I */
#define ST_DRQ     0x02 /* Types II and III */
#define ST_TRACK0  0x04 /* Type I */
#define ST_LOST    0x04 /* Types II and III */
#define ST_CRC     0x08
#define ST_SEEK    0x10 /* Type I */
#define ST_RNF     0x10 /* Types II and III */
#define ST_HEAD    0x20 /* Type I: head loaded */
#define ST_RECTYPE 0x20 /* Read Sector: deleted data mark */
#define ST_WPROT   0x40
#define ST_NREADY  0x80

#define WD_IS_1771(dev) ((dev)->chip == WD_FD1771)

/* Timed phases. */
enum {
    PH_NONE = 0,
    PH_STEP,     /* waiting out the step rate after a step pulse */
    PH_SETTLE,   /* 15 ms settling before verify (FD1771: head load delay) */
    PH_HEAD,     /* E flag delay, or waiting for HLT */
    PH_UNLOAD    /* idle: unload the head after 15 revolutions */
};

/* Bit engine states, while the disk turns under the head. */
enum {
    BS_IDLE = 0,
    BS_FIND_ID,
    BS_READ_ID,
    BS_FIND_DAM,
    BS_READ_DATA,
    BS_WRITE_GAP,
    BS_WRITE,
    BS_TRACK_INDEX,
    BS_READ_TRACK,
    BS_WRITE_TRACK
};

/* What a written byte is. */
enum {
    WB_NORMAL = 0,
    WB_MARK,     /* FM address mark: data with the C7 clock */
    WB_INDEX,    /* FM index mark: FC with the D7 clock */
    WB_A1,       /* MFM A1 with a missing clock */
    WB_C2,       /* MFM C2 with a missing clock */
    WB_RAW
};

struct wd179x_t {
    int      chip;
    uint8_t  status;
    uint8_t  command;
    uint8_t  track;
    uint8_t  sector;
    uint8_t  data;
    int      type;     /* type of the last command, 1 to 4 */
    int      busy;
    int      drq;
    int      intrq;
    int      imm_irq;  /* Force Interrupt I3: held until a D0 */
    int      idx_irq;  /* Force Interrupt I2: on every index pulse */

    int      hld;
    double   hld_at;
    double   hlt_delay;

    int      fm;
    int      drive;
    int      side;
    int      step_dir;
    int      steps;
    int      test;     /* TEST input; low steps fast, without head settling */
    int      dint;     /* FD1771 DINT input; low inhibits Write Track */

    double   clk;
    double   tsc_per_us;
    int      phase;
    pc_timer_t timer;

    /* Bit engine. */
    int      bstate;
    uint16_t shift;
    int      cells;     /* bit cells into the current byte */
    int      aligned;
    int      syncs;     /* MFM A1 marks in a row */
    int      sync_gap;
    int      bytes;
    int      indexes;
    uint8_t  id[6];
    uint16_t crc;
    int      len;
    int      pos;
    uint8_t  dam;       /* data address mark of the sector being read */

    /* Writing. */
    uint16_t wword;
    int      wcells;
    int      wprev;      /* last data bit written, for MFM clocks */
    int      wseq;       /* step of the Write Sector sequence */
    int      wcount;

    /* Drives that are not being polled, and since when. */
    double   frozen_at[4];
    int      frozen[4];

    void (*irq_cb)(void *priv, int level);
    void (*drq_cb)(void *priv, int level);
    void  *priv;

    /* Board wiring between the chip and the drives. */
    int  (*ready_cb)(void *priv, int drive);
    void (*step_cb)(void *priv, int drive, int dir);
    int  (*settled_cb)(void *priv, int drive);
    void  *board;
};

static void wd_bit(void *ctrl, void *priv, int side);
static void wd_poll_drive(wd179x_t *dev, int drive, int on);

/* ---- Outputs ---------------------------------------------------------------- */

static void
wd_set_intrq(wd179x_t *dev, int level)
{
    if (dev->imm_irq)
        level = 1;

    if (level != dev->intrq) {
        dev->intrq = level;
        if (dev->irq_cb != NULL)
            dev->irq_cb(dev->priv, level);
    }
}

static void
wd_set_drq(wd179x_t *dev, int level)
{
    if (level != dev->drq) {
        dev->drq = level;
        if (dev->drq_cb != NULL)
            dev->drq_cb(dev->priv, level);
    }
}

/* ---- Drive -------------------------------------------------------------------- */

static fdd_drive_t *
wd_drv(wd179x_t *dev)
{
    if ((dev->drive < 0) || (dev->drive > 3))
        return NULL;

    return &drives[dev->drive];
}

/* READY: a disk in the drive (an 8" drive is ready once its door is shut),
   unless the board wires the input otherwise. */
static int
wd_ready(wd179x_t *dev)
{
    fdd_drive_t *drv = wd_drv(dev);

    if (dev->ready_cb != NULL)
        return dev->ready_cb(dev->board, dev->drive);

    return (drv != NULL) && (fdd_get_type(drv) != 0) && !drv->empty;
}

static int
wd_wprt(wd179x_t *dev)
{
    fdd_drive_t *drv = wd_drv(dev);

    return (drv != NULL) && (drv->writeprot || drv->read_only);
}

static int
wd_hlt(wd179x_t *dev)
{
    return dev->hld && (((double) tsc - dev->hld_at) >= dev->hlt_delay);
}

static void
wd_head_load(wd179x_t *dev, int load)
{
    if (load && !dev->hld)
        dev->hld_at = (double) tsc;

    dev->hld = load;
}

static double
wd_cell_tsc(fdd_drive_t *drv)
{
    return (double) d86f_byteperiod(drv) / (double) (1ULL << 32);
}

/* A drive that is not polled keeps turning: move its head over the cells
   that passed while it was not looked at. */
static void
wd_catch_up(wd179x_t *dev, int drive)
{
    fdd_drive_t *drv = &drives[drive];
    double       cell;
    uint32_t     raw;
    uint64_t     cells;

    if (!dev->frozen[drive] || (drv->poll == NULL) || drv->empty) {
        dev->frozen_at[drive] = (double) tsc;
        return;
    }

    cell = wd_cell_tsc(drv);
    raw  = d86f_get_raw_size(drv, fdd_get_head(drv));
    if ((cell > 0.0) && (raw > 0)) {
        cells = (uint64_t) (((double) tsc - dev->frozen_at[drive]) / cell);
        d86f_set_track_pos(drv, (uint32_t) ((d86f_get_track_pos(drv) + cells) % raw));
    }

    dev->frozen_at[drive] = (double) tsc;
}

static void
wd_poll_drive(wd179x_t *dev, int drive, int on)
{
    fdd_drive_t *drv = &drives[drive];

    if (on == !dev->frozen[drive]) {
        /* A board that turned the motor on has started the poll timer of
           a drive kept still here. */
        if (!on)
            timer_disable(&drv->poll_time);
        return;
    }

    if (on) {
        wd_catch_up(dev, drive);
        dev->frozen[drive] = 0;
        if (drv->motoron)
            timer_set_delay_u64(&drv->poll_time, d86f_byteperiod(drv));
    } else {
        timer_disable(&drv->poll_time);
        dev->frozen[drive]    = 1;
        dev->frozen_at[drive] = (double) tsc;
    }
}

/* Poll the selected drive only while the chip needs its bits. */
static void
wd_update_polling(wd179x_t *dev)
{
    int need = (dev->bstate != BS_IDLE) || dev->idx_irq;

    for (int i = 0; i < 4; i++)
        wd_poll_drive(dev, i, need && (i == dev->drive));
}

static int
wd_track0(wd179x_t *dev)
{
    fdd_drive_t *drv = wd_drv(dev);

    return (drv != NULL) && fdd_track0(drv);
}

static int
wd_index(wd179x_t *dev)
{
    fdd_drive_t *drv = wd_drv(dev);

    if ((drv == NULL) || (drv->poll == NULL))
        return 0;

    wd_catch_up(dev, dev->drive);
    return fdd_index(drv);
}

/* The data separator runs at one rate per density: with CLK at 2 MHz, 8"
   FM and MFM both use the 500 kbps cell rate. */
static int
wd_rate_ok(wd179x_t *dev, fdd_drive_t *drv)
{
    int rate = d86f_track_flags(drv) & 7;

    if (d86f_is_mfm(drv) == dev->fm)
        return 0;

    return rate == ((dev->clk >= 2000000.0) ? 0 : 2);
}

/* ---- CRC and coding ------------------------------------------------------------- */

static uint16_t
wd_crc(uint16_t crc, uint8_t val)
{
    crc ^= val << 8;

    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);

    return crc;
}

/* The CRC after A1 A1 A1 from the preset; an MFM mark adds to that. */
#define WD_CRC_A1 0xcdb4

static uint8_t
wd_decode(uint16_t w)
{
    uint8_t b = 0;

    for (int i = 0; i < 8; i++) {
        if (w & (1 << (i * 2)))
            b |= (1 << i);
    }

    return b;
}

static uint16_t
wd_encode(wd179x_t *dev, uint8_t b, int kind)
{
    uint16_t w = 0;

    if (kind == WB_A1) {
        dev->wprev = 1;
        return 0x4489;
    }
    if (kind == WB_C2) {
        dev->wprev = 0;
        return 0x5224;
    }

    if (dev->fm) {
        uint8_t clk = (kind == WB_MARK) ? 0xc7 : ((kind == WB_INDEX) ? 0xd7 : 0xff);

        for (int i = 7; i >= 0; i--)
            w = (w << 2) | (((clk >> i) & 1) << 1) | ((b >> i) & 1);
    } else {
        for (int i = 7; i >= 0; i--) {
            int d = (b >> i) & 1;
            int c = !d && !dev->wprev;

            w          = (w << 2) | (c << 1) | d;
            dev->wprev = d;
        }
    }

    return w;
}

static void
wd_queue(wd179x_t *dev, uint8_t b, int kind)
{
    dev->wword  = wd_encode(dev, b, kind);
    dev->wcells = 16;
}

/* ---- Command completion --------------------------------------------------------- */

static void
wd_finish(wd179x_t *dev)
{
    fdd_drive_t *drv = wd_drv(dev);
    int          wrote = (dev->bstate == BS_WRITE) || (dev->bstate == BS_WRITE_TRACK);

    dev->bstate = BS_IDLE;
    dev->busy   = 0;
    wd_set_drq(dev, 0);
    wd_set_intrq(dev, 1);

    if (wrote && (drv != NULL))
        fdd_do_writeback(drv);

    /* An idle chip unloads the head after 15 index pulses; the FD1771 at
       the third index pulse after the command. */
    timer_disable(&dev->timer);
    dev->phase = PH_NONE;
    if (dev->hld && (drv != NULL) && (drv->poll != NULL) && !drv->empty) {
        uint32_t raw  = d86f_get_raw_size(drv, fdd_get_head(drv));
        double   cell = wd_cell_tsc(drv);
        double   rev  = raw * cell;

        if (rev > 0.0) {
            double wait = 15.0 * rev;

            if (WD_IS_1771(dev)) {
                wd_catch_up(dev, dev->drive);
                wait = ((raw - (d86f_get_track_pos(drv) % raw)) * cell) + (2.0 * rev);
            }
            dev->phase = PH_UNLOAD;
            timer_set_delay_u64(&dev->timer, ((uint64_t) ceil(wait)) << 32);
        }
    }

    wd_update_polling(dev);
}

static void
wd_delay_us(wd179x_t *dev, int phase, double us)
{
    dev->phase = phase;
    timer_set_delay_u64(&dev->timer, ((uint64_t) ceil(us * dev->tsc_per_us)) << 32);
}

/* Times in the data sheet are for a 2 MHz clock and double at 1 MHz. */
static double
wd_ms(wd179x_t *dev, double ms)
{
    return ms * 1000.0 * (2000000.0 / dev->clk);
}

static void
wd_start_bits(wd179x_t *dev, int state)
{
    dev->bstate  = state;
    dev->indexes = 0;
    dev->cells   = 0;
    dev->aligned = 0;
    dev->syncs   = 0;
    dev->bytes   = 0;
    wd_update_polling(dev);
}

/* Head loaded and settled: start what the command does on the disk. */
static void
wd_go(wd179x_t *dev)
{
    int cmd = dev->command >> 4;

    /* The FD1771 samples WPRT, and DINT for Write Track, once the head is
       settled (Type II and III command flow). */
    if (WD_IS_1771(dev) && ((cmd == 0xa) || (cmd == 0xb) || (cmd == 0xf)) &&
        (wd_wprt(dev) || ((cmd == 0xf) && !dev->dint))) {
        dev->status |= ST_WPROT;
        wd_finish(dev);
        return;
    }

    if (cmd == 0xf) {
        /* Write Track asks for its first byte at once. */
        wd_set_drq(dev, 1);
        wd_start_bits(dev, BS_TRACK_INDEX);
    } else if (cmd == 0xe)
        wd_start_bits(dev, BS_TRACK_INDEX);
    else
        /* Type I verify, Read and Write Sector, Read Address. */
        wd_start_bits(dev, BS_FIND_ID);
}

/* Wait for HLT, then go. */
static void
wd_wait_head(wd179x_t *dev)
{
    double left = dev->hlt_delay - ((double) tsc - dev->hld_at);

    if (wd_hlt(dev) || !dev->hld)
        wd_go(dev);
    else {
        dev->phase = PH_HEAD;
        timer_set_delay_u64(&dev->timer, ((uint64_t) ceil(left)) << 32);
    }
}

/* ---- Type I ------------------------------------------------------------------------ */

static void
wd_type1_step(wd179x_t *dev)
{
    static const double rate[4]      = { 3.0, 6.0, 10.0, 15.0 };
    static const double rate_1771[4] = { 6.0, 6.0, 10.0, 20.0 };
    fdd_drive_t        *drv          = wd_drv(dev);
    int                 cmd          = dev->command >> 4;
    double              ms;

    if (cmd == 0x0) {
        /* Restore: step out until TR00, or 255 steps. The FD1771 counts
           its track register down from FFh on the way. */
        if (wd_track0(dev)) {
            dev->track = 0;
            goto done;
        }
        if (dev->steps++ >= 255) {
            dev->status |= ST_SEEK;
            wd_finish(dev);
            return;
        }
        if (WD_IS_1771(dev))
            dev->track--;
        dev->step_dir = -1;
    } else if (cmd == 0x1) {
        /* Seek: the data register holds the destination. */
        if (dev->track == dev->data)
            goto done;
        dev->step_dir = (dev->data > dev->track) ? 1 : -1;
        dev->track += dev->step_dir;
    } else {
        /* Step, Step In, Step Out: one pulse, then done. */
        if (dev->steps++)
            goto done;
        if (cmd >= 0x4)
            dev->step_dir = (cmd >= 0x6) ? -1 : 1;
        if (dev->command & 0x10)
            dev->track += dev->step_dir;
    }

    /* A step pulse; the drive does not go below track 0. */
    if (dev->step_cb != NULL)
        dev->step_cb(dev->board, dev->drive, dev->step_dir);
    else if (drv != NULL)
        fdd_forced_seek(drv, dev->step_dir);

    /* FD1771 with TEST low: about 400 us a step whatever r1 r0 say
       (Table 1). */
    if (WD_IS_1771(dev))
        ms = dev->test ? rate_1771[dev->command & 3] : 0.4;
    else
        ms = rate[dev->command & 3];
    wd_delay_us(dev, PH_STEP, wd_ms(dev, ms));
    return;

done:
    if (dev->command & 0x04) {
        /* Verify: load the head, settle 15 ms, wait for HLT. The FD1771
           waits its 10 ms head load delay instead, none with TEST low. */
        wd_head_load(dev, 1);
        if (!WD_IS_1771(dev))
            wd_delay_us(dev, PH_SETTLE, wd_ms(dev, 15.0));
        else if (dev->test)
            wd_delay_us(dev, PH_SETTLE, wd_ms(dev, 10.0));
        else
            wd_wait_head(dev);
    } else
        wd_finish(dev);
}

static void
wd_timer(void *priv)
{
    wd179x_t *dev = (wd179x_t *) priv;
    int       phase = dev->phase;

    dev->phase = PH_NONE;

    switch (phase) {
        case PH_STEP:
            wd_type1_step(dev);
            break;
        case PH_SETTLE:
        case PH_HEAD:
            wd_wait_head(dev);
            break;
        case PH_UNLOAD:
            if (!dev->busy)
                wd_head_load(dev, 0);
            break;
        default:
            break;
    }
}

/* ---- Bit engine ------------------------------------------------------------------- */

static void
wd_put_cell(wd179x_t *dev, fdd_drive_t *drv, int side)
{
    d86f_put_bit(drv, side, (dev->wword >> 15) & 1);
    dev->wword <<= 1;
    dev->wcells--;
}

/* An address mark in the cells just read: 0 none, else the mark byte, with
   the stream byte aligned on it. */
static int
wd_find_mark(wd179x_t *dev)
{
    if (dev->fm) {
        switch (dev->shift) {
            case 0xf57e: /* FE, ID */
            case 0xf56f: /* FB */
            case 0xf56e: /* FA */
            case 0xf56b: /* F9 */
            case 0xf56a: /* F8 */
                dev->cells = 0;
                return wd_decode(dev->shift);
            default:
                return 0;
        }
    }

    if (dev->shift == 0x4489) {
        dev->syncs   = ((dev->syncs > 0) && (dev->sync_gap == 16)) ? (dev->syncs + 1) : 1;
        dev->sync_gap = 0;
        dev->cells   = 0;
        dev->aligned = 1;
        return 0;
    }

    if (dev->syncs && dev->aligned && (dev->cells == 16)) {
        uint8_t b = wd_decode(dev->shift);

        dev->syncs = 0;
        if ((b == 0xfe) || ((b >= 0xf8) && (b <= 0xfb))) {
            dev->cells = 0;
            return b;
        }
    }

    return 0;
}

static void
wd_got_id(wd179x_t *dev, fdd_drive_t *drv, int side)
{
    int      cmd    = dev->command >> 4;
    uint16_t crc    = dev->fm ? wd_crc(0xffff, 0xfe) : wd_crc(WD_CRC_A1, 0xfe);
    int      crc_ok;

    for (int i = 0; i < 4; i++)
        crc = wd_crc(crc, dev->id[i]);
    crc_ok = (crc == ((dev->id[4] << 8) | dev->id[5]));

    if (cmd == 0xc) {
        /* Read Address: the track address goes to the sector register;
           on the FD1771 the sector address does. */
        dev->sector = WD_IS_1771(dev) ? dev->id[2] : dev->id[0];
        if (!crc_ok)
            dev->status |= ST_CRC;
        wd_finish(dev);
        return;
    }

    if (cmd < 0x8) {
        /* Verify. */
        if (!crc_ok) {
            /* Look at the next ID, still within the five revolutions. */
            dev->status |= ST_CRC;
            dev->bstate = BS_FIND_ID;
        } else {
            if (dev->id[0] != dev->track)
                dev->status |= ST_SEEK;
            else
                dev->status &= ~ST_CRC;
            wd_finish(dev);
        }
        return;
    }

    /* Read and Write Sector: track, sector and, if C is set, side. The
       FD1771 has no side compare. */
    if ((dev->id[0] != dev->track) || (dev->id[2] != dev->sector) ||
        (!WD_IS_1771(dev) && (dev->command & 0x02) &&
         ((dev->id[1] & 1) != ((dev->command >> 3) & 1)))) {
        dev->bstate = BS_FIND_ID;
        return;
    }

    if (!crc_ok) {
        dev->status |= ST_CRC;
        dev->bstate = BS_FIND_ID;
        return;
    }

    dev->status &= ~ST_CRC;
    if (WD_IS_1771(dev) && !(dev->command & 0x08))
        /* b = 0: the length code counts 16-byte groups, 00 meaning 256. */
        dev->len = (dev->id[3] ? dev->id[3] : 256) * 16;
    else
        dev->len = 128 << (dev->id[3] & 3);
    dev->bytes = 0;

    drv->d86f_handler.set_sector(drv, side, dev->id[0], dev->id[1], dev->id[2], dev->id[3]);

    if (cmd < 0xa) {
        dev->bstate = BS_FIND_DAM;
    } else {
        /* Write Sector: the first byte is asked for now; the FD1771 asks
           two bytes into the gap. */
        dev->bstate = BS_WRITE_GAP;
        if (!WD_IS_1771(dev))
            wd_set_drq(dev, 1);
    }
}

static void
wd_data_byte(wd179x_t *dev, uint8_t b)
{
    if (dev->drq)
        dev->status |= ST_LOST;

    dev->data = b;
    wd_set_drq(dev, 1);
}

static void
wd_sector_done(wd179x_t *dev, int crc_ok)
{
    if (!crc_ok) {
        dev->status |= ST_CRC;
        wd_finish(dev);
        return;
    }

    if (dev->command & 0x10) {
        /* Multiple records: on to the next sector. */
        dev->sector++;
        dev->bstate  = BS_FIND_ID;
        dev->indexes = 0;
        return;
    }

    wd_finish(dev);
}

/* Write Sector, byte by byte from the end of the ID field. */
static void
wd_write_next(wd179x_t *dev, fdd_drive_t *drv, int side)
{
    int sync  = dev->fm ? 6 : 12;
    int marks = dev->fm ? 0 : 3;
    int k     = dev->wseq++;

    if (k < sync) {
        wd_queue(dev, 0x00, WB_NORMAL);
    } else if (k < (sync + marks)) {
        wd_queue(dev, 0xa1, WB_A1);
    } else if (k == (sync + marks)) {
        /* a0 picks F8 or FB; the FD1771's a1 a0 pick FB, FA, F9 or F8. */
        uint8_t am = WD_IS_1771(dev) ? (0xfb - (dev->command & 0x03)) :
                                       ((dev->command & 0x01) ? 0xf8 : 0xfb);

        dev->crc = dev->fm ? wd_crc(0xffff, am) : wd_crc(WD_CRC_A1, am);
        wd_queue(dev, am, dev->fm ? WB_MARK : WB_NORMAL);
    } else if (k <= (sync + marks + dev->len)) {
        int     n = k - (sync + marks + 1);
        uint8_t b = dev->data;

        /* A byte the CPU did not supply in time is written as zero. */
        if (dev->drq) {
            dev->status |= ST_LOST;
            b = 0x00;
        }
        if (n < (dev->len - 1))
            wd_set_drq(dev, 1);

        dev->crc = wd_crc(dev->crc, b);
        drv->d86f_handler.write_data(drv, side, n, b);
        wd_queue(dev, b, WB_NORMAL);
    } else if (k == (sync + marks + dev->len + 1)) {
        wd_queue(dev, dev->crc >> 8, WB_NORMAL);
    } else if (k == (sync + marks + dev->len + 2)) {
        wd_queue(dev, dev->crc & 0xff, WB_NORMAL);
    } else if (k == (sync + marks + dev->len + 3)) {
        wd_queue(dev, 0xff, WB_NORMAL);
    } else
        wd_sector_done(dev, 1);
}

/* Write Track: bytes from the CPU, with F5 to FE standing for marks and
   CRCs (control bytes table, p. 14). */
static void
wd_write_track_next(wd179x_t *dev)
{
    uint8_t b = dev->data;

    if (dev->wcount > 0) {
        /* The second CRC byte of an F7. */
        dev->wcount = 0;
        wd_queue(dev, dev->crc & 0xff, WB_NORMAL);
        return;
    }

    if (dev->drq) {
        dev->status |= ST_LOST;
        b = 0x00;
    }
    wd_set_drq(dev, 1);

    if (b == 0xf7) {
        wd_queue(dev, dev->crc >> 8, WB_NORMAL);
        dev->wcount = 1;
        return;
    }

    if (WD_IS_1771(dev) && (b >= 0xf8) && (b <= 0xfe)) {
        /* FD1771: every byte from F8 to FE presets the CRC; FC goes out
           with the D7 clock, the others (the spare FD too) with C7. */
        dev->crc = wd_crc(0xffff, b);
        wd_queue(dev, b, (b == 0xfc) ? WB_INDEX : WB_MARK);
        return;
    }

    if (dev->fm) {
        if ((b >= 0xf8) && (b <= 0xfe) && (b != 0xfc) && (b != 0xfd)) {
            dev->crc = wd_crc(0xffff, b);
            wd_queue(dev, b, WB_MARK);
        } else if (b == 0xfc)
            wd_queue(dev, b, WB_INDEX);
        else {
            dev->crc = wd_crc(dev->crc, b);
            wd_queue(dev, b, WB_NORMAL);
        }
        return;
    }

    if (b == 0xf5) {
        /* An A1 presets the CRC so that it holds A1 A1 A1 afterwards. */
        dev->crc = WD_CRC_A1;
        wd_queue(dev, 0xa1, WB_A1);
    } else if (b == 0xf6)
        wd_queue(dev, 0xc2, WB_C2);
    else {
        dev->crc = wd_crc(dev->crc, b);
        wd_queue(dev, b, WB_NORMAL);
    }
}

static void
wd_bit(void *ctrl, void *priv, int side)
{
    wd179x_t    *dev   = (wd179x_t *) ctrl;
    fdd_drive_t *drv   = (fdd_drive_t *) priv;
    int          index;

    if ((dev->drive < 0) || (drv != &drives[dev->drive]))
        return;

    index = d86f_at_index(drv, side);

    if (index) {
        if (dev->idx_irq)
            wd_set_intrq(dev, 1);
        dev->indexes++;
    }

    /* Writing: one cell out per call. */
    if (dev->wcells > 0) {
        wd_put_cell(dev, drv, side);
        if (dev->wcells > 0)
            return;

        if (dev->bstate == BS_WRITE) {
            wd_write_next(dev, drv, side);
            if (dev->bstate != BS_WRITE)
                dev->wcells = 0;
            return;
        }
        if (dev->bstate == BS_WRITE_TRACK) {
            /* Writing stops with the next index pulse. */
            if (dev->indexes > 1) {
                wd_finish(dev);
                return;
            }
            wd_write_track_next(dev);
            return;
        }
        return;
    }

    d86f_get_bit(drv, side);
    {
        int bit = d86f_last_word(drv, side) & 1;

        /* A head still moving to its track reads no flux changes. */
        if ((dev->settled_cb != NULL) && !dev->settled_cb(dev->board, dev->drive))
            bit = 0;
        dev->shift = (dev->shift << 1) | bit;
    }
    dev->cells++;
    dev->sync_gap++;

    switch (dev->bstate) {
        case BS_FIND_ID:
            /* Five index pulses without the field: Seek error or RNF; six
               for Read Address. The FD1771 gives up at three. */
            if (dev->indexes >= (WD_IS_1771(dev) ? 3 : (((dev->command >> 4) == 0xc) ? 6 : 5))) {
                dev->status |= ((dev->command >> 4) < 0x8) ? ST_SEEK : ST_RNF;
                wd_finish(dev);
                return;
            }
            if (wd_rate_ok(dev, drv) && (wd_find_mark(dev) == 0xfe)) {
                dev->bstate = BS_READ_ID;
                dev->bytes  = 0;
            }
            break;

        case BS_READ_ID:
            if (dev->cells == 16) {
                dev->cells             = 0;
                dev->id[dev->bytes++] = wd_decode(dev->shift);
                if ((dev->command >> 4) == 0xc)
                    wd_data_byte(dev, dev->id[dev->bytes - 1]);
                if (dev->bytes == 6) {
                    dev->bytes = 0;
                    wd_got_id(dev, drv, side);
                }
            }
            break;

        case BS_FIND_DAM: {
            int mark = wd_find_mark(dev);

            if ((mark >= 0xf8) && (mark <= 0xfb)) {
                dev->dam     = mark;
                dev->crc     = dev->fm ? wd_crc(0xffff, mark) : wd_crc(WD_CRC_A1, mark);
                dev->bstate  = BS_READ_DATA;
                dev->bytes   = 0;
                break;
            }
            if (mark == 0xfe) {
                /* The next ID came first: no data field here. */
                dev->status |= ST_RNF;
                wd_finish(dev);
                break;
            }
            if (dev->cells == 16) {
                dev->cells = 0;
                /* The mark must come within 30 bytes of the ID CRC in FM,
                   43 in MFM; 28 on the FD1771. */
                if (++dev->bytes > (WD_IS_1771(dev) ? 28 : (dev->fm ? 30 : 43))) {
                    dev->status |= ST_RNF;
                    wd_finish(dev);
                }
            }
            break;
        }

        case BS_READ_DATA:
            if (dev->cells == 16) {
                uint8_t b = wd_decode(dev->shift);

                dev->cells = 0;
                if (dev->bytes < dev->len) {
                    if (drv->d86f_handler.read_data != NULL)
                        b = drv->d86f_handler.read_data(drv, side, dev->bytes);
                    dev->crc = wd_crc(dev->crc, b);
                    wd_data_byte(dev, b);
                } else
                    dev->id[dev->bytes - dev->len] = b;

                if (++dev->bytes == (dev->len + 2)) {
                    if (WD_IS_1771(dev)) {
                        /* Bits 6 and 5: bits 1 and 0 of the mark,
                           inverted (4FDC manual, p. 19). */
                        dev->status &= ~(ST_WPROT | ST_RECTYPE);
                        dev->status |= ((~dev->dam) & 0x03) << 5;
                    } else if (dev->dam == 0xf8)
                        dev->status |= ST_RECTYPE;
                    else
                        dev->status &= ~ST_RECTYPE;
                    wd_sector_done(dev, dev->crc == ((dev->id[0] << 8) | dev->id[1]));
                }
            }
            break;

        case BS_WRITE_GAP:
            if (dev->cells == 16) {
                dev->cells = 0;
                dev->bytes++;
                if (WD_IS_1771(dev)) {
                    /* FD1771: DRQ two bytes on, the byte must be there
                       eight bytes later. */
                    if (dev->bytes == 2)
                        wd_set_drq(dev, 1);
                    else if ((dev->bytes == 10) && dev->drq) {
                        dev->status |= ST_LOST;
                        wd_finish(dev);
                        break;
                    }
                } else if ((dev->bytes == 2) && dev->drq) {
                    /* Two bytes on, the first data byte must be there. */
                    dev->status |= ST_LOST;
                    wd_finish(dev);
                    break;
                }
                /* Write Gate goes on 11 bytes (FM) or 22 (MFM) after the ID. */
                if (dev->bytes == (dev->fm ? 11 : 22)) {
                    dev->bstate = BS_WRITE;
                    dev->wseq   = 0;
                    dev->wprev  = 0;
                    wd_write_next(dev, drv, side);
                }
            }
            break;

        case BS_TRACK_INDEX:
            if (index) {
                dev->indexes = 1;
                if ((dev->command >> 4) == 0xf) {
                    /* Nothing loaded by the index pulse: Lost Data. */
                    if (dev->drq || !wd_rate_ok(dev, drv)) {
                        if (dev->drq)
                            dev->status |= ST_LOST;
                        wd_finish(dev);
                        break;
                    }
                    dev->bstate = BS_WRITE_TRACK;
                    dev->wprev  = 0;
                    dev->wcount = 0;
                    dev->crc    = 0xffff;
                    wd_write_track_next(dev);
                } else {
                    dev->bstate = BS_READ_TRACK;
                    dev->cells  = 0;
                }
            }
            break;

        case BS_READ_TRACK:
            if (index && (dev->indexes > 1)) {
                wd_finish(dev);
                break;
            }
            /* Bytes realign on each address mark, unless the FD1771's s
               bit says not to. */
            if (!WD_IS_1771(dev) || !(dev->command & 0x01)) {
                if (dev->fm) {
                    if (wd_find_mark(dev) || (dev->shift == 0xf77a))
                        dev->cells = 16;
                } else if (dev->shift == 0x4489 || dev->shift == 0x5224)
                    dev->cells = 16;
            }
            if (dev->cells == 16) {
                dev->cells = 0;
                dev->syncs = 0;
                wd_data_byte(dev, wd_decode(dev->shift));
            }
            break;

        default:
            break;
    }
}

/* ---- Registers ------------------------------------------------------------------------- */

static uint8_t
wd_status(wd179x_t *dev)
{
    uint8_t ret;

    if (dev->type == 1) {
        ret = dev->status & (ST_SEEK | ST_CRC);
        if (wd_wprt(dev))
            ret |= ST_WPROT;
        if (dev->hld && wd_hlt(dev))
            ret |= ST_HEAD;
        if (wd_track0(dev))
            ret |= ST_TRACK0;
        if (wd_index(dev))
            ret |= ST_INDEX;
    } else {
        ret = dev->status & (ST_WPROT | ST_RECTYPE | ST_RNF | ST_CRC | ST_LOST);
        if (dev->drq)
            ret |= ST_DRQ;
    }

    if (dev->busy)
        ret |= ST_BUSY;
    if (!wd_ready(dev))
        ret |= ST_NREADY;

    return ret;
}

static void
wd_command(wd179x_t *dev, uint8_t val)
{
    int cmd = val >> 4;

    if (cmd == 0xd) {
        /* Force Interrupt. */
        if (dev->busy) {
            timer_disable(&dev->timer);
            dev->phase  = PH_NONE;
            dev->bstate = BS_IDLE;
            dev->wcells = 0;
            dev->busy   = 0;
            wd_set_drq(dev, 0);
        } else {
            /* Status now shows the Type I bits, cleared. */
            dev->type   = 1;
            dev->status = 0x00;
        }

        dev->imm_irq = !!(val & 0x08);
        dev->idx_irq = !!(val & 0x04);
        wd_set_intrq(dev, dev->imm_irq);
        wd_update_polling(dev);
        return;
    }

    /* Only Force Interrupt is taken while busy. */
    if (dev->busy)
        return;

    dev->command = val;
    dev->busy    = 1;
    dev->wcells  = 0;
    timer_disable(&dev->timer);
    dev->phase = PH_NONE;
    wd_set_intrq(dev, 0);
    wd_set_drq(dev, 0);

    wd179x_log("WD179X: command %02X track %02X sector %02X data %02X\n",
               val, dev->track, dev->sector, dev->data);

    if (cmd < 0x8) {
        dev->type   = 1;
        dev->status = 0x00;
        dev->steps  = 0;
        if (WD_IS_1771(dev)) {
            /* h alone loads or unloads the head; Restore starts as a seek
               from FFh to 00h. */
            wd_head_load(dev, !!(val & 0x08));
            if (cmd == 0x0) {
                dev->track = 0xff;
                dev->data  = 0x00;
            }
        } else if (val & 0x08)
            /* h loads the head at the start; with neither h nor V it
               unloads. */
            wd_head_load(dev, 1);
        else if (!(val & 0x04))
            wd_head_load(dev, 0);
        wd_type1_step(dev);
        return;
    }

    dev->type   = (cmd < 0xc) ? 2 : 3;
    dev->status = 0x00;

    if (!wd_ready(dev)) {
        wd_finish(dev);
        return;
    }

    if (WD_IS_1771(dev)) {
        /* E loads the head and waits 10 ms (none with TEST low) for HLT;
           without it the head is taken as engaged. */
        if (!(val & 0x04))
            wd_go(dev);
        else {
            wd_head_load(dev, 1);
            if (dev->test)
                wd_delay_us(dev, PH_HEAD, wd_ms(dev, 10.0));
            else
                wd_wait_head(dev);
        }
        return;
    }

    if (((cmd == 0xa) || (cmd == 0xb) || (cmd == 0xf)) && wd_wprt(dev)) {
        dev->status |= ST_WPROT;
        wd_finish(dev);
        return;
    }

    wd_head_load(dev, 1);
    if (val & 0x04)
        wd_delay_us(dev, PH_HEAD, wd_ms(dev, 15.0));
    else
        wd_wait_head(dev);
}

uint8_t
wd179x_read(wd179x_t *dev, int reg)
{
    uint8_t ret;

    switch (reg & 3) {
        case 0:
            ret = wd_status(dev);
            /* Reading status clears INTRQ, except after an immediate
               Force Interrupt. */
            if (!dev->imm_irq)
                wd_set_intrq(dev, 0);
            break;
        case 1:
            ret = dev->track;
            break;
        case 2:
            ret = dev->sector;
            break;
        default:
            ret = dev->data;
            wd_set_drq(dev, 0);
            break;
    }

    return ret;
}

void
wd179x_write(wd179x_t *dev, int reg, uint8_t val)
{
    switch (reg & 3) {
        case 0:
            wd_command(dev, val);
            break;
        case 1:
            dev->track = val;
            break;
        case 2:
            dev->sector = val;
            break;
        default:
            dev->data = val;
            wd_set_drq(dev, 0);
            break;
    }
}

/* ---- Board interface --------------------------------------------------------------------- */

int
wd179x_get_drq(wd179x_t *dev)
{
    return dev->drq;
}

int
wd179x_get_intrq(wd179x_t *dev)
{
    return dev->intrq;
}

void
wd179x_set_callbacks(wd179x_t *dev, void (*intrq)(void *priv, int level),
                     void (*drq)(void *priv, int level), void *priv)
{
    dev->irq_cb = intrq;
    dev->drq_cb = drq;
    dev->priv   = priv;
}

void
wd179x_set_drive(wd179x_t *dev, int drive, int side)
{
    if ((drive >= 0) && (drive <= 3)) {
        fdd_drive_t *drv = &drives[drive];

        if (fdd_get_head(drv) != side) {
            wd_catch_up(dev, drive);
            fdd_set_head(drv, side);
        }
    }

    dev->drive = drive;
    dev->side  = side;
    wd_update_polling(dev);
}

/* The FD1771 has no DDEN input: it only records FM. */
void
wd179x_set_dden(wd179x_t *dev, int fm)
{
    dev->fm = WD_IS_1771(dev) ? 1 : fm;
}

void
wd179x_set_hlt_delay(wd179x_t *dev, double us)
{
    dev->hlt_delay = us * dev->tsc_per_us;
}

void
wd179x_set_clock(wd179x_t *dev, double clk_hz)
{
    dev->clk = clk_hz;
}

void
wd179x_set_test(wd179x_t *dev, int test)
{
    dev->test = test;
}

void
wd179x_set_dint(wd179x_t *dev, int dint)
{
    dev->dint = dint;
}

void
wd179x_set_board(wd179x_t *dev, int (*ready)(void *priv, int drive),
                 void (*step)(void *priv, int drive, int dir),
                 int (*settled)(void *priv, int drive), void *priv)
{
    dev->ready_cb   = ready;
    dev->step_cb    = step;
    dev->settled_cb = settled;
    dev->board      = priv;
}

int
wd179x_get_hld(wd179x_t *dev)
{
    return dev->hld;
}

void
wd179x_reset(wd179x_t *dev)
{
    timer_disable(&dev->timer);
    dev->phase   = PH_NONE;
    dev->bstate  = BS_IDLE;
    dev->wcells  = 0;
    dev->busy    = 0;
    dev->imm_irq = 0;
    dev->idx_irq = 0;
    dev->hld     = 0;
    wd_set_drq(dev, 0);
    wd_set_intrq(dev, 0);

    /* MR loads 03h into the command register and 01h into the sector
       register (the FD1771 data sheet names the command register only);
       a Restore runs when MR goes inactive, Ready or not. */
    if (!WD_IS_1771(dev))
        dev->sector = 0x01;
    dev->type = 1;
    wd_command(dev, 0x03);
}

wd179x_t *
wd179x_create(int chip, double clk_hz)
{
    wd179x_t *dev = (wd179x_t *) calloc(1, sizeof(wd179x_t));

    dev->chip       = chip;
    dev->clk        = clk_hz;
    dev->tsc_per_us = cpuclock / 1000000.0;
    dev->drive      = -1;
    dev->fm         = 1;
    dev->step_dir   = 1;
    dev->type       = 1;
    dev->test       = 1;
    dev->dint       = 1;

    timer_add(&dev->timer, wd_timer, dev, 0);

    /* The 8" drives turn from power on. */
    for (int i = 0; i < 4; i++) {
        drives[i].fdc      = NULL;
        drives[i].ctrl     = dev;
        drives[i].ctrl_bit = wd_bit;
        fdd_set_motor_enable(&drives[i], 1);
        dev->frozen[i] = 0;
        wd_poll_drive(dev, i, 0);
    }

    /* Lets the status bar and the media menu show the drives. */
    fdcinited = 1;

    return dev;
}

void
wd179x_close(wd179x_t *dev)
{
    timer_disable(&dev->timer);

    for (int i = 0; i < 4; i++) {
        timer_disable(&drives[i].poll_time);
        drives[i].ctrl     = NULL;
        drives[i].ctrl_bit = NULL;
        fdd_set_motor_enable(&drives[i], 0);
    }

    fdcinited = 0;

    free(dev);
}
