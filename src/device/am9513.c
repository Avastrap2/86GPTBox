/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the AMD Am9513 System Timing Controller,
 *          after the Am9513 data sheet (AMD, 1980).
 *
 *          The counters are not clocked tick by tick. Each one keeps the
 *          index of the last source edge it has counted, and catches up by
 *          arithmetic whenever it is looked at; timers run only for the
 *          Terminal Counts that something outside the counter observes (an
 *          OUT pin with a load on it, or a counter fed by TCN-1).
 *
 *          SRC and GATE pins that the board leaves open are taken as a
 *          steady high level, so they never supply edges: edge-gated modes
 *          never start, and GATE-selected reloads always take the Hold
 *          register.
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
#include <86box/video.h>
#include <86box/am9513.h>
#include <86box/plat_unused.h>

#ifdef ENABLE_AM9513_LOG
int am9513_do_log = ENABLE_AM9513_LOG;

static void
am9513_log(const char *fmt, ...)
{
    va_list ap;

    if (am9513_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define am9513_log(fmt, ...)
#endif

/* Master Mode register. */
#define MM_TOD       0x0003
#define MM_CMP1      0x0004
#define MM_CMP2      0x0008
#define MM_FOUT_SRC  0x00f0
#define MM_FOUT_DIV  0x0f00
#define MM_FOUT_OFF  0x1000
#define MM_BUS16     0x2000
#define MM_DP_HOLD   0x4000
#define MM_BCD_SCALE 0x8000

/* Counter Mode register. */
#define CM_OUTPUT    0x0007
#define CM_UP        0x0008
#define CM_BCD       0x0010
#define CM_REPEAT    0x0020
#define CM_RELOAD_LH 0x0040
#define CM_SPECIAL   0x0080
#define CM_SOURCE    0x0f00
#define CM_FALLING   0x1000
#define CM_GATING    0xe000

enum {
    SRC_NONE = 0,
    SRC_PERIODIC,
    SRC_TCN1
};

typedef struct am9513_counter_t {
    uint16_t mode;
    uint16_t load;
    uint16_t hold;
    uint16_t alarm;
    uint16_t cnt;

    uint8_t armed;
    uint8_t tc;             /* in the Terminal Count state */
    uint8_t toggle;         /* the TC Toggled flip-flop */
    uint8_t hold_next;      /* alternating modes: the next TC reloads from Hold */
    uint8_t cycle_tcs;      /* TCs counted in the current single cycle */
    uint8_t disarm_pending; /* disarm once the counter has left TC */
    int8_t  pin;            /* last level driven on OUT, -1 = not yet */

    int     src_kind;
    int64_t src_period;     /* source period in half F1 ticks */
    int64_t src_phase;
    int64_t edge;           /* index of the last source edge counted */

    pc_timer_t timer;

    void (*out_cb)(void *priv, int level);
    void  *out_priv;
    void (*clk_cb)(void *priv);
    void  *clk_priv;
} am9513_counter_t;

struct am9513_t {
    uint16_t mm;
    uint8_t  dp;            /* E2 E1 G4 G2 G1, as in the Load Data Pointer command */
    uint8_t  bp;            /* byte pointer: 1 = least significant byte next */
    uint16_t prefetch;
    uint8_t  src_fout;      /* SRC pins wired to FOUT, bit n-1 for SRC n */

    double   osc;           /* X1/X2 frequency in Hz */
    double   tsc_per_htick; /* TSC units per half F1 period */
    uint64_t tsc_base;
    int64_t  htick_base;

    am9513_counter_t c[5];
};

static void am9513_update(am9513_t *dev, int notify_clocks);

/* ---- Time base ---------------------------------------------------------- */

static int64_t
am9513_htick_now(am9513_t *dev)
{
    return dev->htick_base + (int64_t) floor((double) (tsc - dev->tsc_base) / dev->tsc_per_htick);
}

static double
am9513_htick_tsc(am9513_t *dev, int64_t htick)
{
    return (double) dev->tsc_base + ((double) (htick - dev->htick_base) * dev->tsc_per_htick);
}

static void
am9513_set_timebase(am9513_t *dev)
{
    if (dev->tsc_per_htick > 0.0)
        dev->htick_base = am9513_htick_now(dev);
    dev->tsc_base      = tsc;
    dev->tsc_per_htick = cpuclock / (2.0 * dev->osc);
}

/* Period of internal frequency F1-F5 in F1 periods (Figure 16). */
static int64_t
am9513_scaler_period(am9513_t *dev, int f)
{
    static const int64_t binary[5] = { 1, 16, 256, 4096, 65536 };
    static const int64_t bcd[5]    = { 1, 10, 100, 1000, 10000 };

    return (dev->mm & MM_BCD_SCALE) ? bcd[f] : binary[f];
}

/* FOUT period in F1 periods, or 0 when it has no edges. */
static int64_t
am9513_fout_period(am9513_t *dev)
{
    int     src = (dev->mm & MM_FOUT_SRC) >> 4;
    int     div = (dev->mm & MM_FOUT_DIV) >> 8;
    int64_t per;

    if (dev->mm & MM_FOUT_OFF)
        return 0;

    if (src == 0x0)
        per = 1;
    else if (src >= 0xb)
        per = am9513_scaler_period(dev, src - 0xb);
    else
        return 0; /* SRC and GATE pins: no edges, and FOUT may not feed itself */

    return per * (div ? div : 16);
}

static void
am9513_source(am9513_t *dev, am9513_counter_t *c)
{
    int     code = (c->mode & CM_SOURCE) >> 8;
    int64_t per  = 0;

    c->src_kind   = SRC_NONE;
    c->src_period = 0;
    c->src_phase  = 0;

    if (code == 0x0) {
        c->src_kind = SRC_TCN1;
        return;
    }

    if (code >= 0xb)
        per = am9513_scaler_period(dev, code - 0xb);
    else if ((code <= 0x5) && (dev->src_fout & (1 << (code - 1))))
        per = am9513_fout_period(dev);

    if (per == 0)
        return;

    c->src_kind   = SRC_PERIODIC;
    c->src_period = per * 2;
    /* The count edge polarity moves the counted edges by half a period. */
    c->src_phase  = (c->mode & CM_FALLING) ? per : 0;
}

static int64_t
am9513_edge_index(am9513_counter_t *c, int64_t htick)
{
    int64_t d = htick - c->src_phase;

    if (d < 0)
        return -1 - ((-d - 1) / c->src_period);

    return d / c->src_period;
}

static int64_t
am9513_edge_htick(am9513_counter_t *c, int64_t edge)
{
    return c->src_phase + (edge * c->src_period);
}

/* Stop counting edges that passed under the old configuration. */
static void
am9513_anchor(am9513_t *dev, am9513_counter_t *c)
{
    am9513_source(dev, c);
    if (c->src_kind == SRC_PERIODIC)
        c->edge = am9513_edge_index(c, am9513_htick_now(dev));
}

/* ---- Count sequences ---------------------------------------------------- */

/* The modulus and digit layout of a counter. Counters 1 and 2 turn over as a
   24-hour clock when the Master Mode enables time-of-day (Figure 14). */
static int
am9513_tod_div(am9513_t *dev, int n)
{
    static const int div[4] = { 0, 5, 6, 10 };

    if ((n > 1) || !(dev->mm & MM_TOD) || !(dev->c[n].mode & CM_BCD))
        return 0;

    return div[dev->mm & MM_TOD];
}

static int64_t
am9513_modulus(am9513_t *dev, int n)
{
    int tod = am9513_tod_div(dev, n);

    if (tod)
        return (n == 0) ? (600 * tod) : 1440;

    return (dev->c[n].mode & CM_BCD) ? 10000 : 65536;
}

static int64_t
am9513_ordinal(am9513_t *dev, int n, uint16_t v)
{
    int     tod = am9513_tod_div(dev, n);
    int     d0  = v & 0xf;
    int     d1  = (v >> 4) & 0xf;
    int     d2  = (v >> 8) & 0xf;
    int     d3  = (v >> 12) & 0xf;
    int64_t ord;

    if (tod && (n == 0))
        ord = ((((d3 * 10) + d2) * 10 + d1) * tod) + d0; /* SS.T plus the input scaler */
    else if (tod)
        ord = ((d3 * 10 + d2) * 60) + (d1 * 10) + d0;     /* HH:MM */
    else if (dev->c[n].mode & CM_BCD)
        ord = (d3 * 1000) + (d2 * 100) + (d1 * 10) + d0;
    else
        ord = v;

    /* Digits outside their range have no defined sequence; keep them on it. */
    return ord % am9513_modulus(dev, n);
}

static uint16_t
am9513_value(am9513_t *dev, int n, int64_t ord)
{
    int tod = am9513_tod_div(dev, n);
    int sec;
    int min;

    if (tod && (n == 0)) {
        sec = (int) (ord / (10 * tod));
        return (uint16_t) (((sec / 10) << 12) | ((sec % 10) << 8) |
                           ((int) ((ord / tod) % 10) << 4) | (int) (ord % tod));
    } else if (tod) {
        min = (int) (ord % 60);
        return (uint16_t) ((((int) (ord / 60) / 10) << 12) | (((int) (ord / 60) % 10) << 8) |
                           ((min / 10) << 4) | (min % 10));
    } else if (dev->c[n].mode & CM_BCD)
        return (uint16_t) ((((ord / 1000) % 10) << 12) | (((ord / 100) % 10) << 8) |
                           (((ord / 10) % 10) << 4) | (ord % 10));

    return (uint16_t) ord;
}

/* Source edges from value v (outside TC) until the counter reaches TC: TC is
   the count that would have been zero (0001 down, 9999 or FFFF up). */
static int64_t
am9513_edges_to_tc(am9513_t *dev, int n, uint16_t v)
{
    int64_t m   = am9513_modulus(dev, n);
    int64_t ord = am9513_ordinal(dev, n, v);

    if (dev->c[n].mode & CM_UP)
        return m - ord;

    return ord ? ord : m;
}

static uint16_t
am9513_advance(am9513_t *dev, int n, uint16_t v, int64_t k)
{
    int64_t m   = am9513_modulus(dev, n);
    int64_t ord = am9513_ordinal(dev, n, v);

    if (dev->c[n].mode & CM_UP)
        ord = (ord + k) % m;
    else
        ord = ((ord - k) % m + m) % m;

    return am9513_value(dev, n, ord);
}

/* ---- Counter control ---------------------------------------------------- */

static int
am9513_gating(am9513_counter_t *c)
{
    return (c->mode & CM_GATING) >> 13;
}

/* The reload source for the coming TC. */
static uint16_t
am9513_reload_value(am9513_counter_t *c)
{
    /* With no gating, the special gate bit lets GATE N pick the register
       (modes S and V); an open GATE pin reads high, selecting Hold. */
    if ((c->mode & CM_SPECIAL) && !am9513_gating(c) && (c->mode & CM_RELOAD_LH))
        return c->hold;

    if ((c->mode & CM_RELOAD_LH) && !(c->mode & CM_SPECIAL) && c->hold_next)
        return c->hold;

    return c->load;
}

static int
am9513_counting(am9513_t *dev, int n)
{
    am9513_counter_t *c = &dev->c[n];

    if (!c->armed)
        return 0;

    switch (am9513_gating(c)) {
        case 0:
            return 1;
        case 1:
            /* Active high level TCN-1. */
            return dev->c[(n + 4) % 5].tc;
        case 2:
        case 3:
        case 4:
            return 1; /* active high level on an open GATE pin */
        default:
            /* Active low level, or an edge that an open pin never makes. */
            return 0;
    }
}

/* Whether something outside the counter watches its TCs one by one. */
static int
am9513_observed(am9513_t *dev, int n)
{
    am9513_counter_t *next = &dev->c[(n + 1) % 5];

    if (dev->c[n].out_cb != NULL)
        return 1;

    if ((next->src_kind == SRC_TCN1) || (am9513_gating(next) == 1))
        return 1;

    /* Counter 1's comparator conditions OUT2 in time-of-day mode. */
    if ((n == 0) && (dev->c[1].out_cb != NULL) && (dev->mm & MM_TOD) &&
        ((dev->mm & (MM_CMP1 | MM_CMP2)) == (MM_CMP1 | MM_CMP2)))
        return 1;

    return 0;
}

static int
am9513_compare(am9513_t *dev, int n)
{
    int eq = (dev->c[n].cnt == dev->c[n].alarm);

    /* With time-of-day and both comparators on, OUT2 needs all 32 bits. */
    if ((n == 1) && (dev->mm & MM_TOD) && (dev->mm & MM_CMP1))
        eq = eq && (dev->c[0].cnt == dev->c[0].alarm);

    return eq;
}

/* The OUT signal as the status register shows it (status = 1), or as the
   pin drives the board (status = 0; a high-impedance pin drives nothing). */
static int
am9513_out(am9513_t *dev, int n, int status)
{
    am9513_counter_t *c  = &dev->c[n];
    int               oc = c->mode & CM_OUTPUT;

    if (((n == 0) && (dev->mm & MM_CMP1)) || ((n == 1) && (dev->mm & MM_CMP2))) {
        int cmp = am9513_compare(dev, n);

        switch (oc) {
            case 0:
                return status ? !cmp : 0;
            case 4:
                return status ? cmp : 0;
            case 5:
                return !cmp;
            default:
                return cmp;
        }
    }

    switch (oc) {
        case 0:
            return status ? c->tc : 0;
        case 1:
            return c->tc;
        case 2:
        case 3:
            /* 3 is marked illegal, yet SCP's 8086 Monitor programs it for
               its baud rate clock ("output toggle"), which works. */
            return c->toggle;
        case 4:
            return status ? !c->tc : 0;
        case 5:
            return !c->tc;
        default:
            return 0;
    }
}

static void
am9513_pin_update(am9513_t *dev, int n)
{
    am9513_counter_t *c     = &dev->c[n];
    int               level = am9513_out(dev, n, 0);

    if ((c->out_cb != NULL) && (c->pin != level)) {
        c->pin = (int8_t) level;
        c->out_cb(c->out_priv, level);
    }
}

static void am9513_run(am9513_t *dev, int n, int64_t edge);
static void am9513_tc_edge(am9513_t *dev, int n, int rising, int64_t htick);

/* A counter gated by this one's TC level counts up to the change first. */
static void
am9513_gate_sync(am9513_t *dev, int n, int64_t htick)
{
    int               m    = (n + 1) % 5;
    am9513_counter_t *next = &dev->c[m];

    if ((am9513_gating(next) == 1) && (next->src_kind == SRC_PERIODIC))
        am9513_run(dev, m, am9513_edge_index(next, htick));
}

static void
am9513_enter_tc(am9513_t *dev, int n, int64_t htick)
{
    am9513_counter_t *c = &dev->c[n];
    int               cycle;

    am9513_gate_sync(dev, n, htick);

    c->cnt = am9513_reload_value(c);
    c->tc  = 1;

    if ((c->mode & CM_RELOAD_LH) && !(c->mode & CM_SPECIAL))
        c->hold_next ^= 1;

    /* A single cycle ends after one TC, or two in the modes that reload
       from Load and Hold (G-I and S). */
    cycle = (c->mode & CM_RELOAD_LH) ? 2 : 1;
    if (!(c->mode & CM_REPEAT) && (++c->cycle_tcs >= cycle)) {
        c->cycle_tcs      = 0;
        c->disarm_pending = 1;
    }

    am9513_pin_update(dev, n);
    if ((n == 0) && (dev->mm & MM_CMP2))
        am9513_pin_update(dev, 1);
    am9513_tc_edge(dev, n, 1, htick);
}

/* The source edge after TC always counts, armed or not (data sheet, TC). */
static void
am9513_leave_tc(am9513_t *dev, int n, int64_t htick)
{
    am9513_counter_t *c = &dev->c[n];

    am9513_gate_sync(dev, n, htick);

    c->toggle ^= 1;

    if (am9513_edges_to_tc(dev, n, c->cnt) == 1) {
        /* Reloaded with 0001 (or the last count up): TC stays active. */
        c->tc = 0;
        am9513_enter_tc(dev, n, htick);
        return;
    }

    c->cnt = am9513_advance(dev, n, c->cnt, 1);
    c->tc  = 0;

    if (c->disarm_pending) {
        c->disarm_pending = 0;
        c->armed          = 0;
    }

    am9513_pin_update(dev, n);
    if ((n == 0) && (dev->mm & MM_CMP2))
        am9513_pin_update(dev, 1);
    am9513_tc_edge(dev, n, 0, htick);
}

/* One count pulse: a source edge, or STEP and LOAD's internal pulse (force). */
static void
am9513_pulse(am9513_t *dev, int n, int force, int64_t htick)
{
    am9513_counter_t *c = &dev->c[n];

    if (c->tc) {
        am9513_leave_tc(dev, n, htick);
        return;
    }

    if (!force && !am9513_counting(dev, n))
        return;

    if (am9513_edges_to_tc(dev, n, c->cnt) == 1)
        am9513_enter_tc(dev, n, htick);
    else {
        c->cnt = am9513_advance(dev, n, c->cnt, 1);
        if (n < 2)
            am9513_pin_update(dev, n);
        if ((n == 0) && (dev->mm & MM_CMP2))
            am9513_pin_update(dev, 1);
    }
}

/* TCN-1 as the next counter's count source. */
static void
am9513_tc_edge(am9513_t *dev, int n, int rising, int64_t htick)
{
    int               m    = (n + 1) % 5;
    am9513_counter_t *next = &dev->c[m];

    if (next->src_kind == SRC_TCN1) {
        if (rising != !!(next->mode & CM_FALLING))
            am9513_pulse(dev, m, 0, htick);
    }
}

/* Count the source edges up to and including edge index 'edge'. */
static void
am9513_run(am9513_t *dev, int n, int64_t edge)
{
    am9513_counter_t *c = &dev->c[n];
    int64_t           d;
    int64_t           cyc;

    if (c->src_kind != SRC_PERIODIC)
        return;

    while (c->edge < edge) {
        if (c->tc) {
            c->edge++;
            am9513_leave_tc(dev, n, am9513_edge_htick(c, c->edge));
            continue;
        }

        if (!am9513_counting(dev, n)) {
            c->edge = edge;
            break;
        }

        d = am9513_edges_to_tc(dev, n, c->cnt);
        if ((c->edge + d) > edge) {
            c->cnt  = am9513_advance(dev, n, c->cnt, edge - c->edge);
            c->edge = edge;
            break;
        }

        c->cnt = am9513_advance(dev, n, c->cnt, d - 1);
        c->edge += d;
        am9513_enter_tc(dev, n, am9513_edge_htick(c, c->edge));

        /* Nobody watches the TCs one by one: jump whole periods, after which
           count, toggle and reload alternation are all back where they were. */
        if ((c->mode & CM_REPEAT) && !c->disarm_pending && !am9513_observed(dev, n) &&
            (am9513_gating(c) != 1)) {
            if ((c->mode & CM_RELOAD_LH) && !(c->mode & CM_SPECIAL))
                cyc = am9513_edges_to_tc(dev, n, c->load) + am9513_edges_to_tc(dev, n, c->hold);
            else
                cyc = 2 * am9513_edges_to_tc(dev, n, am9513_reload_value(c));

            if ((edge - c->edge) > cyc)
                c->edge += ((edge - c->edge - 1) / cyc) * cyc;
        }
    }
}

static void
am9513_sync(am9513_t *dev)
{
    int64_t now = am9513_htick_now(dev);

    /* Counters gated by TCN-1 go last, so that the counter before them has
       already brought them up to each change of their gate. */
    for (int pass = 0; pass < 2; pass++) {
        for (int n = 0; n < 5; n++) {
            if ((dev->c[n].src_kind == SRC_PERIODIC) && ((am9513_gating(&dev->c[n]) == 1) == pass))
                am9513_run(dev, n, am9513_edge_index(&dev->c[n], now));
        }
    }
}

/* ---- Event scheduling ---------------------------------------------------- */

/* Source edges from now until the comparator output changes, or -1. */
static int64_t
am9513_compare_edges(am9513_t *dev, int n)
{
    am9513_counter_t *c = &dev->c[n];
    int64_t           d;
    int64_t           k;

    if (am9513_compare(dev, n))
        return 1; /* the next count ends the match */

    if (c->tc)
        return 1;

    /* Within the run to the next TC the count moves monotonically. */
    d = am9513_edges_to_tc(dev, n, c->cnt);
    k = am9513_ordinal(dev, n, c->alarm) - am9513_ordinal(dev, n, c->cnt);
    if (!(c->mode & CM_UP))
        k = -k;
    if ((k > 0) && (k < d))
        return k;

    return d; /* look again from the TC */
}

static void
am9513_schedule(am9513_t *dev, int n)
{
    am9513_counter_t *c = &dev->c[n];
    int64_t           d;
    int64_t           k;
    double            when;

    timer_disable(&c->timer);

    if ((c->src_kind != SRC_PERIODIC) || !am9513_observed(dev, n))
        return;

    if (c->tc)
        d = 1;
    else if (am9513_counting(dev, n))
        d = am9513_edges_to_tc(dev, n, c->cnt);
    else
        return;

    if ((n < 2) && (dev->mm & ((n == 0) ? MM_CMP1 : MM_CMP2)) && am9513_counting(dev, n)) {
        k = am9513_compare_edges(dev, n);
        if ((k > 0) && (k < d))
            d = k;
    }

    /* Timers fire on whole TSC ticks: round up, so that the edge has been
       reached when the callback counts up to now. */
    when = ceil(am9513_htick_tsc(dev, am9513_edge_htick(c, c->edge + d))) - (double) tsc;
    if (when < 0.0)
        when = 0.0;
    timer_set_delay_u64(&c->timer, ((uint64_t) when) << 32);
}

static void
am9513_timer(void *priv)
{
    am9513_t *dev = (am9513_t *) priv;

    am9513_update(dev, 0);
}

static void
am9513_update(am9513_t *dev, int notify_clocks)
{
    uint8_t armed[5];

    for (int n = 0; n < 5; n++)
        armed[n] = dev->c[n].armed;

    am9513_sync(dev);

    for (int n = 0; n < 5; n++) {
        am9513_pin_update(dev, n);
        am9513_schedule(dev, n);
    }

    for (int n = 0; n < 5; n++) {
        if ((notify_clocks || (armed[n] != dev->c[n].armed)) && (dev->c[n].clk_cb != NULL))
            dev->c[n].clk_cb(dev->c[n].clk_priv);
    }
}

/* ---- Register access ------------------------------------------------------ */

static uint8_t
am9513_status(am9513_t *dev)
{
    uint8_t ret = 0xc0 | (dev->bp ? 0x01 : 0x00);

    for (int n = 0; n < 5; n++) {
        if (am9513_out(dev, n, 1))
            ret |= (2 << n);
    }

    return ret;
}

static uint16_t *
am9513_reg(am9513_t *dev, int *is_status)
{
    int e = (dev->dp >> 3) & 3;
    int g = dev->dp & 7;

    *is_status = 0;

    if ((g >= 1) && (g <= 5)) {
        am9513_counter_t *c = &dev->c[g - 1];

        switch (e) {
            case 0:
                return &c->mode;
            case 1:
                return &c->load;
            default:
                return &c->hold;
        }
    }

    if (g == 7) {
        switch (e) {
            case 0:
                return &dev->c[0].alarm;
            case 1:
                return &dev->c[1].alarm;
            case 2:
                return &dev->mm;
            default:
                *is_status = 1;
                break;
        }
    }

    return NULL;
}

/* "The register addressed by the Data Pointer is read at the time the Data
   Pointer is set ... and whenever the data port is read or written" (SCP-300F
   manual p. 27): later changes are not seen until the next prefetch. */
static void
am9513_prefetch(am9513_t *dev)
{
    int       is_status;
    uint16_t *reg = am9513_reg(dev, &is_status);

    if (is_status)
        dev->prefetch = am9513_status(dev) * 0x0101;
    else if (reg != NULL)
        dev->prefetch = *reg;
}

/* Figure 10: the element, hold and control cycles. */
static void
am9513_sequence(am9513_t *dev)
{
    int e = (dev->dp >> 3) & 3;
    int g = dev->dp & 7;

    if (dev->mm & MM_DP_HOLD)
        return;

    if ((g >= 1) && (g <= 5)) {
        if (e == 3)
            g = (g % 5) + 1;
        else if (++e == 3) {
            e = 0;
            g = (g % 5) + 1;
        }
    } else if ((g == 7) && (e != 3))
        e = (e + 1) % 3;

    dev->dp = (uint8_t) ((e << 3) | g);
}

static void
am9513_data_done(am9513_t *dev)
{
    if (dev->mm & MM_BUS16)
        dev->bp = 1;
    else
        dev->bp ^= 1;

    if (dev->bp)
        am9513_sequence(dev);

    am9513_prefetch(dev);
}

static void
am9513_write_data(am9513_t *dev, uint8_t val)
{
    int       e = (dev->dp >> 3) & 3;
    int       g = dev->dp & 7;
    int       is_status;
    uint16_t *reg = am9513_reg(dev, &is_status);

    if (reg != NULL) {
        /* Count under the old setting up to now first. */
        am9513_sync(dev);

        if (dev->bp)
            *reg = (*reg & 0xff00) | val;
        else
            *reg = (*reg & 0x00ff) | (val << 8);

        if ((g == 7) && (e == 2)) {
            for (int n = 0; n < 5; n++)
                am9513_anchor(dev, &dev->c[n]);
        } else if ((g >= 1) && (g <= 5) && (e == 0))
            am9513_anchor(dev, &dev->c[g - 1]);
    }

    am9513_data_done(dev);
    am9513_update(dev, 1);
}

static uint8_t
am9513_read_data(am9513_t *dev)
{
    uint8_t ret;
    int     e = (dev->dp >> 3) & 3;
    int     g = dev->dp & 7;

    if ((g == 7) && (e == 3)) {
        am9513_sync(dev);
        ret = am9513_status(dev);
    } else
        ret = dev->bp ? (dev->prefetch & 0xff) : (dev->prefetch >> 8);

    am9513_data_done(dev);

    return ret;
}

/* LOAD: from the register the coming TC would use; a counter one count away
   from TC goes to TC instead, and a counter in TC leaves it (Load Counters). */
static void
am9513_load(am9513_t *dev, int n, int64_t htick)
{
    am9513_counter_t *c = &dev->c[n];
    /* Disarmed counters take Load, except where GATE picks (modes S, V). */
    uint16_t          val = (c->armed || ((c->mode & CM_SPECIAL) && !am9513_gating(c))) ?
                                am9513_reload_value(c) : c->load;

    if (c->tc) {
        am9513_gate_sync(dev, n, htick);
        c->tc = 0;
        c->toggle ^= 1;
        c->cnt = val;
        if (c->disarm_pending) {
            c->disarm_pending = 0;
            c->armed          = 0;
        }
        am9513_tc_edge(dev, n, 0, htick);
    } else if (am9513_edges_to_tc(dev, n, c->cnt) == 1)
        am9513_enter_tc(dev, n, htick);
    else
        c->cnt = val;
}

static void
am9513_reset(am9513_t *dev)
{
    am9513_sync(dev);

    /* Master reset: the counters and the Data Pointer are left alone. */
    dev->mm = 0x0000;
    for (int n = 0; n < 5; n++) {
        am9513_counter_t *c = &dev->c[n];

        c->armed          = 0;
        c->disarm_pending = 0;
        c->mode           = 0x0b00;
        c->load           = 0x0000;
        c->hold           = 0x0000;
        am9513_anchor(dev, c);
    }
}

static void
am9513_command(am9513_t *dev, uint8_t val)
{
    int64_t now = am9513_htick_now(dev);
    int     n   = (val & 7) - 1;
    uint8_t sel = val & 0x1f;

    am9513_sync(dev);

    switch (val >> 5) {
        case 0x0:
            /* Load Data Pointer; groups 000 and 110 are not transferred. */
            if (((val & 7) != 0) && ((val & 7) != 6)) {
                dev->dp = val & 0x1f;
                dev->bp = 1;
                am9513_prefetch(dev);
            }
            break;

        case 0x1:
            /* Arm. */
            for (int i = 0; i < 5; i++) {
                if (sel & (1 << i)) {
                    dev->c[i].armed          = 1;
                    dev->c[i].hold_next      = 1;
                    dev->c[i].cycle_tcs      = 0;
                    dev->c[i].disarm_pending = 0;
                    am9513_anchor(dev, &dev->c[i]);
                }
            }
            break;

        case 0x2:
            /* Load. */
            for (int i = 0; i < 5; i++) {
                if (sel & (1 << i))
                    am9513_load(dev, i, now);
            }
            break;

        case 0x3:
            /* Load and Arm. */
            for (int i = 0; i < 5; i++) {
                if (sel & (1 << i)) {
                    am9513_load(dev, i, now);
                    dev->c[i].armed          = 1;
                    dev->c[i].hold_next      = 1;
                    dev->c[i].cycle_tcs      = 0;
                    dev->c[i].disarm_pending = 0;
                    am9513_anchor(dev, &dev->c[i]);
                }
            }
            break;

        case 0x4:
        case 0x5:
        case 0x6:
            /* Disarm and Save, Save, Disarm. */
            for (int i = 0; i < 5; i++) {
                if (!(sel & (1 << i)))
                    continue;
                if ((val >> 5) != 0x6)
                    dev->c[i].hold = dev->c[i].cnt;
                if ((val >> 5) != 0x5) {
                    /* A counter in TC leaves it before disarming. */
                    if (dev->c[i].tc)
                        dev->c[i].disarm_pending = 1;
                    else
                        dev->c[i].armed = 0;
                }
            }
            break;

        default:
            if (val == 0xff)
                am9513_reset(dev);
            else if (val == 0xe8)
                dev->mm |= MM_DP_HOLD;
            else if (val == 0xe0)
                dev->mm &= ~MM_DP_HOLD;
            else if (val == 0xee)
                dev->mm |= MM_FOUT_OFF;
            else if (val == 0xe6)
                dev->mm &= ~MM_FOUT_OFF;
            else if (val == 0xef)
                dev->mm |= MM_BUS16;
            else if (val == 0xe7)
                dev->mm &= ~MM_BUS16;
            else if ((n >= 0) && (n < 5)) {
                switch (val & 0xf8) {
                    case 0xe0:
                        dev->c[n].toggle = 0; /* Clear Output */
                        break;
                    case 0xe8:
                        dev->c[n].toggle = 1; /* Set Output */
                        break;
                    case 0xf0:
                        am9513_pulse(dev, n, 1, now); /* Step */
                        break;
                    default:
                        break;
                }
            }

            /* FOUT and the scaler may have moved. */
            for (int i = 0; i < 5; i++)
                am9513_anchor(dev, &dev->c[i]);
            break;
    }

    am9513_update(dev, 1);
}

uint8_t
am9513_read(uint16_t addr, void *priv)
{
    am9513_t *dev = (am9513_t *) priv;
    uint8_t   ret;

    if (addr & 1) {
        am9513_sync(dev);
        ret = am9513_status(dev);
    } else
        ret = am9513_read_data(dev);

    am9513_log("AM9513: read %c = %02X\n", (addr & 1) ? 'C' : 'D', ret);

    return ret;
}

void
am9513_write(uint16_t addr, uint8_t val, void *priv)
{
    am9513_t *dev = (am9513_t *) priv;

    am9513_log("AM9513: write %c = %02X\n", (addr & 1) ? 'C' : 'D', val);

    if (addr & 1)
        am9513_command(dev, val);
    else
        am9513_write_data(dev, val);
}

/* ---- Board interface ----------------------------------------------------- */

void
am9513_set_out_callback(am9513_t *dev, int n, void (*callback)(void *priv, int level), void *priv)
{
    if ((n < 1) || (n > 5))
        return;

    dev->c[n - 1].out_cb   = callback;
    dev->c[n - 1].out_priv = priv;
    dev->c[n - 1].pin      = -1;
    am9513_update(dev, 0);
}

void
am9513_set_clock_callback(am9513_t *dev, int n, void (*callback)(void *priv), void *priv)
{
    if ((n < 1) || (n > 5))
        return;

    dev->c[n - 1].clk_cb   = callback;
    dev->c[n - 1].clk_priv = priv;
}

int
am9513_get_out_clock(am9513_t *dev, int n, double *period, double *rise)
{
    am9513_counter_t *c;
    int               oc;
    int64_t           first;
    int64_t           second;
    int64_t           cyc;

    if ((n < 1) || (n > 5))
        return 0;

    am9513_sync(dev);

    c  = &dev->c[n - 1];
    oc = c->mode & CM_OUTPUT;
    n--;

    if ((c->src_kind != SRC_PERIODIC) || !(c->mode & CM_REPEAT) || c->disarm_pending ||
        !am9513_counting(dev, n) || (am9513_gating(c) == 1) || ((oc != 2) && (oc != 3)) ||
        ((n == 0) && (dev->mm & MM_CMP1)) || ((n == 1) && (dev->mm & MM_CMP2)))
        return 0;

    /* A toggled output changes as each TC ends; a full cycle takes two TCs. */
    if ((c->mode & CM_RELOAD_LH) && !(c->mode & CM_SPECIAL))
        cyc = am9513_edges_to_tc(dev, n, c->load) + am9513_edges_to_tc(dev, n, c->hold);
    else
        cyc = 2 * am9513_edges_to_tc(dev, n, am9513_reload_value(c));

    if (c->tc)
        first = c->edge + 1;
    else
        first = c->edge + am9513_edges_to_tc(dev, n, c->cnt) + 1;

    if (!c->toggle)
        second = first;
    else if (c->tc)
        second = first + am9513_edges_to_tc(dev, n, c->cnt);
    else
        second = first + am9513_edges_to_tc(dev, n, am9513_reload_value(c));

    *period = (double) cyc * (double) c->src_period * dev->tsc_per_htick;
    *rise   = am9513_htick_tsc(dev, am9513_edge_htick(c, second));

    return 1;
}

static void *
am9513_init(const device_t *info)
{
    am9513_t *dev = (am9513_t *) calloc(1, sizeof(am9513_t));

    dev->osc      = info->local ? (double) info->local : 4000000.0;
    /* SCP-300F: FOUT is wired to SRC1 (the board's FOUT source, p. 30). */
    dev->src_fout = 0x01;
    am9513_set_timebase(dev);

    for (int n = 0; n < 5; n++) {
        dev->c[n].pin = -1;
        timer_add(&dev->c[n].timer, am9513_timer, dev, 0);
    }

    /* Power-on reset; the counters themselves come up undefined. */
    am9513_reset(dev);
    dev->dp = 0x1f; /* status register: a legal pointer */
    dev->bp = 1;
    am9513_prefetch(dev);

    return dev;
}

static void
am9513_close(void *priv)
{
    am9513_t *dev = (am9513_t *) priv;

    for (int n = 0; n < 5; n++)
        timer_disable(&dev->c[n].timer);

    free(dev);
}

static void
am9513_speed_changed(void *priv)
{
    am9513_t *dev = (am9513_t *) priv;

    am9513_sync(dev);
    am9513_set_timebase(dev);
    for (int n = 0; n < 5; n++)
        am9513_anchor(dev, &dev->c[n]);
    am9513_update(dev, 1);
}

const device_t am9513_device = {
    .name          = "AMD Am9513 System Timing Controller",
    .internal_name = "am9513",
    .flags         = 0,
    .local         = 4000000,
    .init          = am9513_init,
    .close         = am9513_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = am9513_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};
