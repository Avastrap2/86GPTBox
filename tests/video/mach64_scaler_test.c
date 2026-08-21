/* Mach64 GT front-end scaler integration regression test. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/video/vid_ati_mach64_3d.h"

enum {
    VRAM_SIZE = 4 * 1024 * 1024,
    SOURCE_OFFSET = 0x10000,
    SOURCE_SIZE = 32,
    DESTINATION_SIZE = 64
};

const device_t mach64vt2_device = { 0 };

void
fatal(const char *format, ...)
{
    (void) format;
    abort();
}

void
pclog(const char *format, ...)
{
    (void) format;
}

uint64_t
plat_timer_read(void)
{
    return 0;
}

void
mach64_wake_fifo_thread(mach64_t *mach64)
{
    (void) mach64;
}

void
mach64_wait_fifo_idle(mach64_t *mach64)
{
    mach64->fifo_read_idx = mach64->fifo_write_idx;
}

uint8_t
mach64_ext_readb(uint32_t address, void *private_data)
{
    (void) address;
    (void) private_data;
    return 0;
}

uint16_t
mach64_ext_readw(uint32_t address, void *private_data)
{
    (void) address;
    (void) private_data;
    return 0;
}

uint32_t
mach64_ext_readl(uint32_t address, void *private_data)
{
    (void) address;
    (void) private_data;
    return 0;
}

int
mach64_gtb_cfg_readb(mach64_t *mach64, uint32_t address, uint8_t *value)
{
    (void) mach64;
    (void) address;
    (void) value;
    return 0;
}

int
mach64_gtb_cfg_writeb(mach64_t *mach64, uint32_t address, uint8_t value)
{
    (void) mach64;
    (void) address;
    (void) value;
    return 0;
}

void
mach64_pci_write_gtb_legacy_dispatch(int function, int address, int length,
                                     uint8_t value, void *private_data)
{
    (void) function;
    (void) address;
    (void) length;
    (void) value;
    (void) private_data;
}

static mach64_t *
create_machine(void)
{
    mach64_t *mach64 = calloc(1, sizeof(*mach64));
    monitor_t *monitor = calloc(1, sizeof(*monitor));

    if (!mach64 || !monitor)
        goto failure;
    mach64->svga.vram = calloc(1, VRAM_SIZE);
    mach64->svga.changedvram = calloc(VRAM_SIZE >> 12,
                                      sizeof(*mach64->svga.changedvram));
    if (!mach64->svga.vram || !mach64->svga.changedvram)
        goto failure;

    mach64->svga.monitor = monitor;
    mach64->vram_size = VRAM_SIZE;
    mach64->vram_mask = VRAM_SIZE - 1;
    mach64->type = MACH64_GTB;
    return mach64;

failure:
    if (mach64) {
        free(mach64->svga.changedvram);
        free(mach64->svga.vram);
    }
    free(monitor);
    free(mach64);
    return NULL;
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

static uint32_t *
pixel32(mach64_t *mach64, uint32_t address)
{
    return (uint32_t *) &mach64->svga.vram[address & mach64->vram_mask];
}

static int
write_scaler_register(mach64_t *mach64, uint32_t address, uint32_t value)
{
    if (!mach64_3d_write(mach64, address, value, FIFO_WRITE_DWORD)) {
        fprintf(stderr, "scaler register %03x was not claimed\n", address);
        return 1;
    }
    return 0;
}

int
main(void)
{
    static const uint32_t colors[4] = {
        0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu
    };
    mach64_t *mach64 = create_machine();
    int failures = 0;

    if (!mach64)
        return 1;

    for (int y = 0; y < SOURCE_SIZE; y++) {
        for (int x = 0; x < SOURCE_SIZE; x++) {
            unsigned quadrant = (x >= 16 ? 1u : 0u) |
                                  (y >= 16 ? 2u : 0u);
            *pixel32(mach64, SOURCE_OFFSET +
                     (uint32_t) (y * SOURCE_SIZE + x) * 4u) =
                colors[quadrant];
        }
    }

    mach64->dp_pix_width = 0x60000606u;
    mach64->dp_src = 0x00000500u;
    mach64->dp_mix = 0x00070007u;
    mach64->write_mask = 0xffffffffu;
    mach64->dst_off_pitch = 0x02000000u; /* 64 pixels */
    mach64->dst_y_x = 0;
    mach64->dst_cntl = DST_X_DIR | DST_Y_DIR;
    mach64->sc_left_right = 0x003f0000u;
    mach64->sc_top_bottom = 0x003f0000u;

    mach64_3d_attach(mach64);
    failures += write_scaler_register(mach64, 0x1c0, SOURCE_OFFSET);
    failures += write_scaler_register(mach64, 0x1dc, SOURCE_SIZE);
    failures += write_scaler_register(mach64, 0x1e0, SOURCE_SIZE);
    failures += write_scaler_register(mach64, 0x1ec, SOURCE_SIZE);
    failures += write_scaler_register(mach64, 0x1f0, 0x00008000u);
    failures += write_scaler_register(mach64, 0x1f4, 0x00008000u);
    failures += write_scaler_register(mach64, 0x1f8, 0);
    failures += write_scaler_register(mach64, 0x3c8, 0);
    failures += write_scaler_register(mach64, 0x1fc, 0x00000140u);

    if (!mach64_3d_write(mach64, 0x118, 0x00400040u,
                         FIFO_WRITE_DWORD)) {
        fprintf(stderr, "scaler destination trigger was not claimed\n");
        failures++;
    }

    for (int y = 0; y < DESTINATION_SIZE; y++) {
        for (int x = 0; x < DESTINATION_SIZE; x++) {
            unsigned quadrant = (x >= 32 ? 1u : 0u) |
                                  (y >= 32 ? 2u : 0u);
            uint32_t actual = *pixel32(mach64,
                                       (uint32_t) (y * DESTINATION_SIZE + x) *
                                       4u);

            if (actual != colors[quadrant]) {
                fprintf(stderr,
                        "scaled pixel %d,%d: expected %08x, got %08x\n",
                        x, y, colors[quadrant], actual);
                failures++;
                y = DESTINATION_SIZE;
                break;
            }
        }
    }

    destroy_machine(mach64);
    return failures ? 1 : 0;
}
