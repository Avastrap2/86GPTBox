/*
 * Rage II+ drawing-engine timing model: FIFO sequencing, PLL clock decoding
 * and the memory costs of ATI's documented method.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/video/vid_ati_mach64_gtb_timing.h"

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
            failures++;                                     \
        }                                                   \
    } while (0)

static int
near(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

/* Core FIFO entries with index < executed have run; their cost is idx * 7. */
typedef struct {
    int executed;
} core_t;

static int
cost_of(void *priv, const mach64_gtb_fifo_entry_t *e, uint64_t *cost)
{
    const core_t *core = (const core_t *) priv;

    if (!e->legacy) {
        *cost = e->cost;
        return 1;
    }
    if (e->fifo_idx >= core->executed)
        return 0;
    *cost = (uint64_t) e->fifo_idx * 7;
    return 1;
}

static void
fifo_sequencing(void)
{
    mach64_gtb_fifo_model_t f;
    core_t                  core = { 0 };

    memset(&f, 0, sizeof(f));
    mach64_gtb_fifo_push(&f, 0, 0, 0, 10);
    mach64_gtb_fifo_push(&f, 0, 0, 0, 0);
    mach64_gtb_fifo_push(&f, 0, 0, 0, 100);
    mach64_gtb_fifo_fold(&f, cost_of, &core);
    CHECK(f.fold == 3 && f.free_at == 110, "fold %u free_at %llu", f.fold, (unsigned long long) f.free_at);

    /* The first entry is taken at once; the others wait for its 10 ticks. */
    mach64_gtb_fifo_depart(&f, 5);
    CHECK(mach64_gtb_fifo_used(&f) == 2, "used at 5: %u", mach64_gtb_fifo_used(&f));
    CHECK(mach64_gtb_fifo_busy(&f, 5), "busy at 5");
    mach64_gtb_fifo_depart(&f, 10);
    CHECK(mach64_gtb_fifo_used(&f) == 0, "used at 10: %u", mach64_gtb_fifo_used(&f));
    CHECK(mach64_gtb_fifo_busy(&f, 109), "engine still busy at 109");
    CHECK(!mach64_gtb_fifo_busy(&f, 110), "idle at 110");

    /* An entry arriving after the engine went idle starts on arrival. */
    mach64_gtb_fifo_push(&f, 500, 0, 0, 20);
    mach64_gtb_fifo_fold(&f, cost_of, &core);
    CHECK(f.ring[3].start == 500 && f.free_at == 520, "start %llu free_at %llu",
          (unsigned long long) f.ring[3].start, (unsigned long long) f.free_at);
}

static void
fifo_waits_for_core(void)
{
    mach64_gtb_fifo_model_t f;
    core_t                  core = { 0 };

    memset(&f, 0, sizeof(f));
    mach64_gtb_fifo_push(&f, 0, 1, 3, 0);  /* core entry 3: cost 21 once run */
    mach64_gtb_fifo_push(&f, 0, 0, 0, 5);  /* 3D entry behind it */
    mach64_gtb_fifo_fold(&f, cost_of, &core);
    CHECK(f.fold == 0, "a core entry that has not run cannot be folded (fold %u)", f.fold);
    mach64_gtb_fifo_depart(&f, 1000);
    CHECK(mach64_gtb_fifo_used(&f) == 2, "unknown costs keep entries in the FIFO");

    core.executed = 4;
    mach64_gtb_fifo_fold(&f, cost_of, &core);
    CHECK(f.fold == 2 && f.free_at == 26, "fold %u free_at %llu", f.fold, (unsigned long long) f.free_at);

    /* Entries orphaned by a core reset are dropped, not waited for forever. */
    mach64_gtb_fifo_push(&f, 30, 1, 99, 0);
    mach64_gtb_fifo_fold(&f, cost_of, &core);
    mach64_gtb_fifo_drop_unfolded(&f);
    mach64_gtb_fifo_depart(&f, 30);
    CHECK(mach64_gtb_fifo_used(&f) == 0, "dropped entries leave the FIFO");
}

static void
pll_clocks(void)
{
    uint8_t             pll[64];
    mach64_gtb_clocks_t c;
    double              expect;

    memset(pll, 0, sizeof(pll));
    pll[2]  = 0x21; /* PLL_REF_DIV */
    pll[3]  = 0x10; /* MCLK_SRC = PLLMCLK / 2 */
    pll[4]  = 0x96; /* MCLK_FB_DIV */
    pll[11] = 0x00; /* XCLK = MCLK = MCLK_SRC, feedback 2 * MCLK_FB_DIV */
    c       = mach64_gtb_pll_clocks(pll, 33333333.0);
    expect  = 14318180.0 * 2.0 * 0x96 / 0x21 / 2.0;
    CHECK(near(c.mclk, expect, 1.0) && near(c.xclk, expect, 1.0), "mclk %.0f xclk %.0f expect %.0f", c.mclk, c.xclk, expect);

    pll[11] = 0x04; /* feedback 4 * MCLK_FB_DIV */
    pll[3]  = 0x20; /* MCLK_SRC = PLLMCLK / 4 */
    c       = mach64_gtb_pll_clocks(pll, 33333333.0);
    CHECK(near(c.mclk, expect, 1.0), "MFB_TIMES_4 mclk %.0f", c.mclk);

    pll[11] = 0x06; /* XCLK = SRC / 2, MCLK = SRC / 3 */
    c       = mach64_gtb_pll_clocks(pll, 33333333.0);
    CHECK(near(c.xclk, expect / 2.0, 1.0) && near(c.mclk, expect / 3.0, 1.0), "ratio 10: xclk %.0f mclk %.0f", c.xclk, c.mclk);

    /* Unprogrammed or implausible PLLs fall back to 60 MHz. */
    memset(pll, 0, sizeof(pll));
    c = mach64_gtb_pll_clocks(pll, 33333333.0);
    CHECK(c.mclk == 60000000.0 && c.xclk == 60000000.0, "fallback %.0f %.0f", c.mclk, c.xclk);
}

static void
rect_costs(void)
{
    mach64_gtb_rect_t   r;
    mach64_gtb_work_t   w;
    mach64_gtb_clocks_t c = { 60000000.0, 60000000.0, 0.0 };

    /* 640x480x16 write-only fill on a 1280-byte pitch: 76,800 QWORDs, one
       cycle each, and 150 four-KiB pages, each opened once (ATI 7.9.7). */
    memset(&r, 0, sizeof(r));
    r.width     = 640;
    r.height    = 480;
    r.dst_pitch = 1280;
    r.dst_bits  = 16;
    w           = mach64_gtb_rect_work(&r);
    CHECK(near(w.memory_cycles, 76800.0 + 6.0 * 150.0, 0.5), "fill memory %.1f", w.memory_cycles);
    CHECK(near(mach64_gtb_work_seconds(&w, &c), 77700.0 / 60000000.0, 1e-9), "fill seconds %.9f",
          mach64_gtb_work_seconds(&w, &c));

    /* The display's share of the bandwidth slows memory-bound work. */
    c.crtc_fraction = 0.25;
    CHECK(near(mach64_gtb_work_seconds(&w, &c), 77700.0 / 45000000.0, 1e-9), "fill with display %.9f",
          mach64_gtb_work_seconds(&w, &c));
    c.crtc_fraction = 0.0;

    /* An XOR blit reads the source and the destination and writes the
       destination: ATI's example, 3 accesses per QWORD. */
    r.width     = 160;
    r.height    = 120;
    r.dst_pitch = 1024;
    r.dst_bits  = 8;
    r.src_bits  = 8;
    r.dst_read  = 1;
    w           = mach64_gtb_rect_work(&r);
    CHECK(w.memory_cycles > 120.0 * 20.0 * 3.0, "xor blit memory %.1f", w.memory_cycles);
    CHECK(w.memory_cycles < 120.0 * 20.0 * 3.25 * 1.3, "xor blit memory %.1f", w.memory_cycles);

    /* Unaligned edges cost a read each. */
    memset(&r, 0, sizeof(r));
    r.width     = 10;
    r.height    = 1;
    r.x         = 1;
    r.dst_pitch = 4096;
    r.dst_bits  = 16;
    w           = mach64_gtb_rect_work(&r);
    CHECK(near(w.memory_cycles, 3.0 + 2.0 + 6.0, 0.5), "unaligned memory %.1f", w.memory_cycles);

    /* Nothing to draw: setup only. */
    r.width = 0;
    w       = mach64_gtb_rect_work(&r);
    CHECK(w.memory_cycles == 0.0 && w.engine_clocks > 0.0, "empty rect");
}

static void
three_d_costs(void)
{
    mach64_gtb_3d_t     t;
    mach64_gtb_work_t   flat, textured;
    mach64_gtb_clocks_t c = { 60000000.0, 60000000.0, 0.0 };

    memset(&t, 0, sizeof(t));
    t.pixels   = 1000;
    t.rows     = 10;
    t.dst_bits = 16;
    flat       = mach64_gtb_3d_work(&t);
    /* Flat 16 bpp without Z: the pipeline, one pixel a clock, is the limit. */
    CHECK(flat.engine_clocks > flat.memory_cycles, "flat engine %.1f memory %.1f", flat.engine_clocks, flat.memory_cycles);

    t.z_read   = 1;
    t.z_write  = 1;
    t.tex_bits = 16;
    t.texels   = 4;
    textured   = mach64_gtb_3d_work(&t);
    CHECK(textured.memory_cycles > textured.engine_clocks, "bilinear+Z is memory bound");
    CHECK(mach64_gtb_work_seconds(&textured, &c) > mach64_gtb_work_seconds(&flat, &c) * 1.5,
          "bilinear+Z slower than flat");
}

int
main(void)
{
    fifo_sequencing();
    fifo_waits_for_core();
    pll_clocks();
    rect_costs();
    three_d_costs();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mach64_gtb_timing: all checks passed\n");
    return 0;
}
