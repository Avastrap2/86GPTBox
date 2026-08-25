/* Mach64 GT front-end scaler integration regression test. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/video/vid_ati_mach64_3d.h"
#include "../../src/video/vid_ati_mach64_3d_yuv_math.h"

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
    /* mach64_t stores this field in MiB; vram_mask is the byte range. */
    mach64->vram_size = VRAM_SIZE >> 20;
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

static void
write_yuyv_pair(mach64_t *mach64, uint32_t address,
                uint8_t y0, uint8_t u, uint8_t y1, uint8_t v)
{
    mach64->svga.vram[(address + 0) & mach64->vram_mask] = y0;
    mach64->svga.vram[(address + 1) & mach64->vram_mask] = u;
    mach64->svga.vram[(address + 2) & mach64->vram_mask] = y1;
    mach64->svga.vram[(address + 3) & mach64->vram_mask] = v;
}

static uint32_t
argb_from_centered_yuv(int y, int u, int v)
{
    mach64_yuv_rgb_t rgb = mach64_yuv_centered_to_rgb(y, u, v);

    return 0xff000000u | ((uint32_t) rgb.r << 16) |
           ((uint32_t) rgb.g << 8) | rgb.b;
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

static int
run_rgb_scaler_test(mach64_t *mach64)
{
    static const uint32_t colors[4] = {
        0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu
    };
    int failures = 0;

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
    return failures;
}

static int
run_yuyv_scaler_test(mach64_t *mach64)
{
    static const int expected_u[4] = { 0, 32, 64, 64 };
    int failures = 0;

    memset(mach64->svga.vram, 0, 64u * 4u);
    write_yuyv_pair(mach64, SOURCE_OFFSET + 0, 100, 128, 100, 128);
    write_yuyv_pair(mach64, SOURCE_OFFSET + 4, 100, 192, 100, 128);

    mach64->dp_pix_width = 0xb0000606u; /* SCALE=YUYV, DST=ARGB8888 */
    mach64->dst_y_x = 0;
    mach64->sc_left_right = 0x00030000u;
    mach64->sc_top_bottom = 0x00000000u;

    failures += write_scaler_register(mach64, 0x1c0, SOURCE_OFFSET);
    failures += write_scaler_register(mach64, 0x1dc, 4);
    failures += write_scaler_register(mach64, 0x1e0, 1);
    failures += write_scaler_register(mach64, 0x1ec, 4);
    failures += write_scaler_register(mach64, 0x1f0, 0x00010000u);
    failures += write_scaler_register(mach64, 0x1f4, 0x00010000u);
    failures += write_scaler_register(mach64, 0x1f8, 0);
    failures += write_scaler_register(mach64, 0x3c8, 0);
    failures += write_scaler_register(mach64, 0x3d8, 0x00008000u);
    failures += write_scaler_register(mach64, 0x3e0, 0);
    failures += write_scaler_register(mach64, 0x1fc, 0x00000040u);

    if (!mach64_3d_write(mach64, 0x118, 0x00040001u,
                         FIFO_WRITE_DWORD)) {
        fprintf(stderr, "YUYV scaler destination trigger was not claimed\n");
        failures++;
    }

    for (int x = 0; x < 4; x++) {
        uint32_t expected = argb_from_centered_yuv(100, expected_u[x], 0);
        uint32_t actual = *pixel32(mach64, (uint32_t) x * 4u);

        if (actual != expected) {
            fprintf(stderr,
                    "YUYV pixel %d: expected %08x, got %08x\n",
                    x, expected, actual);
            failures++;
        }
    }
    return failures;
}

static int
run_apple_yuv_test(mach64_t *mach64)
{
    int failures = 0;
    const uint32_t expected = argb_from_centered_yuv(100, 0, 0);

    memset(mach64->svga.vram, 0, 64u * 4u);
    write_yuyv_pair(mach64, SOURCE_OFFSET + 0, 100, 0, 100, 0);
    write_yuyv_pair(mach64, SOURCE_OFFSET + 4, 100, 0, 100, 0);

    failures += write_scaler_register(mach64, 0x1f8, 0);
    failures += write_scaler_register(mach64, 0x3c8, 0);
    failures += write_scaler_register(mach64, 0x3e0, 0);
    failures += write_scaler_register(mach64, 0x1fc, 0x00000440u);

    if (!mach64_3d_write(mach64, 0x118, 0x00040001u,
                         FIFO_WRITE_DWORD)) {
        fprintf(stderr, "APPLE YUV scaler destination trigger was not claimed\n");
        failures++;
    }

    for (int x = 0; x < 4; x++) {
        uint32_t actual = *pixel32(mach64, (uint32_t) x * 4u);

        if (actual != expected) {
            fprintf(stderr,
                    "APPLE YUV pixel %d: expected %08x, got %08x\n",
                    x, expected, actual);
            failures++;
        }
    }
    return failures;
}

static int
run_scaler_color_compare_test(mach64_t *mach64)
{
    const uint32_t source = 0x00ff0000u;
    const uint32_t destination = 0x0000ff00u;
    int failures = 0;

    *pixel32(mach64, SOURCE_OFFSET) = source;
    *pixel32(mach64, 0) = destination;

    mach64->dp_pix_width = 0x60000606u; /* SCALE=ARGB8888, DST=ARGB8888 */
    mach64->dp_src = 0x00000500u;
    mach64->dp_mix = 0x00070007u;
    mach64->write_mask = 0xffffffffu;
    mach64->dst_off_pitch = 0x02000000u; /* 64 pixels */
    mach64->dst_y_x = 0;
    mach64->dst_cntl = DST_X_DIR | DST_Y_DIR;
    mach64->sc_left_right = 0;
    mach64->sc_top_bottom = 0;

    /* GT CLR_CMP_SRC=2 selects the scaler source.  Put ignored bits in the
     * key to verify that the comparison mask applies to CLR_CMP_CLR too. */
    mach64->clr_cmp_clr = 0xaaff0000u;
    mach64->clr_cmp_mask = 0x00ffffffu;
    mach64->clr_cmp_cntl = 0x02000005u;

    failures += write_scaler_register(mach64, 0x1c0, SOURCE_OFFSET);
    failures += write_scaler_register(mach64, 0x1dc, 1);
    failures += write_scaler_register(mach64, 0x1e0, 1);
    failures += write_scaler_register(mach64, 0x1ec, 1);
    failures += write_scaler_register(mach64, 0x1f0, 0);
    failures += write_scaler_register(mach64, 0x1f4, 0);
    failures += write_scaler_register(mach64, 0x1f8, 0);
    failures += write_scaler_register(mach64, 0x3c8, 0);
    failures += write_scaler_register(mach64, 0x1fc, 0x00000140u);

    if (!mach64_3d_write(mach64, 0x118, 0x00010001u,
                         FIFO_WRITE_DWORD)) {
        fprintf(stderr, "color-key scaler destination trigger was not claimed\n");
        failures++;
    }

    if (*pixel32(mach64, 0) != destination) {
        fprintf(stderr,
                "CLR_CMP_SRC=2 failed: expected inhibited destination %08x, got %08x\n",
                destination, *pixel32(mach64, 0));
        failures++;
    }

    mach64->clr_cmp_cntl = 0;
    return failures;
}

int
main(void)
{
    mach64_t *mach64 = create_machine();
    int failures = 0;

    if (!mach64)
        return 1;

    mach64_3d_attach(mach64);
    failures += run_rgb_scaler_test(mach64);
    failures += run_yuyv_scaler_test(mach64);
    failures += run_apple_yuv_test(mach64);
    failures += run_scaler_color_compare_test(mach64);

    destroy_machine(mach64);
    return failures ? 1 : 0;
}
