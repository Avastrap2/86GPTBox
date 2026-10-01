#ifndef VID_ATI_MACH64_GTB_TIMING_H
#define VID_ATI_MACH64_GTB_TIMING_H

#include <stdint.h>

/*
 * Guest-time model of the Rage II+ (GT-B) drawing engine and its command
 * FIFO.  The pure model below takes explicit times so that it can be tested
 * without a CPU; vid_ati_mach64_gtb_timing.c connects it to the emulator.
 */

#define MACH64_GTB_FIFO_DEPTH 48
#define MACH64_GTB_FIFO_RING  64

typedef struct mach64_gtb_fifo_entry_t {
    uint64_t arrival; /* when the CPU wrote it */
    uint64_t start;   /* when the engine took it; valid once folded */
    uint64_t cost;    /* engine time, for entries whose cost was known on arrival */
    int      fifo_idx;
    int      legacy;  /* cost comes from the core FIFO entry fifo_idx */
} mach64_gtb_fifo_entry_t;

typedef struct mach64_gtb_fifo_model_t {
    mach64_gtb_fifo_entry_t ring[MACH64_GTB_FIFO_RING];
    uint32_t head; /* oldest entry still in the FIFO */
    uint32_t fold; /* first entry whose start time is not known yet */
    uint32_t tail;
    uint64_t free_at; /* the engine is busy until then */
} mach64_gtb_fifo_model_t;

/* Engine and memory work for one operation, in their own clocks. */
typedef struct mach64_gtb_work_t {
    double engine_clocks;
    double memory_cycles;
} mach64_gtb_work_t;

typedef struct mach64_gtb_clocks_t {
    double mclk;          /* memory clock, Hz */
    double xclk;          /* engine clock, Hz */
    double crtc_fraction; /* share of memory bandwidth the display fetch takes */
} mach64_gtb_clocks_t;

/* Fold every entry whose cost is known into the engine timeline.  cost_of
   returns 0 when a core FIFO entry has not run yet. */
typedef int (*mach64_gtb_cost_fn)(void *priv, const mach64_gtb_fifo_entry_t *e, uint64_t *cost);

void     mach64_gtb_fifo_fold(mach64_gtb_fifo_model_t *f, mach64_gtb_cost_fn cost_of, void *priv);
void     mach64_gtb_fifo_depart(mach64_gtb_fifo_model_t *f, uint64_t now);
uint32_t mach64_gtb_fifo_used(const mach64_gtb_fifo_model_t *f);
int      mach64_gtb_fifo_busy(const mach64_gtb_fifo_model_t *f, uint64_t now);
void     mach64_gtb_fifo_push(mach64_gtb_fifo_model_t *f, uint64_t arrival, int legacy, int fifo_idx, uint64_t cost);
void     mach64_gtb_fifo_drop_unfolded(mach64_gtb_fifo_model_t *f);

/* Seconds a piece of work takes at the given clocks. */
double mach64_gtb_work_seconds(const mach64_gtb_work_t *w, const mach64_gtb_clocks_t *c);

/* Memory clock and engine clock from the PLL registers (RRG-G02700 B-2). */
mach64_gtb_clocks_t mach64_gtb_pll_clocks(const uint8_t pll_regs[64], double cpu_bus_hz);

/* 2D rectangle and line work, following ATI's method for the Rage II+
   (RAGE PRO and Derivatives Programmer's Guide 7.9.7, table 7-1). */
typedef struct mach64_gtb_rect_t {
    uint32_t width;          /* pixels */
    uint32_t height;
    int32_t  x;              /* starting destination pixel */
    uint32_t dst_offset;     /* destination base, bytes */
    uint32_t dst_pitch;      /* bytes per row */
    uint32_t dst_bits;       /* bits per destination pixel */
    uint32_t src_bits;       /* bits per source pixel; 0 = no screen source */
    int      dst_read;       /* read-modify-write destination */
} mach64_gtb_rect_t;

mach64_gtb_work_t mach64_gtb_rect_work(const mach64_gtb_rect_t *r);
mach64_gtb_work_t mach64_gtb_line_work(uint32_t length, uint32_t dst_bits, uint32_t dst_pitch,
                                       int y_major, int dst_read);

/* 3D trapezoid or line work. */
typedef struct mach64_gtb_3d_t {
    uint32_t pixels;     /* span pixels walked */
    uint32_t rows;       /* scanlines walked */
    uint32_t dst_bits;
    uint32_t tex_bits;   /* 0 = untextured */
    uint32_t texels;     /* texels read per pixel: 1, 4 (2x2) or 8 (two maps) */
    int      z_read;
    int      z_write;
    int      dst_read;   /* alpha blending */
} mach64_gtb_3d_t;

mach64_gtb_work_t mach64_gtb_3d_work(const mach64_gtb_3d_t *t);

#endif
