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
mach64_3d_rect_debug_begin(mach64_t *mach64)
{
    (void) mach64;
}

void
mach64_3d_rect_debug_end(mach64_t *mach64)
{
    (void) mach64;
}

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

int
main(void)
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

    test_machine       = mach64;
    worker_waits       = 0;
    mach64->thread_run = 1;
    mach64_fifo_thread(mach64);
    test_machine = NULL;

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

    if (mach64->clr_cmp_clr != 0x00007c1f) {
        fprintf(stderr,
                "DP_SET_GUI_ENGINE changed CLR_CMP_CLR=%08x; expected 00007c1f\n",
                mach64->clr_cmp_clr);
        free(mach64);
        return 1;
    }

    if (mach64->clr_cmp_mask != 0xffffffff) {
        fprintf(stderr,
                "DP_SET_GUI_ENGINE changed CLR_CMP_MASK=%08x; expected ffffffff\n",
                mach64->clr_cmp_mask);
        free(mach64);
        return 1;
    }

    printf("dp_set_gui_engine_compare_reset: CNTL=%08x CLR=%08x MASK=%08x\n",
           mach64->clr_cmp_cntl, mach64->clr_cmp_clr, mach64->clr_cmp_mask);
    free(mach64);
    return 0;
}
