/*
 * 86GPTBox - guest-time model of the ATI 3D Rage II+ (Mach64 GT-B) drawing
 * engine.
 *
 * The Mach64 core draws an operation as soon as its trigger register is
 * written, so a fill or a blit takes no emulated time at all.  Software that
 * measures the engine, or keeps it busy back to back (3D WinBench 98's
 * Z-buffer clear), then sees an impossibly fast chip and does far more work
 * per emulated second than the real card could; 3D WinBench reported 7600
 * Mpixels/s and took six real minutes for nine emulated seconds.
 *
 * This file holds the pure parts of a second, guest-time view of the
 * 48-entry command FIFO: an entry leaves the FIFO when the engine is free,
 * and a drawing operation keeps the engine busy for as long as its memory
 * traffic takes on the real chip.  The pixels are still drawn immediately;
 * only time is modelled.  vid_ati_mach64_rage2p.c connects it to the CPU.
 *
 * Memory costs follow ATI's own method (RAGE PRO and Derivatives
 * Programmer's Guide 7.9.7 and table 7-1, RAGE II+ column): 64-bit memory
 * with 2 MB or more, one cycle per access to an open page and seven for a
 * page change, pages of 512 QWORDs, write-only operations one access per
 * QWORD, read-modify-write two, screen-to-screen blits reading and writing
 * with a page change every source-FIFO load (32x32 bits = 16 QWORDs), and
 * the display refresh taking its share of the bandwidth.  ATI publishes no
 * 3D figures for this chip; 3D work uses the same memory rule plus one pixel
 * per engine clock.
 */
#include <stdint.h>
#include "vid_ati_mach64_gtb_timing.h"

#define RING_MASK (MACH64_GTB_FIFO_RING - 1)

/* RRG-G02700 B-2: PLL reference crystal. */
#define PLL_REF_HZ 14318180.0

/* ATI table 7-1, RAGE II+: page hit 1 cycle, page miss 7, 512 QWORD pages,
   32x32 source FIFO. */
#define PAGE_SHIFT       12
#define PAGE_MISS_EXTRA  6.0
#define SRC_FIFO_QWORDS  16.0

/* Setup an operation needs before its first pixel (engine clocks).  ATI
   notes that this overhead dominates small operations but gives no figure. */
#define OP_SETUP_CLOCKS   16.0
#define ROW_CLOCKS_2D     2.0
#define SETUP_CLOCKS_3D   20.0
#define ROW_CLOCKS_3D     2.0

void
mach64_gtb_fifo_fold(mach64_gtb_fifo_model_t *f, mach64_gtb_cost_fn cost_of, void *priv)
{
    while (f->fold != f->tail) {
        mach64_gtb_fifo_entry_t *e = &f->ring[f->fold & RING_MASK];
        uint64_t                 cost;

        if (!cost_of(priv, e, &cost))
            break;
        e->start   = (e->arrival > f->free_at) ? e->arrival : f->free_at;
        f->free_at = e->start + cost;
        f->fold++;
    }
}

void
mach64_gtb_fifo_depart(mach64_gtb_fifo_model_t *f, uint64_t now)
{
    while ((f->head != f->fold) && (f->ring[f->head & RING_MASK].start <= now))
        f->head++;
}

uint32_t
mach64_gtb_fifo_used(const mach64_gtb_fifo_model_t *f)
{
    return f->tail - f->head;
}

int
mach64_gtb_fifo_busy(const mach64_gtb_fifo_model_t *f, uint64_t now)
{
    return (f->tail != f->head) || (f->free_at > now);
}

void
mach64_gtb_fifo_push(mach64_gtb_fifo_model_t *f, uint64_t arrival, int legacy, int fifo_idx, uint64_t cost)
{
    mach64_gtb_fifo_entry_t *e = &f->ring[f->tail & RING_MASK];

    e->arrival  = arrival;
    e->start    = 0;
    e->cost     = cost;
    e->fifo_idx = fifo_idx;
    e->legacy   = legacy;
    f->tail++;
}

/* Entries whose cost can never arrive (the core FIFO was reset under them). */
void
mach64_gtb_fifo_drop_unfolded(mach64_gtb_fifo_model_t *f)
{
    f->tail = f->fold;
}

double
mach64_gtb_work_seconds(const mach64_gtb_work_t *w, const mach64_gtb_clocks_t *c)
{
    double fraction = c->crtc_fraction;
    double mclk;
    double engine = 0.0;
    double memory = 0.0;

    if (fraction < 0.0)
        fraction = 0.0;
    if (fraction > 0.6)
        fraction = 0.6;
    mclk = c->mclk * (1.0 - fraction);

    if (c->xclk > 0.0)
        engine = w->engine_clocks / c->xclk;
    if (mclk > 0.0)
        memory = w->memory_cycles / mclk;
    return (engine > memory) ? engine : memory;
}

mach64_gtb_clocks_t
mach64_gtb_pll_clocks(const uint8_t pll_regs[64], double cpu_bus_hz)
{
    const mach64_gtb_clocks_t fallback = { 60000000.0, 60000000.0, 0.0 };
    mach64_gtb_clocks_t       c        = fallback;
    unsigned                  ref_div  = pll_regs[2];
    unsigned                  gen_cntl = pll_regs[3];
    unsigned                  fb_div   = pll_regs[4];
    unsigned                  xclk     = pll_regs[11];
    double                    pll_mclk;
    double                    src;

    if (!ref_div || !fb_div)
        return fallback;

    /* MFB_TIMES_4_2b picks a feedback of 4 or 2 times MCLK_FB_DIV. */
    pll_mclk = PLL_REF_HZ * ((xclk & 4) ? 4.0 : 2.0) * (double) fb_div / (double) ref_div;
    switch ((gen_cntl >> 4) & 7) {
        case 0:
            src = pll_mclk;
            break;
        case 1:
            src = pll_mclk / 2.0;
            break;
        case 2:
            src = pll_mclk / 4.0;
            break;
        case 3:
            src = pll_mclk / 8.0;
            break;
        case 4:
            src = cpu_bus_hz;
            break;
        case 6:
        case 7:
            src = PLL_REF_HZ;
            break;
        default:
            return fallback;
    }

    /* XCLK_MCLK_RATIO (PLL_XCLK_CNTL 1:0). */
    switch (xclk & 3) {
        case 0:
            c.xclk = src;
            c.mclk = src;
            break;
        case 1:
            c.xclk = src / 2.0;
            c.mclk = src / 4.0;
            break;
        case 2:
            c.xclk = src / 2.0;
            c.mclk = src / 3.0;
            break;
        default:
            c.xclk = src / 3.0;
            c.mclk = src / 4.0;
            break;
    }

    if ((c.mclk < 20000000.0) || (c.mclk > 150000000.0) ||
        (c.xclk < 20000000.0) || (c.xclk > 150000000.0))
        return fallback;
    return c;
}

static uint64_t
div_up(uint64_t a, uint64_t b)
{
    return (a + b - 1) / b;
}

mach64_gtb_work_t
mach64_gtb_rect_work(const mach64_gtb_rect_t *r)
{
    mach64_gtb_work_t w = { OP_SETUP_CLOCKS, 0.0 };
    uint64_t          left;
    uint64_t          right;
    uint64_t          qwords_row;
    double            accesses_row;
    double            misses = 0.0;
    int32_t           x      = (r->x < 0) ? 0 : r->x;

    if (!r->width || !r->height || !r->dst_bits)
        return w;

    left       = (uint64_t) x * r->dst_bits;
    right      = left + (uint64_t) r->width * r->dst_bits;
    qwords_row = div_up(right, 64) - left / 64;

    /* Unaligned edge QWORDs are read-modify-write even for a plain fill. */
    accesses_row = (double) qwords_row * (r->dst_read ? 2.0 : 1.0);
    if (!r->dst_read)
        accesses_row += ((left & 63) ? 1.0 : 0.0) + ((right & 63) ? 1.0 : 0.0);
    if (r->src_bits)
        accesses_row += (double) div_up((uint64_t) r->width * r->src_bits, 64) + 1.0;

    if (r->src_bits) {
        /* Source reads and destination writes alternate per source-FIFO
           load; each switch opens another page. */
        misses = 2.0 * (double) div_up(qwords_row * r->height, (uint64_t) SRC_FIFO_QWORDS);
    } else {
        uint64_t row_bytes = qwords_row * 8;
        uint64_t base      = (uint64_t) r->dst_offset + (left / 64) * 8;
        uint64_t last      = UINT64_MAX;
        uint32_t rows      = r->height;
        uint32_t walked    = (rows > 2048) ? 2048 : rows;

        for (uint32_t y = 0; y < walked; y++) {
            uint64_t start = base + (uint64_t) y * r->dst_pitch;
            uint64_t p0    = start >> PAGE_SHIFT;
            uint64_t p1    = (start + row_bytes - 1) >> PAGE_SHIFT;

            if (p0 != last)
                misses += 1.0;
            misses += (double) (p1 - p0);
            last = p1;
        }
        if (walked < rows)
            misses *= (double) rows / (double) walked;
    }

    w.memory_cycles = accesses_row * r->height + PAGE_MISS_EXTRA * misses;
    w.engine_clocks += ROW_CLOCKS_2D * r->height;
    return w;
}

mach64_gtb_work_t
mach64_gtb_line_work(uint32_t length, uint32_t dst_bits, uint32_t dst_pitch, int y_major, int dst_read)
{
    mach64_gtb_work_t w = { OP_SETUP_CLOCKS, 0.0 };
    double            accesses;
    double            misses;
    double            pitch_share = (double) dst_pitch / (double) (1u << PAGE_SHIFT);

    if (!length)
        return w;
    if (pitch_share > 1.0)
        pitch_share = 1.0;

    if (y_major) {
        /* Every pixel is on another row. */
        accesses = (double) length * (dst_read ? 2.0 : 1.0);
        misses   = (double) length * pitch_share;
    } else {
        /* Runs along a row share QWORDs; assume a row change every other pixel. */
        accesses = ((double) div_up((uint64_t) length * dst_bits, 64) + length / 2.0) * (dst_read ? 2.0 : 1.0);
        misses   = (length / 2.0) * pitch_share;
    }
    w.engine_clocks += (double) length;
    w.memory_cycles = accesses + PAGE_MISS_EXTRA * misses;
    return w;
}

mach64_gtb_work_t
mach64_gtb_3d_work(const mach64_gtb_3d_t *t)
{
    mach64_gtb_work_t w = { SETUP_CLOCKS_3D + ROW_CLOCKS_3D * t->rows, 0.0 };
    double            per_pixel;
    int               streams = 1;

    if (!t->pixels)
        return w;

    /* QWORD accesses per pixel: colour write, destination read for blending,
       16-bit Z read and write, and texels.  A 2x2 filter shares half of its
       texels with the neighbouring pixel. */
    per_pixel = t->dst_bits / 64.0;
    if (t->dst_read)
        per_pixel += t->dst_bits / 64.0;
    if (t->z_read)
        per_pixel += 16.0 / 64.0;
    if (t->z_write)
        per_pixel += 16.0 / 64.0;
    if (t->z_read || t->z_write)
        streams++;
    if (t->tex_bits) {
        double fetched = (t->texels <= 1) ? 1.0 : ((t->texels <= 2) ? 2.0 : (t->texels / 2.0));

        per_pixel += (t->tex_bits / 64.0) * fetched;
        streams++;
    }

    /* Each extra stream costs a page change every source-FIFO load. */
    w.memory_cycles = t->pixels * per_pixel * (1.0 + (PAGE_MISS_EXTRA / SRC_FIFO_QWORDS) * (streams - 1));
    w.engine_clocks += (double) t->pixels;
    return w;
}
