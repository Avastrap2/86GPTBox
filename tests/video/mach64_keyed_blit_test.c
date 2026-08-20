/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM PC
 *          systems and compatibles from 1981 through fairly recent systems.
 *
 * Mach64 color-compare regression test.
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

enum {
    VRAM_SIZE        = 4 * 1024 * 1024,
    SRC_BYTE_OFFSET  = 0x12f200,
    SRC_PITCH_PIXELS = 128,
    DST_PITCH_PIXELS = 640,
    DST_X            = 100,
    DST_Y            = 100,
    WIDTH            = 128,
    HEIGHT           = 128
};

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

static uint32_t *
pixel32(mach64_t *mach64, uint32_t byte_offset)
{
    return (uint32_t *) &mach64->svga.vram[byte_offset & mach64->vram_mask];
}

static mach64_t *
create_machine(void)
{
    mach64_t  *mach64  = calloc(1, sizeof(*mach64));
    monitor_t *monitor = calloc(1, sizeof(*monitor));

    if (!mach64 || !monitor) {
        free(monitor);
        free(mach64);
        return NULL;
    }

    mach64->svga.vram        = calloc(1, VRAM_SIZE);
    mach64->svga.changedvram = calloc(VRAM_SIZE >> 12,
                                      sizeof(*mach64->svga.changedvram));
    if (!mach64->svga.vram || !mach64->svga.changedvram) {
        free(mach64->svga.changedvram);
        free(mach64->svga.vram);
        free(monitor);
        free(mach64);
        return NULL;
    }

    mach64->svga.monitor = monitor;
    mach64->vram_size    = VRAM_SIZE;
    mach64->vram_mask    = VRAM_SIZE - 1;
    mach64->type         = MACH64_GTB;
    return mach64;
}

static void
destroy_machine(mach64_t *mach64)
{
    monitor_t *monitor;

    if (!mach64)
        return;

    monitor = mach64->svga.monitor;
    free(mach64->svga.changedvram);
    free(mach64->svga.vram);
    free(monitor);
    free(mach64);
}

static void
run_fifo_blit(mach64_t *mach64, uint32_t compare_control,
              uint32_t compare_mask, uint32_t compare_color)
{
    /* Register sequence captured from the 640x480x32 DirectDraw test. */
    mach64_queue(mach64, 0x130, 0x00000003, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2c8, 0xffffffff, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2d0, 0x60060606, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2a8, 0x027f0000, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2b4, 0x01df0000, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x180, 0x04025e40, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x100, 0x14000000, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x198, 0x00800080, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2d8, 0x00000300, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x2d4, 0x00070003, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x308, compare_control, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x304, compare_mask, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x300, compare_color, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x18c, 0x00000000, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x10c, 0x00640064, FIFO_WRITE_DWORD);
    mach64_queue(mach64, 0x118, 0x00800080, FIFO_WRITE_DWORD);

    test_machine       = mach64;
    worker_waits       = 0;
    mach64->thread_run = 1;
    mach64_fifo_thread(mach64);
    test_machine = NULL;
}

static int
run_source_equal(void)
{
    const uint32_t transparent = 0x00ff00ff;
    const uint32_t opaque      = 0x0000ff00;
    const uint32_t background  = 0x000000ff;
    mach64_t     *mach64       = create_machine();
    int           failures     = 0;

    if (!mach64)
        return 1;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const uint32_t src_addr = SRC_BYTE_OFFSET +
                                      ((y * SRC_PITCH_PIXELS + x) * 4);
            const uint32_t dst_addr = ((DST_Y + y) * DST_PITCH_PIXELS +
                                       DST_X + x) * 4;

            *pixel32(mach64, src_addr) =
                (x >= 32 && x < 96 && y >= 32 && y < 96) ? opaque : transparent;
            *pixel32(mach64, dst_addr) = background;
        }
    }

    run_fifo_blit(mach64, 0x01000005, 0xffffffff, transparent);

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const uint32_t src_addr = SRC_BYTE_OFFSET +
                                      ((y * SRC_PITCH_PIXELS + x) * 4);
            const uint32_t dst_addr = ((DST_Y + y) * DST_PITCH_PIXELS +
                                       DST_X + x) * 4;
            const uint32_t expected_src =
                (x >= 32 && x < 96 && y >= 32 && y < 96) ? opaque : transparent;
            const uint32_t expected_dst =
                (x >= 32 && x < 96 && y >= 32 && y < 96) ? opaque : background;
            const uint32_t actual_src = *pixel32(mach64, src_addr);
            const uint32_t actual_dst = *pixel32(mach64, dst_addr);

            if ((actual_src != expected_src || actual_dst != expected_dst) &&
                failures++ < 8) {
                fprintf(stderr,
                        "mismatch at %d,%d: src %08x/%08x, dst %08x/%08x\n",
                        x, y, actual_src, expected_src, actual_dst, expected_dst);
            }
        }
    }

    if (mach64->accel.busy || mach64->fifo_read_idx != mach64->fifo_write_idx)
        failures++;

    printf("source_equal: first=%08x center=%08x failures=%d busy=%d fifo=%d\n",
           *pixel32(mach64, (DST_Y * DST_PITCH_PIXELS + DST_X) * 4),
           *pixel32(mach64,
                    ((DST_Y + 32) * DST_PITCH_PIXELS + DST_X + 32) * 4),
           failures,
           mach64->accel.busy,
           mach64->fifo_write_idx - mach64->fifo_read_idx);

    destroy_machine(mach64);
    return failures;
}

static int
run_destination_compare(const char *name, uint32_t function,
                        uint32_t mask, uint32_t compare_color,
                        uint32_t matching_destination,
                        uint32_t expected_matching,
                        uint32_t expected_nonmatching)
{
    const uint32_t source_color            = 0x0000ff00;
    const uint32_t nonmatching_destination = 0x00ff0000;
    mach64_t     *mach64                   = create_machine();
    int           failures                 = 0;

    if (!mach64)
        return 1;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const uint32_t src_addr = SRC_BYTE_OFFSET +
                                      ((y * SRC_PITCH_PIXELS + x) * 4);
            const uint32_t dst_addr = ((DST_Y + y) * DST_PITCH_PIXELS +
                                       DST_X + x) * 4;

            *pixel32(mach64, src_addr) = source_color;
            *pixel32(mach64, dst_addr) =
                (x < WIDTH / 2) ? matching_destination : nonmatching_destination;
        }
    }

    run_fifo_blit(mach64, function, mask, compare_color);

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const uint32_t dst_addr = ((DST_Y + y) * DST_PITCH_PIXELS +
                                       DST_X + x) * 4;
            const uint32_t expected =
                (x < WIDTH / 2) ? expected_matching : expected_nonmatching;
            const uint32_t actual = *pixel32(mach64, dst_addr);

            if (actual != expected && failures++ < 8) {
                fprintf(stderr, "%s mismatch at %d,%d: expected %08x, got %08x\n",
                        name, x, y, expected, actual);
            }
        }
    }

    if (mach64->accel.busy || mach64->fifo_read_idx != mach64->fifo_write_idx)
        failures++;

    printf("%s: match=%08x nonmatch=%08x failures=%d busy=%d fifo=%d\n",
           name,
           *pixel32(mach64, (DST_Y * DST_PITCH_PIXELS + DST_X) * 4),
           *pixel32(mach64,
                    (DST_Y * DST_PITCH_PIXELS + DST_X + WIDTH / 2) * 4),
           failures,
           mach64->accel.busy,
           mach64->fifo_write_idx - mach64->fifo_read_idx);

    destroy_machine(mach64);
    return failures;
}

int
main(void)
{
    int failures = 0;

    failures += run_source_equal();
    failures += run_destination_compare("destination_equal",
                                        0x00000005, 0xffffffff,
                                        0x000000ff, 0x000000ff,
                                        0x000000ff, 0x0000ff00);
    failures += run_destination_compare("destination_not_equal_masked",
                                        0x00000004, 0x00ffffff,
                                        0x000000ff, 0xa50000ff,
                                        0x0000ff00, 0x00ff0000);
    return failures ? 1 : 0;
}
