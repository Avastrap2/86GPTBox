/*
 * GT/GTB command-level triangle regressions. Link the production 3D renderer,
 * program its public register interface, and check framebuffer/Z contents.
 * Shared legacy registers are seeded directly: these tests do not model CPU
 * MMIO dispatch, the asynchronous 2D FIFO, a guest driver, or real hardware.
 *
 * Command/trajectory definitions: RRG-G02700 Rev. 0.10, printed pages
 * 4-43 through 4-48 (especially the DST_BRES_LNTH bit 31/15 table on 4-46).
 * The expected triangle and interpolants below are closed-form expressions;
 * they do not call the renderer's edge, clipping, or interpolation helpers.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/video/vid_ati_mach64_3d.h"

enum {
    VRAM_SIZE = 4 * 1024 * 1024,
    SIZE = 64,
    Z_BASE = 0x10000,
    TEX_BASE = 0x20000,
    LEAD_LENGTH = 0x120,
    DST_Y_X_ALIAS = 0x134,
    TRAIL_ERR = 0x138,
    TRAIL_INC = 0x13c,
    TRAIL_DEC = 0x140,
    LEAD_ALIAS = 0x144,
    Z_OFF_PITCH = 0x148,
    Z_CNTL = 0x14c,
    TEX_3_OFF = 0x1cc,
    SCALE_3D_CNTL = 0x1fc,
    S_X_INC2 = 0x340,
    S_Y_INC2 = 0x344,
    S_XY_INC2 = 0x348,
    S_X_INC = 0x34c,
    S_Y_INC = 0x350,
    S_START = 0x354,
    T_X_INC2 = 0x358,
    T_Y_INC2 = 0x35c,
    T_XY_INC2 = 0x360,
    T_X_INC = 0x364,
    T_Y_INC = 0x368,
    T_START = 0x36c,
    TEX_SIZE_PITCH = 0x370,
    RED_X_INC = 0x3c0,
    RED_Y_INC = 0x3c4,
    RED_START = 0x3c8,
    GREEN_X_INC = 0x3cc,
    GREEN_Y_INC = 0x3d0,
    GREEN_START = 0x3d4,
    BLUE_X_INC = 0x3d8,
    BLUE_Y_INC = 0x3dc,
    BLUE_START = 0x3e0,
    Z_X_INC = 0x3e4,
    Z_Y_INC = 0x3e8,
    Z_START = 0x3ec,
    ALPHA_X_INC = 0x3f0,
    ALPHA_Y_INC = 0x3f4,
    ALPHA_START = 0x3f8,
    DRAW_TRAP = 1u << 15,
    TRAIL_X_DIR = 1u << 13,
    TRAP_FILL_DIR = 1u << 14,
    SHADE = 3u << 6,
    TEXTURE = (2u << 6) | (1u << 24), /* Single map, nearest sampling. */
    Z_LEQUAL_WRITE = 0x121
};

#define LOAD_TRAIL 0x80000000u
#define BACKGROUND 0x19324b64u
#define INITIAL_Z 60000u

const device_t mach64vt2_device = { 0 };
static unsigned fifo_waits;
/* Optional delayed shared-state update for command-dispatch ordering tests. */
static int fifo_shared_update;
static uint32_t fifo_shared_source, fifo_shared_cntl;

/* Unused platform/core dependencies of the standalone renderer. */
void fatal(const char *format, ...) { (void) format; abort(); }
void pclog(const char *format, ...) { (void) format; }
uint64_t plat_timer_read(void) { return 0; }
void mach64_wake_fifo_thread(mach64_t *m) { (void) m; }
void mach64_wait_fifo_idle(mach64_t *m)
{
    fifo_waits++;
    if (fifo_shared_update) {
        m->dp_src = fifo_shared_source;
        m->dst_cntl = fifo_shared_cntl;
        fifo_shared_update = 0;
    }
    m->fifo_read_idx = m->fifo_write_idx;
}
uint8_t mach64_ext_readb(uint32_t a, void *p) { (void) a; (void) p; return 0; }
uint16_t mach64_ext_readw(uint32_t a, void *p) { (void) a; (void) p; return 0; }
uint32_t mach64_ext_readl(uint32_t a, void *p) { (void) a; (void) p; return 0; }
int mach64_gtb_cfg_readb(mach64_t *m, uint32_t a, uint8_t *v)
{ (void) m; (void) a; (void) v; return 0; }
int mach64_gtb_cfg_writeb(mach64_t *m, uint32_t a, uint8_t v)
{ (void) m; (void) a; (void) v; return 0; }
void mach64_pci_write_gtb_legacy_dispatch(int f, int a, int n, uint8_t v, void *p)
{ (void) f; (void) a; (void) n; (void) v; (void) p; }

static unsigned failures;
static unsigned cases;
static char case_name[160];

static void
expect(const char *what, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        if (failures < 24)
            fprintf(stderr, "%s: %s: got %08x, expected %08x\n",
                    case_name, what, actual, expected);
        failures++;
    }
}

static void
write_reg(mach64_t *m, uint32_t address, uint32_t value)
{
    expect("3D register claimed", mach64_3d_write(m, address, value,
                                                  FIFO_WRITE_DWORD), 1);
}

static uint32_t
read_reg(mach64_t *m, uint32_t address)
{
    uint32_t value = 0;
    expect("3D read claimed", mach64_3d_read(m, address, &value), 1);
    return value;
}

static uint32_t
load_pixel(mach64_t *m, unsigned address, unsigned bytes)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < bytes; i++)
        value |= (uint32_t) m->svga.vram[address + i] << (8 * i);
    return value;
}

static void
store_pixel(mach64_t *m, unsigned address, unsigned bytes, uint32_t value)
{
    for (unsigned i = 0; i < bytes; i++)
        m->svga.vram[address + i] = (uint8_t) (value >> (8 * i));
}

static uint32_t
pack_color(uint32_t color, int format)
{
    if (format == 3) /* ARGB1555: alpha occupies the otherwise unused high bit. */
        return ((color >> 16) & 0x8000u) | ((color >> 9) & 0x7c00u) |
               ((color >> 6) & 0x03e0u) | ((color >> 3) & 0x001fu);
    if (format == 4) /* RGB565, dithering/rounding disabled. */
        return ((color >> 8) & 0xf800u) | ((color >> 5) & 0x07e0u) |
               ((color >> 3) & 0x001fu);
    return color;
}

static unsigned
destination_bytes(int format)
{
    return format == 6 ? 4 : 2;
}

static uint32_t
texture_color(int u, int v)
{
    return 0xff000000u | (uint32_t) (40 + 20 * u) << 16 |
           (uint32_t) (30 + 24 * v) << 8 | (uint32_t) (16 + 8 * (u + v));
}

static mach64_t *
create_machine(void)
{
    mach64_t *m = calloc(1, sizeof(*m));
    if (!m)
        abort();
    m->svga.monitor = calloc(1, sizeof(*m->svga.monitor));
    m->svga.vram = calloc(1, VRAM_SIZE);
    m->svga.changedvram = calloc(VRAM_SIZE >> 12, sizeof(*m->svga.changedvram));
    if (!m->svga.monitor || !m->svga.vram || !m->svga.changedvram)
        abort();
    m->vram_size = VRAM_SIZE >> 20; /* This member is in MiB. */
    m->vram_mask = VRAM_SIZE - 1;
    m->type = MACH64_GTB;
    return m;
}

static void
clear_surfaces(mach64_t *m, int format)
{
    unsigned bytes = destination_bytes(format);
    for (unsigned i = 0; i < SIZE * SIZE; i++) {
        store_pixel(m, i * bytes, bytes, pack_color(BACKGROUND, format));
        store_pixel(m, Z_BASE + i * 2, 2, INITIAL_Z);
    }
}

static void
setup(mach64_t *m, int format)
{
    mach64_3d_detach(m);
    mach64_3d_attach(m);
    m->dp_pix_width = 0x60000600u | (unsigned) format;
    m->dp_src = 0x00000500u;
    m->dp_mix = 0x00070007u;
    m->write_mask = 0xffffffffu;
    m->clr_cmp_cntl = 0;
    m->dst_off_pitch = (SIZE / 8) << 22;
    m->dst_cntl = DST_X_DIR | DST_Y_DIR | TRAIL_X_DIR | TRAP_FILL_DIR;
    m->sc_left_right = (SIZE - 1) << 16;
    m->sc_top_bottom = (SIZE - 1) << 16;
    m->dst_bres_err = (uint32_t) -1;
    m->dst_bres_inc = 0;
    m->dst_bres_dec = (uint32_t) -1;
    write_reg(m, TRAIL_ERR, (uint32_t) -1);
    write_reg(m, TRAIL_INC, 0);
    write_reg(m, TRAIL_DEC, (uint32_t) -1);
    write_reg(m, Z_OFF_PITCH, ((SIZE / 8) << 22) | (Z_BASE >> 3));
    write_reg(m, Z_CNTL, Z_LEQUAL_WRITE);
    write_reg(m, SCALE_3D_CNTL, SHADE);
    /* Explicitly reprogram all live interpolators between independent cases. */
    for (unsigned a = S_X_INC2; a <= T_START; a += 4)
        write_reg(m, a, 0);
    for (unsigned a = RED_X_INC; a <= ALPHA_START; a += 4)
        write_reg(m, a, 0);
    write_reg(m, RED_START, 32u << 16);
    write_reg(m, GREEN_START, 64u << 16);
    write_reg(m, BLUE_START, 96u << 16);
    write_reg(m, ALPHA_START, 255u << 16);
    write_reg(m, Z_START, 1000u << 12);
    write_reg(m, TEX_SIZE_PITCH, 0x333);
    write_reg(m, TEX_3_OFF, TEX_BASE);
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++)
            store_pixel(m, TEX_BASE + (v * 8 + u) * 4, 4, texture_color(u, v));
    clear_surfaces(m, format);
}

static uint32_t
xy(int x, int y)
{
    return ((uint32_t) x & 0x1fffu) << 16 | ((uint32_t) y & 0x7fffu);
}

/* Coefficients are raw fixed-point register images. Include low derivative
 * bits so continuation checks also exercise hidden S/T fractional precision. */
static const int s_coeff[6] = { 0x200000, 0x400003, 0x100007,
                              0x040001, 0x020003, 0x010005 };
static const int t_coeff[6] = { 0x200000, 0x080005, 0x400009,
                              0x020001, 0x040003, 0x010007 };

static int64_t
polynomial(const int *c, int x, int y)
{
    return c[0] + (int64_t) x * c[1] + (int64_t) y * c[2] +
           (int64_t) x * (x - 1) / 2 * c[3] +
           (int64_t) y * (y - 1) / 2 * c[4] + (int64_t) x * y * c[5];
}

static void
set_texture_polynomial(mach64_t *m, unsigned base, const int *c)
{
    write_reg(m, base + 0, c[3]);
    write_reg(m, base + 4, c[4]);
    write_reg(m, base + 8, c[5]);
    write_reg(m, base + 12, c[1]);
    write_reg(m, base + 16, c[2]);
    write_reg(m, base + 20, c[0]);
}

static void
check_continuation(mach64_t *m, int ox, int oy, int dx, int dy,
                   int slope, int row)
{
    int n = slope * row;
    expect("live destination", m->dst_y_x, xy(ox + dx * n, oy + dy * row));
    expect("lead error", m->dst_bres_err, (uint32_t) -1);
    expect("trail error", read_reg(m, TRAIL_ERR), (uint32_t) -1);
    expect("red continuation", read_reg(m, RED_START), (32 + 2 * n + row) << 16);
    expect("green continuation", read_reg(m, GREEN_START), (64 + n) << 16);
    expect("blue continuation", read_reg(m, BLUE_START), (96 + 2 * row) << 16);
    expect("alpha continuation", read_reg(m, ALPHA_START), 255u << 16);
    expect("depth continuation", read_reg(m, Z_START), (1000 + 3 * n + 5 * row) << 12);
    expect("S continuation", read_reg(m, S_START),
           (uint32_t) polynomial(s_coeff, n, row) & 0x03ffffe0u);
    expect("T continuation", read_reg(m, T_START),
           (uint32_t) polynomial(t_coeff, n, row) & 0x03ffffe0u);
    expect("S X derivative", read_reg(m, S_X_INC), s_coeff[1] + n * s_coeff[3] + row * s_coeff[5]);
    expect("S Y derivative", read_reg(m, S_Y_INC), s_coeff[2] + n * s_coeff[5] + row * s_coeff[4]);
    expect("T X derivative", read_reg(m, T_X_INC), t_coeff[1] + n * t_coeff[3] + row * t_coeff[5]);
    expect("T Y derivative", read_reg(m, T_Y_INC), t_coeff[2] + n * t_coeff[5] + row * t_coeff[4]);
}

static void
triangle_case(mach64_t *m, int format, int textured, int dx, int dy,
              int clip, int slope, int additive)
{
    int ox = dx > 0 ? 8 : 32, oy = dy > 0 ? 8 : 32;
    int left = 0, right = SIZE - 1, top = 0, bottom = SIZE - 1;
    unsigned bytes = destination_bytes(format);
    snprintf(case_name, sizeof(case_name), "triangle format=%d tex=%d dir=%d,%d clip=%d slope=%d add=%d",
             format, textured, dx, dy, clip, slope, additive);
    cases++;
    setup(m, format);
    if (clip == 1) {
        left = ox + dx * (dx > 0 ? 1 : 9);
        right = ox + dx * (dx > 0 ? 9 : 1);
        top = oy + dy * (dy > 0 ? 2 : 5);
        bottom = oy + dy * (dy > 0 ? 5 : 2);
    } else if (clip == 2) {
        left = 48; /* Both halves invisible, but trajectory must still advance. */
        right = 56;
    }
    m->sc_left_right = (unsigned) left | (unsigned) right << 16;
    m->sc_top_bottom = (unsigned) top | (unsigned) bottom << 16;
    m->dst_cntl = (dx > 0 ? DST_X_DIR | TRAIL_X_DIR | TRAP_FILL_DIR : 0) |
                  (dy > 0 ? DST_Y_DIR : 0);
    m->dst_bres_inc = slope;
    write_reg(m, TRAIL_INC, slope + 2);
    write_reg(m, DST_Y_X_ALIAS, xy(ox, oy));
    write_reg(m, RED_X_INC, 2u << 16);
    write_reg(m, RED_Y_INC, 1u << 16);
    write_reg(m, GREEN_X_INC, 1u << 16);
    write_reg(m, BLUE_Y_INC, 2u << 16);
    write_reg(m, Z_X_INC, 3u << 12);
    write_reg(m, Z_Y_INC, 5u << 12);
    set_texture_polynomial(m, S_X_INC2, s_coeff);
    set_texture_polynomial(m, T_X_INC2, t_coeff);
    write_reg(m, SCALE_3D_CNTL, (textured ? TEXTURE : SHADE) |
              (additive ? (1u << 11) | (1u << 16) | (1u << 19) : 0));
    if (additive) {
        /* ONE + ONE onto black exposes double coverage at the join. Disable
         * Z so a depth rejection cannot hide a duplicate color write. */
        memset(m->svga.vram, 0, SIZE * SIZE * bytes);
        write_reg(m, Z_CNTL, 0);
    }

    /* Vertices in local coordinates: (0,0), (4*slope+8,4), (8*slope,8).
     * The leading edge continues across both four-scanline trapezoids. */
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (unsigned) ox << 16 | DRAW_TRAP | 4);
    check_continuation(m, ox, oy, dx, dy, slope, 4);
    m->dst_cntl ^= TRAIL_X_DIR;
    write_reg(m, TRAIL_INC, 2 - slope);
    write_reg(m, LEAD_ALIAS, DRAW_TRAP | 4); /* Retain the live trailing X. */
    check_continuation(m, ox, oy, dx, dy, slope, 8);

    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int r = (y - oy) * dy, n = (x - ox) * dx;
            int width = r < 4 ? 2 * r : 2 * (8 - r);
            int inside = r >= 0 && r < 8 && n >= slope * r &&
                         n < slope * r + width && x >= left && x <= right &&
                         y >= top && y <= bottom;
            uint32_t color = additive ? 0 : BACKGROUND;
            uint32_t depth = INITIAL_Z;
            char label[64];
            if (inside) {
                if (textured) {
                    int u = (int) (polynomial(s_coeff, n, r) >> 23) & 7;
                    int v = (int) (polynomial(t_coeff, n, r) >> 23) & 7;
                    color = texture_color(u, v);
                } else {
                    color = 0xff000000u | (unsigned) (32 + 2 * n + r) << 16 |
                            (unsigned) (64 + n) << 8 | (unsigned) (96 + 2 * r);
                }
                if (!additive)
                    depth = 1000 + 3 * n + 5 * r;
            }
            snprintf(label, sizeof(label), "color at %d,%d", x, y);
            expect(label, load_pixel(m, (y * SIZE + x) * bytes, bytes), pack_color(color, format));
            snprintf(label, sizeof(label), "depth at %d,%d", x, y);
            expect(label, load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
        }
    }
}

static void
check_surface_unchanged(mach64_t *m)
{
    for (unsigned i = 0; i < SIZE * SIZE; i++) {
        expect("preload leaves color untouched", load_pixel(m, i * 4, 4), BACKGROUND);
        expect("preload leaves Z untouched", load_pixel(m, Z_BASE + i * 2, 2), INITIAL_Z);
    }
}

static void
preload_case(mach64_t *m, unsigned address, unsigned bytes, int shaded, int stale)
{
    uint32_t command = LOAD_TRAIL | (12u << 16) | 2;
    uint32_t type = bytes == 4 ? FIFO_WRITE_DWORD : bytes == 2 ? FIFO_WRITE_WORD : FIFO_WRITE_BYTE;
    snprintf(case_name, sizeof(case_name), "preload alias=%03x bytes=%u shaded=%d stale=%d",
             address, bytes, shaded, stale);
    cases++;
    setup(m, 6);
    if (stale) {
        write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
        write_reg(m, LEAD_ALIAS, LOAD_TRAIL | (10u << 16) | DRAW_TRAP | 1);
        clear_surfaces(m, 6);
    }
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    write_reg(m, SCALE_3D_CNTL, shaded ? SHADE : 0);
    for (unsigned lane = 0; lane < 4; lane += bytes) {
        /* An ordinary/shared 0x120 write can be unclaimed. No legacy draw is
         * requested: the completing high byte has LINE_DIS set. */
        mach64_3d_write(m, address + lane, command >> (8 * lane), type);
        check_surface_unchanged(m);
        expect("preload does not move lead", m->dst_y_x, xy(8, 8));
    }
    write_reg(m, SCALE_3D_CNTL, SHADE);
    /* Deliberately supply a different upper field: bit 31=0/bit 15=1 must
     * retain the preloaded X=12, not load X=2 or reuse a previous X=10. */
    command = (2u << 16) | DRAW_TRAP | 2;
    for (unsigned lane = 0; lane < 4; lane += bytes) {
        unsigned other_alias = address == LEAD_ALIAS ? LEAD_LENGTH : LEAD_ALIAS;
        int claimed = mach64_3d_write(m, other_alias + lane, command >> (8 * lane), type);
        if (lane + bytes == 4)
            expect("completed trapezoid claimed", claimed, 1);
        else
            check_surface_unchanged(m);
    }
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int inside = y >= 8 && y < 10 && x >= 8 && x < 12;
            char label[64];
            snprintf(label, sizeof(label), "preloaded span at %d,%d", x, y);
            expect(label, load_pixel(m, (y * SIZE + x) * 4, 4),
                   inside ? 0xff204060u : BACKGROUND);
            expect("preloaded depth", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2),
                   inside ? 1000u : INITIAL_Z);
        }
    }
}

static void
rectangle(mach64_t *m)
{
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (12u << 16) | DRAW_TRAP | 1);
    write_reg(m, LEAD_ALIAS, DRAW_TRAP | 1);
}

static void
depth_case(mach64_t *m, int format, int enabled, int write_enabled,
           unsigned function, unsigned relation)
{
    /* Comparison truth tables for source <, ==, > destination respectively.
     * Bits name the eight documented Z_TEST functions, not renderer helpers. */
    static const unsigned passes[3] = { 0xc6, 0x9c, 0xf0 };
    unsigned source_z = 999 + relation;
    unsigned bytes = destination_bytes(format);
    int passes_test = !enabled || ((passes[relation] >> function) & 1u);
    snprintf(case_name, sizeof(case_name), "depth format=%d enabled=%d write=%d fn=%u relation=%u",
             format, enabled, write_enabled, function, relation);
    cases++;
    setup(m, format);
    write_reg(m, Z_CNTL, (unsigned) enabled | (function << 4) | ((unsigned) write_enabled << 8));
    write_reg(m, Z_START, source_z << 12);
    for (int y = 8; y < 10; y++)
        for (int x = 8; x < 12; x++)
            store_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2, 1000);
    rectangle(m);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int inside = y >= 8 && y < 10 && x >= 8 && x < 12;
            uint32_t color = inside && passes_test ? 0xff204060u : BACKGROUND;
            unsigned depth = inside ? 1000 : INITIAL_Z;
            if (inside && passes_test && enabled && write_enabled)
                depth = source_z;
            expect("depth-gated color", load_pixel(m, (y * SIZE + x) * bytes, bytes), pack_color(color, format));
            expect("depth write enable", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
        }
    }
}

static void
visibility_case(mach64_t *m, int format, int mode)
{
    unsigned bytes = destination_bytes(format);
    unsigned control = mode == 2 ? SHADE : TEXTURE;
    snprintf(case_name, sizeof(case_name), "visibility format=%d mode=%d", format, mode);
    cases++;
    setup(m, format);
    write_reg(m, S_X_INC, 1u << 23); /* One texel per pixel in an 8-wide map. */
    for (int u = 0; u < 4; u++) {
        uint32_t color = (u & 1) ? 0xff20c040u : 0xffff00ffu;
        if (mode == 1 && !(u & 1))
            color &= 0x00ffffffu; /* Alpha LSB clear: inhibit color and Z. */
        store_pixel(m, TEX_BASE + u * 4, 4, color);
    }
    if (mode == 0) {
        m->clr_cmp_cntl = 0x02000005u; /* Equal texel RGB inhibits. */
        m->clr_cmp_clr = 0x00ff00ffu;
        m->clr_cmp_mask = 0x00ffffffu;
    } else if (mode == 1) {
        control |= (1u << 30) | (1u << 28); /* Texture alpha + alpha mask. */
    } else {
        m->clr_cmp_cntl = 5; /* Equal packed destination inhibits. */
        m->clr_cmp_clr = pack_color(BACKGROUND, format);
        m->clr_cmp_mask = bytes == 2 ? 0xffffu : 0xffffffffu;
        for (int y = 8; y < 10; y++)
            for (int x = 9; x < 12; x += 2)
                store_pixel(m, (y * SIZE + x) * bytes, bytes, 0);
    }
    write_reg(m, SCALE_3D_CNTL, control);
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            /* Disable inhibition without resetting the engine or its VRAM. */
            m->clr_cmp_cntl = 0;
            write_reg(m, SCALE_3D_CNTL, mode == 2 ? SHADE : TEXTURE);
            write_reg(m, Z_START, 900u << 12);
            write_reg(m, S_START, 0);
        }
        rectangle(m);
        for (int y = 0; y < SIZE; y++) {
            for (int x = 0; x < SIZE; x++) {
                int inside = y >= 8 && y < 10 && x >= 8 && x < 12;
                int visible = inside && (pass || (x & 1));
                uint32_t color = BACKGROUND;
                unsigned depth = INITIAL_Z;
                if (visible) {
                    color = mode == 2 ? 0xff204060u :
                            (x & 1) ? 0xff20c040u : 0xffff00ffu;
                    depth = pass ? 900 : 1000;
                }
                expect("visibility color", load_pixel(m, (y * SIZE + x) * bytes, bytes), pack_color(color, format));
                expect("inhibited texel must not occlude later geometry",
                       load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
            }
        }
    }
}

static void
depth_field_case(mach64_t *m, int format, unsigned function, int writes, int dir, int split)
{
    static const unsigned passes[3] = { 0xc6, 0x9c, 0xf0 };
    const unsigned stored_z = 48928; /* Equal to column 3 on the first row. */
    int ox = dir > 0 ? 8 : 24, oy = dir > 0 ? 8 : 24;
    int left = ox + dir * (dir > 0 ? 2 : 6), right = ox + dir * (dir > 0 ? 6 : 2);
    unsigned bytes = destination_bytes(format);
    snprintf(case_name, sizeof(case_name), "depth field format=%d fn=%u write=%d dir=%d split=%d",
             format, function, writes, dir, split);
    cases++;
    setup(m, format);
    m->dst_cntl = dir > 0 ? DST_X_DIR | DST_Y_DIR | TRAIL_X_DIR | TRAP_FILL_DIR : 0;
    m->sc_left_right = (unsigned) left | (unsigned) right << 16;
    write_reg(m, Z_CNTL, 1u | function << 4 | (unsigned) writes << 8);
    write_reg(m, Z_START, 30000u << 12);
    write_reg(m, Z_X_INC, 50000u << 12);
    write_reg(m, Z_Y_INC, (uint32_t) (-40000 * 4096));
    for (unsigned i = 0; i < SIZE * SIZE; i++)
        store_pixel(m, Z_BASE + i * 2, 2, stored_z);
    write_reg(m, DST_Y_X_ALIAS, xy(ox, oy));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (unsigned) (ox + dir * 8) << 16 |
              DRAW_TRAP | (split ? 2 : 4));
    if (split)
        write_reg(m, LEAD_ALIAS, DRAW_TRAP | 2);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int row = (y - oy) * dir, column = (x - ox) * dir;
            int inside = row >= 0 && row < 4 && column >= 2 && column <= 6;
            unsigned depth = stored_z;
            uint32_t color = BACKGROUND;
            if (inside) {
                int physical = (30000 + column * 50000 - row * 40000) % 131072;
                if (physical < 0)
                    physical += 131072;
                unsigned incoming = physical < 65536 ? (unsigned) physical : 0;
                unsigned relation = incoming < stored_z ? 0 : incoming == stored_z ? 1 : 2;
                if ((passes[relation] >> function) & 1u) {
                    color = 0xff204060u;
                    if (writes)
                        depth = incoming;
                }
            }
            expect("physical Z compare gates color", load_pixel(m, (y * SIZE + x) * bytes, bytes),
                   pack_color(color, format));
            expect("physical Z conditional write", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
        }
    }
    expect("physical Z continuation", read_reg(m, Z_START), (uint32_t) (-130000 * 4096) & 0x1fffffffu);
}

/* Scissors are inclusive signed 13/15-bit bounds. Empty rectangles inhibit
 * writes but must still allow the command's live trajectory to advance. */
static void
scissor_case(mach64_t *m, int mode, int line)
{
    static const int bounds[][4] = {
        { 12, 8, 0, 63 }, { 0, 63, 10, 8 },
        { 0, -1, 0, 63 }, { 0, 63, 0, -1 },
        { -4, 9, 0, 63 }, { 0, 63, -4, 8 },
        { 9, 9, 8, 8 }, { 70, 75, 0, 63 },
        { 0, 63, 0, 63 }
    };
    int left = bounds[mode][0], right = bounds[mode][1];
    int top = bounds[mode][2], bottom = bounds[mode][3];
    snprintf(case_name, sizeof(case_name), "signed/empty scissor mode=%d line=%d", mode, line);
    cases++;
    setup(m, 6);
    m->sc_left_right = ((uint32_t) left & 0x1fffu) | ((uint32_t) right & 0x1fffu) << 16;
    m->sc_top_bottom = ((uint32_t) top & 0x7fffu) | ((uint32_t) bottom & 0x7fffu) << 16;
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    unsigned expected_status = (8 < left ? 0x10u : 0) | (8 > right ? 0x20u : 0) |
                               (8 < top ? 0x40u : 0) | (8 > bottom ? 0x80u : 0);
    expect("signed GUI scissor comparisons", read_reg(m, 0x338) & 0xf0u, expected_status);
    if (line == 1) {
        m->dst_cntl |= DST_LAST_PEL;
        write_reg(m, LEAD_LENGTH, 4);
        expect("clipped line still advances", m->dst_y_x, xy(11, 8));
    } else if (line == 2) {
        store_pixel(m, TEX_BASE, 4, 0xff204060u);
        write_reg(m, 0x1c0, TEX_BASE);
        write_reg(m, 0x1dc, 1);
        write_reg(m, 0x1e0, 1);
        write_reg(m, 0x1ec, 1);
        write_reg(m, 0x1f0, 1u << 16);
        write_reg(m, 0x1f4, 1u << 16);
        write_reg(m, 0x1f8, 0);
        write_reg(m, 0x3c8, 0);
        write_reg(m, SCALE_3D_CNTL, 0x140);
        write_reg(m, 0x118, (4u << 16) | 2);
        expect("clipped scaler still advances", read_reg(m, 0x1f8), 2u << 16);
    } else {
        write_reg(m, RED_Y_INC, 1u << 16);
        write_reg(m, Z_Y_INC, 1u << 12);
        rectangle(m);
        expect("clipped trapezoid still advances", m->dst_y_x, xy(8, 10));
        expect("clipped color still advances", read_reg(m, RED_START), 34u << 16);
        expect("clipped Z still advances", read_reg(m, Z_START), 1002u << 12);
    }
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int inside = x >= 8 && x < 12 && y >= 8 && y < (line == 1 ? 9 : 10) &&
                         x >= left && x <= right && y >= top && y <= bottom;
            unsigned red = 32 + (line ? 0 : y - 8);
            uint32_t color = inside ? 0xff004060u | red << 16 : BACKGROUND;
            expect("scissor write inhibition", load_pixel(m, (y * SIZE + x) * 4, 4), color);
            expect("scissor Z inhibition", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2),
                   inside && line != 2 ? 1000u + (unsigned) (line ? 0 : y - 8) : INITIAL_Z);
        }
    }
}

/* Apply delayed source/polygon state at the FIFO barrier. Classification must
 * use this state, not the stale state preceding a 2D/3D transition. */
static void
line_dispatch_case(mach64_t *m, int mode, unsigned alias)
{
    snprintf(case_name, sizeof(case_name), "ordered line dispatch mode=%d alias=%03x", mode, alias);
    cases++;
    setup(m, 6);
    m->dst_cntl |= DST_LAST_PEL;
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    if (mode == 1)
        m->dp_src = 0x00000100u;
    if (mode == 2)
        m->dst_cntl |= DST_POLYGON_EN;
    fifo_shared_source = mode == 0 ? 0x00000100u : 0x00000500u;
    fifo_shared_cntl = mode == 3 ? m->dst_cntl | DST_POLYGON_EN : m->dst_cntl & ~DST_POLYGON_EN;
    fifo_shared_update = 1;
    expect("source change is queued", mach64_3d_write(m, 0x2d8, fifo_shared_source, FIFO_WRITE_DWORD), 0);
    expect("polygon change is queued", mach64_3d_write(m, 0x130, fifo_shared_cntl, FIFO_WRITE_DWORD), 0);
    int shaded = mode == 1 || mode == 2;
    unsigned result = mach64_3d_write(m, alias, 4, FIFO_WRITE_DWORD);
    /* Alias 0x144 belongs to the 3D front end even for non-drawing state. */
    expect("dispatch uses ordered shared state", result, shaded || alias == LEAD_ALIAS);
    expect("pending state was consumed before dispatch", fifo_shared_update, 0);
    expect("line dispatch pixel", load_pixel(m, (8 * SIZE + 8) * 4, 4),
           shaded ? 0xff204060u : BACKGROUND);
    expect("line dispatch depth", load_pixel(m, Z_BASE + (8 * SIZE + 8) * 2, 2),
           shaded ? 1000 : INITIAL_Z);
    fifo_shared_update = 0;
}

/* An additive black texel or a fully transparent source must not accumulate
 * RGB noise in the destination. Exercise every table phase and framebuffer
 * component level, including partial-channel (red-only) contributions. */
static void
ordered_blend_identity_case(mach64_t *m, int format, int mode)
{
    uint32_t rgb_mask=format==3?0x7fffu:0xffffu;
    uint32_t red_mask=format==3?0x7c00u:0xf800u;
    unsigned levels=format==3?32:64;
    snprintf(case_name,sizeof(case_name),"ordered blend identity format=%d mode=%d",format,mode);
    cases++;
    setup(m,format);
    write_reg(m,Z_CNTL,0);
    write_reg(m,SCALE_3D_CNTL,SHADE|6u|(1u<<11)|
              ((mode==1?4u:1u)<<16)|((mode==1?5u:1u)<<19));
    write_reg(m,RED_START,(mode?255u:0u)<<16);
    write_reg(m,GREEN_START,(mode==1?255u:0u)<<16);
    write_reg(m,BLUE_START,(mode==1?255u:0u)<<16);
    write_reg(m,ALPHA_START,0);
    for(unsigned level=0;level<levels;level++) {
        uint32_t background=format==3?
            ((level<<10)|((31-level)<<5)|((level*7)&31)):
            (((level&31)<<11)|((63-level)<<5)|((level*7)&31));
        for(int y=8;y<12;y++)for(int x=8;x<12;x++)
            store_pixel(m,(y*SIZE+x)*2,2,background);
        for(int pass=0;pass<4;pass++) {
            write_reg(m,DST_Y_X_ALIAS,xy(8,8));
            write_reg(m,LEAD_LENGTH,LOAD_TRAIL|(12u<<16)|DRAW_TRAP|4);
            for(int y=8;y<12;y++)for(int x=8;x<12;x++)
                expect("unchanged blend components",load_pixel(m,(y*SIZE+x)*2,2)&rgb_mask,
                       mode==2?(background|red_mask):background);
        }
    }
}

static void
blend_transition_case(mach64_t *m, int format)
{
    static const uint32_t source[3] = { 0xff102030u, 0xff203040u, 0xff406080u };
    unsigned bytes = destination_bytes(format);
    snprintf(case_name, sizeof(case_name), "opaque/additive/opaque format=%d", format);
    cases++;
    setup(m, format);
    for (int pass = 0; pass < 3; pass++) {
        write_reg(m, RED_START, ((source[pass] >> 16) & 255u) << 16);
        write_reg(m, GREEN_START, ((source[pass] >> 8) & 255u) << 16);
        write_reg(m, BLUE_START, (source[pass] & 255u) << 16);
        write_reg(m, SCALE_3D_CNTL, SHADE |
                  (pass == 1 ? (1u << 11) | (1u << 16) | (1u << 19) : 0));
        write_reg(m, Z_CNTL, pass == 1 ? 0x21 : Z_LEQUAL_WRITE);
        write_reg(m, Z_START, (pass == 2 ? 500u : 1000u) << 12);
        rectangle(m);
        for (int y = 0; y < SIZE; y++) {
            for (int x = 0; x < SIZE; x++) {
                int inside = y >= 8 && y < 10 && x >= 8 && x < 12;
                /* For RGB565 the first pass expands to (16,32,49), so
                 * the additive blue sum is 113, which still packs to 112. */
                uint32_t color = inside ? (pass == 1 ? 0xff305070u : source[pass]) : BACKGROUND;
                unsigned depth = inside ? (pass == 2 ? 500 : 1000) : INITIAL_Z;
                expect("blend transition color", load_pixel(m, (y * SIZE + x) * bytes, bytes), pack_color(color, format));
                expect("blend transition depth", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
            }
        }
    }
}

/* Exercise color-field crossings through the actual fragment consumers, not
 * only a conversion helper. White texels isolate modulation from sampling;
 * the blend case uses the renderer's existing SRC_ALPHA/ZERO contract. */
static void
color_field_case(mach64_t *m, int format, int mode, int dx, int dy, int clip, int split)
{
    static const unsigned registers[] = { RED_X_INC, GREEN_X_INC, BLUE_X_INC, ALPHA_X_INC };
    static const int start[] = { 220, 100, 10, 128 };
    static const int x_inc[] = { 100, -100, 200, 200 };
    static const int y_inc[] = { -192, 160, -64, 100 };
    int ox = dx > 0 ? 8 : 24, oy = dy > 0 ? 8 : 24;
    int left = 0, right = SIZE - 1;
    unsigned bytes = destination_bytes(format);

    snprintf(case_name, sizeof(case_name), "color field format=%d mode=%d dir=%d,%d clip=%d split=%d",
             format, mode, dx, dy, clip, split);
    cases++;
    setup(m, format);
    m->dst_cntl = (dx > 0 ? DST_X_DIR | TRAIL_X_DIR | TRAP_FILL_DIR : 0) |
                  (dy > 0 ? DST_Y_DIR : 0);
    if (clip) {
        left = ox + dx * (dx > 0 ? 2 : 6);
        right = ox + dx * (dx > 0 ? 6 : 2);
        m->sc_left_right = (unsigned) left | (unsigned) right << 16;
    }
    write_reg(m, SCALE_3D_CNTL, (mode ? TEXTURE | (1u << 22) : SHADE) |
              (mode == 2 ? (1u << 11) | (4u << 16) : 0));
    for (unsigned i = 0; i < 64; i++)
        store_pixel(m, TEX_BASE + i * 4, 4, 0xffffffffu);
    for (unsigned c = 0; c < 4; c++) {
        write_reg(m, registers[c], (uint32_t) (x_inc[c] * 65536));
        write_reg(m, registers[c] + 4, (uint32_t) (y_inc[c] * 65536));
        write_reg(m, registers[c] + 8, (uint32_t) (start[c] * 65536));
    }
    write_reg(m, DST_Y_X_ALIAS, xy(ox, oy));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (unsigned) (ox + dx * 8) << 16 |
              DRAW_TRAP | (split ? 2 : 4));
    if (split)
        write_reg(m, LEAD_ALIAS, DRAW_TRAP | 2);

    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int row = (y - oy) * dy, column = (x - ox) * dx;
            int inside = row >= 0 && row < 4 && column >= 0 && column < 8 &&
                         x >= left && x <= right;
            uint32_t color = BACKGROUND;
            if (inside) {
                unsigned component[4];
                for (unsigned c = 0; c < 4; c++) {
                    int value = (start[c] + column * x_inc[c] + row * y_inc[c]) % 512;
                    if (value < 0)
                        value += 512;
                    component[c] = value < 256 ? (unsigned) value : 0;
                }
                if (mode == 2) {
                    unsigned alpha = component[3];
                    for (unsigned c = 0; c < 4; c++)
                        component[c] = (component[c] * alpha + 127) / 255;
                }
                color = component[3] << 24 | component[0] << 16 |
                        component[1] << 8 | component[2];
            }
            expect("color-field pipeline", load_pixel(m, (y * SIZE + x) * bytes, bytes),
                   pack_color(color, format));
            expect("color-field depth unchanged", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2),
                   inside ? 1000 : INITIAL_Z);
        }
    }
    for (unsigned c = 0; c < 4; c++)
        expect("color-field START readback", read_reg(m, registers[c] + 8),
               (uint32_t) ((start[c] + 4 * y_inc[c]) * 65536) & 0x01fffff0u);
}

static void
hidden_fraction_case(mach64_t *m)
{
    snprintf(case_name, sizeof(case_name), "hidden S fraction across split and readback");
    cases++;
    setup(m, 6);
    write_reg(m, SCALE_3D_CNTL, TEXTURE);
    write_reg(m, S_START, (1u << 23) - 32);
    write_reg(m, S_Y_INC, 7);
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (12u << 16) | DRAW_TRAP | 4);
    expect("readback excludes hidden fraction", read_reg(m, S_START), (1u << 23) - 32);
    write_reg(m, LEAD_ALIAS, DRAW_TRAP | 4);
    for (int row = 0; row < 8; row++)
        for (int x = 8; x < 12; x++)
            expect("fraction crosses texel boundary after split",
                   load_pixel(m, ((row + 8) * SIZE + x) * 4, 4), texture_color(row >= 5, 0));
    /* A new START write must replace, not retain, the internal accumulator. */
    write_reg(m, S_START, 0);
    rectangle(m);
    expect("guest START replaces hidden state", load_pixel(m, (8 * SIZE + 8) * 4, 4), texture_color(0, 0));
}

static void
line_to_triangle_case(mach64_t *m, unsigned address, int last_pixel)
{
    snprintf(case_name, sizeof(case_name), "shaded line loads trailing X alias=%03x last=%d", address, last_pixel);
    cases++;
    setup(m, 6);
    if (last_pixel)
        m->dst_cntl |= DST_LAST_PEL;
    write_reg(m, DST_Y_X_ALIAS, xy(2, 2));
    write_reg(m, address, (12u << 16) | 2); /* Bit 31=0, bit 15=0. */
    expect("shaded line still draws", load_pixel(m, (2 * SIZE + 2) * 4, 4), 0xff204060u);
    expect("last-pixel selection preserved", load_pixel(m, (2 * SIZE + 3) * 4, 4),
           last_pixel ? 0xff204060u : BACKGROUND);
    expect("line endpoint preserved", m->dst_y_x, xy(3, 2));
    clear_surfaces(m, 6);
    write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
    write_reg(m, LEAD_ALIAS, (2u << 16) | DRAW_TRAP | 1);
    expect("triangle retains X loaded by line", load_pixel(m, (8 * SIZE + 11) * 4, 4), 0xff204060u);
    expect("trailing edge excluded", load_pixel(m, (8 * SIZE + 12) * 4, 4), BACKGROUND);
}

static void
legacy_barrier_case(mach64_t *m)
{
    unsigned before;
    snprintf(case_name, sizeof(case_name), "ordinary 2D commands do not add FIFO waits");
    cases++;
    setup(m, 6);
    write_reg(m, SCALE_3D_CNTL, 0);
    before = fifo_waits;
    /* Model a shared-register fallback notification, not a running FIFO. */
    expect("shared write falls through", mach64_3d_write(m, 0x2d8, m->dp_src, FIFO_WRITE_DWORD), 0);
    for (unsigned i = 0; i < 16; i++)
        expect("ordinary line remains legacy", mach64_3d_write(m, LEAD_LENGTH,
                LOAD_TRAIL | (i << 16), FIFO_WRITE_DWORD), 0);
    expect("no extra waits for ordinary line state", fifo_waits, before);
    write_reg(m, Z_CNTL, Z_LEQUAL_WRITE);
    expect("existing 2D-to-3D barrier retained", fifo_waits, before + 1);
    write_reg(m, Z_CNTL, Z_LEQUAL_WRITE);
    expect("clean 3D state does not wait again", fifo_waits, before + 1);
}

static void
pixie_profile_case(mach64_t *m, int textured, int dx, int dy, int slope, int clip)
{
    int ox = dx > 0 ? 8 : 32, oy = dy > 0 ? 8 : 32;
    int left = 0, right = SIZE - 1, top = 0, bottom = SIZE - 1;
    snprintf(case_name, sizeof(case_name), "RGB555 recorded control tex=%d dir=%d,%d slope=%d clip=%d",
             textured, dx, dy, slope, clip);
    cases++;
    setup(m, 3);
    /* These mode words occur in the saved Pixie captures. Geometry, colors,
     * and maps are deliberately synthetic, not a replay of the failing scene. */
    m->dp_pix_width = 0x30030203u;
    m->dp_src = 0x00000503u;
    m->dp_mix = 0x00070003u;
    write_reg(m, SCALE_3D_CNTL, textured ? 0x06410287u : 0x064102c7u);
    write_reg(m, TEX_SIZE_PITCH, 0x777);
    for (unsigned level = 0; level <= 7; level++) {
        unsigned base = TEX_BASE + (level << 16);
        unsigned side = 1u << level;
        write_reg(m, 0x1c0 + level * 4, base);
        for (unsigned i = 0; i < side * side; i++)
            store_pixel(m, base + i * 2, 2, 0xffff);
    }
    /* White at every mip level makes the expected texture-modulated color
     * independent of undocumented fractional LOD and filter rounding. */
    set_texture_polynomial(m, S_X_INC2, s_coeff);
    set_texture_polynomial(m, T_X_INC2, t_coeff);
    write_reg(m, RED_START, 200u << 16);
    write_reg(m, GREEN_START, 180u << 16);
    write_reg(m, BLUE_START, 160u << 16);
    write_reg(m, RED_X_INC, (uint32_t) (-7 * 65536));
    write_reg(m, RED_Y_INC, (uint32_t) (-9 * 65536));
    write_reg(m, GREEN_X_INC, (uint32_t) (-5 * 65536));
    write_reg(m, GREEN_Y_INC, (uint32_t) (-3 * 65536));
    write_reg(m, BLUE_X_INC, (uint32_t) (-2 * 65536));
    write_reg(m, BLUE_Y_INC, 4u << 16);
    write_reg(m, Z_X_INC, (uint32_t) (-3 * 4096));
    write_reg(m, Z_Y_INC, 5u << 12);
    if (clip) {
        left = ox + dx * (dx > 0 ? 1 : 9);
        right = ox + dx * (dx > 0 ? 9 : 1);
        top = oy + dy * (dy > 0 ? 2 : 5);
        bottom = oy + dy * (dy > 0 ? 5 : 2);
    }
    m->sc_left_right = (unsigned) left | (unsigned) right << 16;
    m->sc_top_bottom = (unsigned) top | (unsigned) bottom << 16;
    m->dst_cntl = (dx > 0 ? DST_X_DIR | TRAIL_X_DIR | TRAP_FILL_DIR : 0) |
                  (dy > 0 ? DST_Y_DIR : 0);
    m->dst_bres_inc = slope;
    write_reg(m, TRAIL_INC, slope + 2);
    write_reg(m, DST_Y_X_ALIAS, xy(ox, oy));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (unsigned) ox << 16 | DRAW_TRAP | 4);
    m->dst_cntl ^= TRAIL_X_DIR;
    write_reg(m, TRAIL_INC, 2 - slope);
    write_reg(m, LEAD_ALIAS, DRAW_TRAP | 4);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int r = (y - oy) * dy, n = (x - ox) * dx;
            int width = r < 4 ? 2 * r : 2 * (8 - r);
            int inside = r >= 0 && r < 8 && n >= slope * r &&
                         n < slope * r + width && x >= left && x <= right &&
                         y >= top && y <= bottom;
            uint32_t pixel = load_pixel(m, (y * SIZE + x) * 2, 2);
            unsigned depth = INITIAL_Z;
            if (inside) {
                int channels[3] = { 200 - 7 * n - 9 * r, 180 - 5 * n - 3 * r,
                                    160 - 2 * n + 4 * r };
                expect("opaque RGB555 alpha", pixel >> 15, 1);
                for (int channel = 0; channel < 3; channel++) {
                    int quantized = (pixel >> (10 - 5 * channel)) & 31;
                    int lower = channels[channel] >> 3;
                    /* Accept either neighboring quantization code; do not
                     * bless the current substituted Bayer table as hardware. */
                    expect("lit component within quantization bounds",
                           quantized >= lower && quantized <= lower + 1, 1);
                }
                depth = 1000 - 3 * n + 5 * r;
            } else {
                expect("RGB555 profile coverage", pixel, pack_color(BACKGROUND, 3));
            }
            expect("RGB555 profile depth", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), depth);
        }
    }
}

#include "mach64_3d_driver_fixture.h"

static void
initial_edge_texture_case(mach64_t *m, int dx, int dy, int zero_negative,
                          int split)
{
    int ox = dx > 0 ? 8 : 48, oy = dy > 0 ? 8 : 48;
    int leading_steps = zero_negative ? 2 : 3;
    int trailing_steps = zero_negative ? 1 : 2;
    snprintf(case_name, sizeof(case_name), "initial edges texture dx=%d dy=%d zero=%d split=%d",
             dx, dy, zero_negative, split);
    cases++;
    setup(m, 6);
    write_reg(m, SCALE_3D_CNTL, TEXTURE);
    m->dst_cntl = (dx > 0 ? DST_X_DIR | TRAIL_X_DIR | TRAP_FILL_DIR : 0) |
                  (dy > 0 ? DST_Y_DIR : 0) | (zero_negative ? (1u << 11) | (1u << 15) : 0);
    m->dst_bres_err = 4;
    m->dst_bres_inc = 0;
    m->dst_bres_dec = (uint32_t) -2;
    write_reg(m, TRAIL_ERR, 2);
    write_reg(m, TRAIL_INC, 0);
    write_reg(m, TRAIL_DEC, (uint32_t) -2);
    write_reg(m, S_START, 0);
    write_reg(m, S_X_INC, 1u << 21);
    write_reg(m, S_Y_INC, 1u << 21);
    write_reg(m, S_X_INC2, 1u << 20);
    write_reg(m, S_Y_INC2, 1u << 19);
    write_reg(m, S_XY_INC2, 1u << 19);
    write_reg(m, T_START, (1u << 23) - 32);
    write_reg(m, T_X_INC, 7);
    write_reg(m, T_Y_INC, 5);
    write_reg(m, T_X_INC2, 3);
    write_reg(m, T_Y_INC2, 2);
    write_reg(m, T_XY_INC2, 1);
    write_reg(m, Z_X_INC, (uint32_t) (-3 * 4096));
    write_reg(m, Z_Y_INC, 5u << 12);
    write_reg(m, DST_Y_X_ALIAS, xy(ox, oy));
    write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (unsigned) (ox + dx * 4) << 16 |
                               DRAW_TRAP | (split ? 2 : 4));
    if (split) {
        (void) read_reg(m, T_START); /* Reading must not discard internal bits. */
        write_reg(m, LEAD_ALIAS, DRAW_TRAP | 2);
    }
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int row = (y - oy) * dy, n = (x - ox) * dx;
            int inside = row >= 0 && row < 4 && n >= leading_steps && n < 4 + trailing_steps;
            uint32_t expected = BACKGROUND;
            unsigned z = INITIAL_Z;
            if (inside) {
                int64_t s = ((int64_t) n + row) * (1 << 21) +
                            (int64_t) n * (n - 1) / 2 * (1 << 20) +
                            ((int64_t) row * (row - 1) / 2 + n * row) * (1 << 19);
                int64_t t = (1 << 23) - 32 + 7 * n + 5 * row +
                            3 * n * (n - 1) / 2 + row * (row - 1) + n * row;
                expected = texture_color((int) (s >> 23) & 7, (int) (t >> 23) & 7);
                z = 1000 - 3 * n + 5 * row;
            }
            expect("initial edge texture and coverage", load_pixel(m, (y * SIZE + x) * 4, 4), expected);
            expect("initial edge depth", load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2), z);
        }
    }
    expect("initial X steps do not advance Y", m->dst_y_x, xy(ox + dx * leading_steps, oy + dy * 4));
}

/* Check actual ATI-generated setup against input-vertex planes. No driver DLL
 * or x86 interpreter is needed when running this regression. Stay strictly
 * inside the triangle so this does not impose an unverified edge-tie rule. */
static void
driver_triangle_case(mach64_t *m, unsigned fixture, int format)
{
    const double (*v)[6] = ati_driver_triangles[fixture].vertex;
    double area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) -
                  (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]);
    unsigned interior = 0;
    unsigned bytes = destination_bytes(format);

    snprintf(case_name, sizeof(case_name), "ATI setup fixture=%u format=%d", fixture, format);
    cases++;
    setup(m, format);
    m->dp_mix = 0x00070003;
    for (unsigned i = 0; i < ati_driver_triangles[fixture].command_count; i++) {
        uint32_t address = ati_driver_triangles[fixture].command[i][0];
        uint32_t value = ati_driver_triangles[fixture].command[i][1];
        switch (address) {
            case 0x124: m->dst_bres_err = value; break;
            case 0x128: m->dst_bres_inc = value; break;
            case 0x12c: m->dst_bres_dec = value; break;
            case 0x130: m->dst_cntl = value; break;
            case 0x194: m->dp_src = value; break;
            default: write_reg(m, address, value); break;
        }
    }
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            double px = x + 0.5 - v[0][0], py = y + 0.5 - v[0][1];
            double w1 = (px * (v[2][1] - v[0][1]) -
                         (v[2][0] - v[0][0]) * py) / area;
            double w2 = ((v[1][0] - v[0][0]) * py -
                         px * (v[1][1] - v[0][1])) / area;
            double w0 = 1.0 - w1 - w2;
            if (w0 < 0.01 || w1 < 0.01 || w2 < 0.01)
                continue;
            interior++;
            uint32_t pixel = load_pixel(m, (y * SIZE + x) * bytes, bytes);
            expect("ATI triangle interior written", pixel != pack_color(BACKGROUND, format), 1);
            for (unsigned channel = 0; channel < 3; channel++) {
                double expected = w0 * v[0][3 + channel] +
                                  w1 * v[1][3 + channel] + w2 * v[2][3 + channel];
                unsigned shift = format == 6 ? 16 - 8 * channel :
                                 (format == 3 ? 10 - 5 * channel :
                                  (channel == 0 ? 11 : (channel == 1 ? 5 : 0)));
                unsigned bits = format == 6 ? 8 : (format == 4 && channel == 1 ? 6 : 5);
                unsigned actual = (pixel >> shift) & ((1u << bits) - 1);
                double error = format == 6 ? (double) actual - expected :
                               (double) actual - ((unsigned) expected >> (8 - bits));
                expect("ATI vertex-plane color", error > -1.1 && error < 1.1, 1);
            }
            double expected_z = w0 * v[0][2] + w1 * v[1][2] + w2 * v[2][2];
            double error_z = load_pixel(m, Z_BASE + (y * SIZE + x) * 2, 2) - expected_z;
            expect("ATI vertex-plane depth", error_z > -1.1 && error_z < 1.1, 1);
        }
    }
    expect("ATI fixture has interior samples", interior > 0, 1);
}

/* One white texel among black ones exposes both the 8-bit binary fraction
 * (not an endpoint-inclusive /255 alpha weight) and the 2x2 filter's texel
 * centres, half a texel past each integer coordinate.  The white texel is
 * fully weighted only at its centre and half weighted at either integer
 * edge.  Exercise both axes, the wrap boundary, and the command path. */
static void
bilinear_fraction_case(mach64_t *m, int axis, int wrap)
{
    snprintf(case_name, sizeof(case_name), "bilinear fraction axis=%d wrap=%d", axis, wrap);
    cases++;
    setup(m, 6);
    write_reg(m, Z_CNTL, 0);
    write_reg(m, SCALE_3D_CNTL, TEXTURE | (1u << 25));
    for (int v=0;v<8;v++)
        for (int u=0;u<8;u++) {
            unsigned level=(axis?v:u)==(wrap?7:0)?255:0;
            store_pixel(m,TEX_BASE+(v*8+u)*4,4,0xff000000u|level*0x010101u);
        }
    for (unsigned fraction=0;fraction<256;fraction++) {
        /* 8x8 map: one texel is 2^23 in the normalized S/T domain. */
        write_reg(m,axis?T_START:S_START,((wrap?7u:0u)<<23)|(fraction<<15));
        write_reg(m,DST_Y_X_ALIAS,xy(8,8));
        write_reg(m,LEAD_LENGTH,LOAD_TRAIL|(9u<<16)|DRAW_TRAP|1);
        /* Weight of the white texel at distance |fraction-128|/256 from its centre. */
        unsigned weight=fraction<128?fraction+128:384-fraction;
        unsigned expected=(255u*weight+128)/256;
        expect("binary texture fraction",load_pixel(m,(8*SIZE+8)*4,4),
               0xff000000u|expected*0x010101u);
    }
}

/* Final Reality's neon entrance bevel samples T=0 of a map whose only bright
 * row is the last one.  With texel-centre weighting its rim blends the last
 * and first rows equally; at the first row's centre only that row remains.
 * Nearest sampling selects the first row throughout. */
static void
bilinear_wrap_edge_case(mach64_t *m, int bilinear)
{
    static const unsigned offsets[] = { 0, 1u << 21, 1u << 22 };
    static const unsigned bilinear_levels[] = { 128, 64, 0 };

    snprintf(case_name, sizeof(case_name), "texture wrap edge bilinear=%d", bilinear);
    cases++;
    setup(m, 6);
    write_reg(m, Z_CNTL, 0);
    write_reg(m, SCALE_3D_CNTL, TEXTURE | (bilinear ? 1u << 25 : 0));
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++)
            store_pixel(m, TEX_BASE + (v * 8 + u) * 4, 4,
                        0xff000000u | (v == 7 ? 0xff2020u : 0u));
    for (unsigned i = 0; i < 3; i++) {
        /* T=0, a quarter texel, then half a texel (the first row's centre). */
        write_reg(m, T_START, offsets[i]);
        write_reg(m, DST_Y_X_ALIAS, xy(8, 8));
        write_reg(m, LEAD_LENGTH, LOAD_TRAIL | (9u << 16) | DRAW_TRAP | 1);
        unsigned level = bilinear ? bilinear_levels[i] : 0;
        unsigned red = (255u * level + 128) / 256, other = (0x20u * level + 128) / 256;
        expect("wrapped edge row weight", load_pixel(m, (8 * SIZE + 8) * 4, 4),
               0xff000000u | red << 16 | other << 8 | other);
    }
}

int
main(void)
{
    static const int formats[] = { 3, 4, 6 };
    mach64_t *m = create_machine();
    for(int axis=0;axis<2;axis++)
        for(int wrap=0;wrap<2;wrap++)bilinear_fraction_case(m,axis,wrap);
    for (int bilinear = 0; bilinear <= 1; bilinear++)
        bilinear_wrap_edge_case(m, bilinear);
    for (unsigned fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
        int format = formats[fi];
        for (int textured = 0; textured <= 1; textured++)
            for (int dx = -1; dx <= 1; dx += 2)
                for (int dy = -1; dy <= 1; dy += 2)
                    for (int clip = 0; clip < 3; clip++)
                        for (int slope = 0; slope <= 1; slope++)
                            for (int additive = 0; additive <= 1; additive++)
                                triangle_case(m, format, textured, dx, dy, clip, slope, additive);
    }
    for (unsigned alias = 0; alias < 2; alias++)
        for (unsigned bytes = 1; bytes <= 4; bytes *= 2)
            for (int shaded = 0; shaded <= 1; shaded++)
                for (int stale = 0; stale <= 1; stale++)
                    preload_case(m, alias ? LEAD_ALIAS : LEAD_LENGTH, bytes, shaded, stale);
    for (unsigned fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
        int format = formats[fi];
        for (int enabled = 0; enabled <= 1; enabled++)
            for (int write_enabled = 0; write_enabled <= 1; write_enabled++)
                for (unsigned function = 0; function < 8; function++)
                    for (unsigned relation = 0; relation < 3; relation++)
                        depth_case(m, format, enabled, write_enabled, function, relation);
        for (int mode = 0; mode < 3; mode++)
            visibility_case(m, format, mode);
        blend_transition_case(m, format);
        if(format==3||format==4)
            for(int mode=0;mode<3;mode++)ordered_blend_identity_case(m,format,mode);
        for (unsigned function = 0; function < 8; function++)
            for (int writes = 0; writes <= 1; writes++)
                for (int dir = -1; dir <= 1; dir += 2)
                    for (int split = 0; split <= 1; split++)
                        depth_field_case(m, format, function, writes, dir, split);
        for (int mode = 0; mode < 3; mode++)
            for (int dx = -1; dx <= 1; dx += 2)
                for (int dy = -1; dy <= 1; dy += 2)
                    for (int clip = 0; clip <= 1; clip++)
                        for (int split = 0; split <= 1; split++)
                            color_field_case(m, format, mode, dx, dy, clip, split);
    }
    hidden_fraction_case(m);
    for (int last_pixel = 0; last_pixel <= 1; last_pixel++) {
        line_to_triangle_case(m, LEAD_LENGTH, last_pixel);
        line_to_triangle_case(m, LEAD_ALIAS, last_pixel);
    }
    legacy_barrier_case(m);
    for (int mode = 0; mode < 9; mode++)
        for (int line = 0; line <= 2; line++)
            scissor_case(m, mode, line);
    for (int mode = 0; mode < 4; mode++) {
        line_dispatch_case(m, mode, LEAD_LENGTH);
        line_dispatch_case(m, mode, LEAD_ALIAS);
    }
    for (int textured = 0; textured <= 1; textured++)
        for (int dx = -1; dx <= 1; dx += 2)
            for (int dy = -1; dy <= 1; dy += 2)
                for (int slope = 0; slope <= 1; slope++)
                    for (int clip = 0; clip <= 1; clip++)
                        pixie_profile_case(m, textured, dx, dy, slope, clip);
    for (unsigned fixture = 0; fixture < sizeof(ati_driver_triangles) / sizeof(ati_driver_triangles[0]); fixture++)
        for (unsigned fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++)
            driver_triangle_case(m, fixture, formats[fi]);
    for (int dx = -1; dx <= 1; dx += 2)
        for (int dy = -1; dy <= 1; dy += 2)
            for (int zero_negative = 0; zero_negative <= 1; zero_negative++)
                for (int split = 0; split <= 1; split++)
                    initial_edge_texture_case(m, dx, dy, zero_negative, split);
    mach64_3d_detach(m);
    free(m->svga.changedvram);
    free(m->svga.vram);
    free(m->svga.monitor);
    free(m);
    printf("Mach64 command regressions: %u cases, %u failed checks\n", cases, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
