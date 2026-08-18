/*
 * 86GPTBox - ATI 3D RAGE / Rage II+ (Mach64 GT/GTB) software 3D pipeline.
 *
 * Register layout and fixed-point formats follow ATI's 1996
 * "mach64 Register Reference Guide - ATI-264VT & 3D RAGE".
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vid_ati_mach64_3d.h"

#define MACH64_3D_CONTEXTS 4
#define R3D_TRACE_RING 4096
#define R3D_LEAD_BRES_LNTH 0x120
#define R3D_TRAIL_BRES_ERR 0x138
#define R3D_TRAIL_BRES_INC 0x13c
#define R3D_TRAIL_BRES_DEC 0x140
#define R3D_LEAD_BRES_LNTH_ALIAS 0x144
#define R3D_Z_OFF_PITCH 0x148
#define R3D_Z_CNTL 0x14c

#define R3D_TEX_0_OFF 0x1c0
#define R3D_TEX_10_OFF 0x1e8
#define R3D_SCALE_3D_CNTL 0x1fc

/* GT/GTB texture interpolators.  Rage Pro and later reused several of these
 * addresses for STW/palette controls; Rage II+ uses the quadratic S/T set. */
#define R3D_S_X_INC2 0x340
#define R3D_S_Y_INC2 0x344
#define R3D_S_XY_INC2 0x348
#define R3D_S_XINC_START 0x34c
#define R3D_S_Y_INC 0x350
#define R3D_S_START 0x354
#define R3D_T_X_INC2 0x358
#define R3D_T_Y_INC2 0x35c
#define R3D_T_XY_INC2 0x360
#define R3D_T_XINC_START 0x364
#define R3D_T_Y_INC 0x368
#define R3D_T_START 0x36c
#define R3D_TEX_SIZE_PITCH 0x370

#define R3D_RED_X_INC 0x3c0
#define R3D_RED_Y_INC 0x3c4
#define R3D_RED_START 0x3c8
#define R3D_GREEN_X_INC 0x3cc
#define R3D_GREEN_Y_INC 0x3d0
#define R3D_GREEN_START 0x3d4
#define R3D_BLUE_X_INC 0x3d8
#define R3D_BLUE_Y_INC 0x3dc
#define R3D_BLUE_START 0x3e0
#define R3D_Z_X_INC 0x3e4
#define R3D_Z_Y_INC 0x3e8
#define R3D_Z_START 0x3ec
#define R3D_ALPHA_X_INC 0x3f0
#define R3D_ALPHA_Y_INC 0x3f4
#define R3D_ALPHA_START 0x3f8

#define R3D_DRAW_TRAP (1u << 15)
#define R3D_TRAIL_X_DIR (1u << 28)

#define R3D_Z_EN (1u << 0)
#define R3D_Z_SRC (1u << 1)
#define R3D_Z_TEST_SHIFT 2
#define R3D_Z_MASK (1u << 5)

#define R3D_FCN_SHIFT 6
#define R3D_FCN_MASK 3
#define R3D_ALPHA_FOG_SHIFT 11
#define R3D_ALPHA_FOG_MASK 3
#define R3D_COLOR_OVERRIDE (1u << 13)
#define R3D_BLEND_SRC_SHIFT 16
#define R3D_BLEND_DST_SHIFT 19
#define R3D_BLEND_MASK 7
#define R3D_TEX_LIGHT_SHIFT 22
#define R3D_TEX_LIGHT_MASK 3
#define R3D_MIP_MAP_DISABLE (1u << 24)
#define R3D_BILINEAR_TEX_EN (1u << 25)
#define R3D_TEX_BLEND_SHIFT 26
#define R3D_TEX_BLEND_MASK 3
#define R3D_TEX_AMASK_AEN (1u << 28)
#define R3D_TEX_AMASK_MODE (1u << 29)
#define R3D_TEX_MAP_AEN (1u << 30)
#define R3D_SRC_3D_SEL (1u << 31)

typedef struct rgba_t { int r, g, b, a; } rgba_t;
typedef struct mach64_3d_ctx_t {
    mach64_t *mach64;
    uint32_t regs[256];
    int trail_x;
    int trail_valid;
} mach64_3d_ctx_t;

typedef struct r3d_trace_event_t {
    uint64_t seq;
    uint32_t addr;
    uint32_t value;
    uint32_t dst_cntl;
    uint32_t dp_pix_width;
    uint32_t scale_3d_cntl;
    uint8_t op;
    uint8_t width;
    uint8_t claimed;
} r3d_trace_event_t;

static mach64_3d_ctx_t mach64_3d_ctx[MACH64_3D_CONTEXTS];
static r3d_trace_event_t r3d_trace_ring[R3D_TRACE_RING];
static uint64_t r3d_trace_seq;
static uint32_t r3d_trace_pos;

static int r3d_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int r3d_clamp16(int64_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : (int)v); }
static int r3d_sign_extend(uint32_t v, unsigned bits) { uint32_t m = 1u << (bits - 1); v &= (1u << bits) - 1u; return (int)((v ^ m) - m); }

static mach64_3d_ctx_t *r3d_find(mach64_t *mach64) {
    for (int i = 0; i < MACH64_3D_CONTEXTS; i++) if (mach64_3d_ctx[i].mach64 == mach64) return &mach64_3d_ctx[i];
    return NULL;
}

static void
r3d_trace_reset(void)
{
    memset(r3d_trace_ring, 0, sizeof(r3d_trace_ring));
    r3d_trace_seq = 0;
    r3d_trace_pos = 0;
}

/*
 * Keep tracing out of the hot path's filesystem code.  The guest may issue
 * tens of thousands of MMIO operations while an ATI diagnostic is running;
 * retaining only the final 4096 relevant operations gives us the sequence
 * immediately preceding a guest-side fault without perturbing FIFO timing.
 */
static void
r3d_trace_record(mach64_t *m, char op, unsigned width, uint32_t addr,
                 uint32_t value, int claimed)
{
    r3d_trace_event_t *e;
    mach64_3d_ctx_t *ctx;

    if (!m || m->pci_id != 0x4755)
        return;

    e = &r3d_trace_ring[r3d_trace_pos];
    e->seq = ++r3d_trace_seq;
    e->addr = addr & 0x3ff;
    e->value = value;
    e->dst_cntl = m->dst_cntl;
    e->dp_pix_width = m->dp_pix_width;
    ctx = r3d_find(m);
    e->scale_3d_cntl = ctx ? ctx->regs[R3D_SCALE_3D_CNTL >> 2] : 0;
    e->op = (uint8_t) op;
    e->width = (uint8_t) width;
    e->claimed = claimed ? 1 : 0;
    r3d_trace_pos = (r3d_trace_pos + 1) % R3D_TRACE_RING;
}

static void
r3d_trace_dump(mach64_t *m)
{
    FILE *fp;
    uint32_t count;
    uint32_t start;

    if (!m || m->pci_id != 0x4755 || !r3d_trace_seq)
        return;

    fp = fopen("rage2p_trace.log", "w");
    if (!fp) {
        pclog("ATI Rage II+: unable to create rage2p_trace.log\n");
        return;
    }

    fprintf(fp, "# 86GPTBox ATI 3D Rage II+ GTB/GU trace\n");
    fprintf(fp, "# PCI=%04X CHIP=%08X MEM_CNTL=%08X CNFG_STAT0=%08X\n",
            m->pci_id, m->config_chip_id, m->mem_cntl, m->config_stat0);
    fprintf(fp, "# CLOCK_CNTL=%08X PLL_ADDR=%02X DP_PIX_WIDTH=%08X DP_MIX=%08X DP_SRC=%08X\n",
            m->clock_cntl, m->pll_addr, m->dp_pix_width, m->dp_mix, m->dp_src);
    fprintf(fp, "# DST_OFF_PITCH=%08X DST_Y_X=%08X DST_CNTL=%08X BRES_ERR=%08X INC=%08X DEC=%08X\n",
            m->dst_off_pitch, m->dst_y_x, m->dst_cntl,
            m->dst_bres_err, m->dst_bres_inc, m->dst_bres_dec);
    fprintf(fp, "# SC_LEFT_RIGHT=%08X SC_TOP_BOTTOM=%08X events=%llu\n",
            m->sc_left_right, m->sc_top_bottom,
            (unsigned long long) r3d_trace_seq);
    fprintf(fp, "# seq op width claimed addr value dst_cntl dp_pix_width scale_3d_cntl\n");

    count = (r3d_trace_seq < R3D_TRACE_RING) ? (uint32_t) r3d_trace_seq : R3D_TRACE_RING;
    start = (r3d_trace_seq < R3D_TRACE_RING) ? 0 : r3d_trace_pos;
    for (uint32_t i = 0; i < count; i++) {
        const r3d_trace_event_t *e = &r3d_trace_ring[(start + i) % R3D_TRACE_RING];
        fprintf(fp, "%010llu %c %u %u %03X %08X %08X %08X %08X\n",
                (unsigned long long) e->seq, e->op, e->width, e->claimed,
                e->addr, e->value, e->dst_cntl, e->dp_pix_width,
                e->scale_3d_cntl);
    }

    fclose(fp);
    pclog("ATI Rage II+: wrote rage2p_trace.log (%u events)\n", count);
}

void mach64_3d_attach(mach64_t *mach64) {
    if (r3d_find(mach64)) return;
    for (int i = 0; i < MACH64_3D_CONTEXTS; i++) if (!mach64_3d_ctx[i].mach64) { memset(&mach64_3d_ctx[i], 0, sizeof(mach64_3d_ctx[i])); mach64_3d_ctx[i].mach64 = mach64; r3d_trace_reset(); return; }
    fatal("ATI 3D RAGE: no free 3D context slot\n");
}
void mach64_3d_detach(mach64_t *mach64) { mach64_3d_ctx_t *ctx = r3d_find(mach64); if (ctx) { r3d_trace_dump(mach64); memset(ctx, 0, sizeof(*ctx)); } }

static uint8_t r3d_vram_read8(mach64_t *m, uint32_t a) { return m->svga.vram[a & m->vram_mask]; }
static uint16_t r3d_vram_read16(mach64_t *m, uint32_t a) { return (uint16_t)(r3d_vram_read8(m,a) | (r3d_vram_read8(m,a+1) << 8)); }
static uint32_t r3d_vram_read32(mach64_t *m, uint32_t a) { return (uint32_t)r3d_vram_read16(m,a) | ((uint32_t)r3d_vram_read16(m,a+2) << 16); }
static void r3d_mark_changed(mach64_t *m, uint32_t a) { a &= m->vram_mask; m->svga.changedvram[a >> 12] = m->svga.monitor->mon_changeframecount; }
static void r3d_vram_write8(mach64_t *m, uint32_t a, uint8_t v) { a &= m->vram_mask; m->svga.vram[a] = v; r3d_mark_changed(m,a); }
static void r3d_vram_write16(mach64_t *m, uint32_t a, uint16_t v) { r3d_vram_write8(m,a,v); r3d_vram_write8(m,a+1,v>>8); }
static void r3d_vram_write32(mach64_t *m, uint32_t a, uint32_t v) { r3d_vram_write16(m,a,v); r3d_vram_write16(m,a+2,v>>16); }

/* DP_DST_PIX_WIDTH is the framebuffer type.  DP_SCALE_PIX_WIDTH is the
 * independent 3D/texture source type. */
static int r3d_dst_format(mach64_t *m) { return m->dp_pix_width & 15; }
static int r3d_tex_format(mach64_t *m) { return (m->dp_pix_width >> 28) & 15; }
static int r3d_bytes_per_pixel(int f) { switch (f) { case 2: case 7: case 8: return 1; case 3: case 4: case 15: return 2; case 11: case 6: case 14: return 4; default: return 0; } }
