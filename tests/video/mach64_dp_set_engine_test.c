/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM PC
 *          systems and compatibles from 1981 through fairly recent systems.
 *
 * Mach64 DP_SET_GUI_ENGINE state-reset regression test.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/video/vid_ati_mach64.h"

int mach64_width[8] = { WIDTH_1BIT, 0, 0, 1, 1, 2, 2, 0 };
monitor_t monitors[MONITORS_NUM];
int       monitor_index_global;

static mach64_t *test_machine;
static int       worker_waits;

void
thread_set_event(event_t *event)
{
    (void) event;
}

void
thread_reset_event(event_t *event)
{
    (void) event;
}

int
thread_wait_event(event_t *event, int timeout)
{
    (void) event;
    (void) timeout;

    if (test_machine && (++worker_waits > 1))
        test_machine->thread_run = 0;
    return 0;
}

/* Uncontended FIFO lock: these tests run the worker inline on one thread. */
int
thread_wait_mutex(mutex_t *mutex)
{
    (void) mutex;
    return 1;
}

int
thread_release_mutex(mutex_t *mutex)
{
    (void) mutex;
    return 1;
}

uint64_t
plat_timer_read(void)
{
    return 0;
}

void
pclog(const char *format, ...)
{
    (void) format;
}

static void
run_fifo(mach64_t *mach64)
{
    test_machine       = mach64;
    worker_waits       = 0;
    mach64->thread_run = 1;
    mach64_fifo_thread(mach64);
    test_machine = NULL;
}

static int
compare_reset_case(void)
{
    mach64_t *mach64 = calloc(1, sizeof(*mach64));

    if (!mach64)
        return 1;

    /* Match the source-equal transparent BLT state used by ATI's TBLIT sample. */
    mach64->clr_cmp_cntl = 0x01000005;
    mach64->clr_cmp_clr  = 0x00007c1f;
    mach64->clr_cmp_mask = 0xffffffff;

    /* DP_SET_GUI_ENGINE is DWORD BF / byte offset 0x2fc. */
    mach64_queue(mach64, 0x2fc, 0x00000000, FIFO_WRITE_DWORD);
    run_fifo(mach64);

    if (mach64->fifo_read_idx != mach64->fifo_write_idx) {
        fprintf(stderr, "DP_SET_GUI_ENGINE command was not drained from the FIFO\n");
        free(mach64);
        return 1;
    }

    if (mach64->clr_cmp_cntl != 0) {
        fprintf(stderr,
                "DP_SET_GUI_ENGINE leaked CLR_CMP_CNTL=%08x; expected compare disabled\n",
                mach64->clr_cmp_cntl);
        free(mach64);
        return 1;
    }

    printf("dp_set_gui_engine_compare_reset: CLR_CMP_CNTL=%08x\n",
           mach64->clr_cmp_cntl);
    free(mach64);
    return 0;
}

enum {
    VRAM_SIZE   = 4 * 1024 * 1024,
    FILL_PITCH  = 1024,
    FILL_X      = 16,
    FILL_Y      = 8,
    FILL_WIDTH  = 4,
    FILL_HEIGHT = 2,
    FILL_COLOR  = 0x00c0c0c0
};

/*
 * The Rage II+ Windows 95 driver resets the engine with DP_SET_GUI_ENGINE
 * before each solid fill and then writes only the colour and rectangle.  That
 * reset leaves SC_LEFT_RIGHT at 1FFF0000h, "OPEN completely" in ATI's
 * register guide, so the fill must not be scissored away; an SC_RIGHT of
 * 3FFFh (-1 in the GT-B's 14 bits) written afterwards still clips it all.
 * Returns the number of rectangle pixels written with the fill colour.
 */
static int
gtb_fill_pixels(uint32_t sc_left_right)
{
    mach64_t  *mach64  = calloc(1, sizeof(*mach64));
    monitor_t *monitor = calloc(1, sizeof(*monitor));
    int        drawn   = -1;

    if (!mach64 || !monitor)
        goto done;
    mach64->svga.vram        = calloc(1, VRAM_SIZE);
    mach64->svga.changedvram = calloc(VRAM_SIZE >> 12, sizeof(*mach64->svga.changedvram));
    if (!mach64->svga.vram || !mach64->svga.changedvram)
        goto done;
    mach64->svga.monitor = monitor;
    mach64->vram_size    = VRAM_SIZE;
    mach64->vram_mask    = VRAM_SIZE - 1;
    mach64->type         = MACH64_GTB;

    /* Register sequence captured from the driver: 32 bpp, 1024 pitch, the
       FgFrgdClr drawing combo. */
    mach64_queue(mach64, 0x2fc, 0x0010a070, FIFO_WRITE_DWORD);
    if (sc_left_right)
        mach64_queue(mach64, 0x2a8, sc_left_right, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2c4, FILL_COLOR, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x10c, (FILL_X << 16) | FILL_Y, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x118, (FILL_WIDTH << 16) | FILL_HEIGHT, FIFO_WRITE_DWORD);
    run_fifo(mach64);

    drawn = 0;
    for (int y = 0; y < FILL_HEIGHT; y++) {
        for (int x = 0; x < FILL_WIDTH; x++) {
            uint32_t off = (((FILL_Y + y) * FILL_PITCH) + FILL_X + x) * 4;

            if (*(uint32_t *) &mach64->svga.vram[off] == FILL_COLOR)
                drawn++;
        }
    }

done:
    if (mach64) {
        free(mach64->svga.changedvram);
        free(mach64->svga.vram);
    }
    free(monitor);
    free(mach64);
    return drawn;
}

static int
gtb_fill_scissor_case(void)
{
    int open    = gtb_fill_pixels(0);
    int clipped = gtb_fill_pixels(0x3fff0000);

    if (open != FILL_WIDTH * FILL_HEIGHT) {
        fprintf(stderr, "GT-B fill after DP_SET_GUI_ENGINE drew %d of %d pixels\n",
                open, FILL_WIDTH * FILL_HEIGHT);
        return 1;
    }
    if (clipped != 0) {
        fprintf(stderr, "GT-B fill with SC_RIGHT=3FFFh (-1) drew %d pixels\n", clipped);
        return 1;
    }
    printf("dp_set_gui_engine_gtb_fill: open %d, clipped %d\n", open, clipped);
    return 0;
}

int
main(void)
{
    if (compare_reset_case())
        return 1;
    return gtb_fill_scissor_case();
}
