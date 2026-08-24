/*
 * 86GPTBox - ATI 3D RAGE / Rage II+ (Mach64 GT/GTB) software 3D pipeline.
 *
 * Register layout and fixed-point formats follow ATI's 1996
 * "mach64 Register Reference Guide - ATI-264VT & 3D RAGE".
 */
#include <stdint.h>
#include <string.h>
#include "vid_ati_mach64_3d.h"
#include "vid_ati_mach64_3d_texture_palette.h"

#define MACH64_3D_CONTEXTS 4
#define R3D_LEAD_BRES_LNTH 0x120
#define R3D_TRAIL_BRES_ERR 0x138
#define R3D_TRAIL_BRES_INC 0x13c
#define R3D_TRAIL_BRES_DEC 0x140
#define R3D_LEAD_BRES_LNTH_ALIAS 0x144
#define R3D_Z_OFF_PITCH 0x148
#define R3D_Z_CNTL 0x14c

#define R3D_TEX_0_OFF 0x1c0
#define R3D_TEX_10_OFF 0x1e8

/* Front-end scaler register names and their texture/interpolator aliases. */
#define R3D_SCALE_Y_OFF 0x1c0
#define R3D_SCALE_WIDTH 0x1dc
#define R3D_SCALE_HEIGHT 0x1e0
#define R3D_SCALE_Y_PITCH 0x1ec
#define R3D_SCALE_X_INC 0x1f0
#define R3D_SCALE_Y_INC 0x1f4
#define R3D_SCALE_VACC 0x1f8
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
#define R3D_TEX_PALETTE 0x37c

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

#define R3D_SCALE_Y_PITCH_ALIAS R3D_S_Y_INC
#define R3D_SCALE_X_INC_ALIAS R3D_RED_X_INC
#define R3D_SCALE_HACC R3D_RED_START
#define R3D_SCALE_Y_INC_ALIAS R3D_GREEN_X_INC

#define R3D_DRAW_TRAP (1u << 15)
#define R3D_TRAIL_X_DIR (1u << 28)

#define R3D_Z_EN (1u << 0)
#define R3D_Z_SRC (1u << 1)
/* 3D RAGE Z_CNTL places the three-bit compare function at bits 4..6 and
 * Z_MASK at bit 8.  A typical Win9x HAL value of 0x121 therefore means
 * Z enabled, LEQUAL (2 << 4), and Z writes enabled. */
#define R3D_Z_TEST_SHIFT 4
#define R3D_Z_MASK (1u << 8)

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
    uint32_t texture_palette[MACH64_3D_TEXTURE_PALETTE_ENTRIES];
    int trail_x;
    int trail_valid;
    /*
     * GT-only register writes are consumed outside the mature Mach64 FIFO.
     * Remember whether an ordinary 2D/shared write has been handed to that
     * FIFO since the last synchronization.  We only need to drain once when
     * crossing back into GT state/commands, not once for every 3D register.
     */
    int legacy_fifo_pending;
} mach64_3d_ctx_t;

static mach64_3d_ctx_t mach64_3d_ctx[MACH64_3D_CONTEXTS];

static int r3d_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int r3d_clamp16(int64_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : (int)v); }
static int r3d_sign_extend(uint32_t v, unsigned bits) { uint32_t m = 1u << (bits - 1); v &= (1u << bits) - 1u; return (int)((v ^ m) - m); }

static mach64_3d_ctx_t *r3d_find(mach64_t *mach64) {
    for (int i = 0; i < MACH64_3D_CONTEXTS; i++) if (mach64_3d_ctx[i].mach64 == mach64) return &mach64_3d_ctx[i];
    return NULL;
}

void mach64_3d_attach(mach64_t *mach64) {
    if (r3d_find(mach64)) return;
    for (int i = 0; i < MACH64_3D_CONTEXTS; i++) if (!mach64_3d_ctx[i].mach64) { memset(&mach64_3d_ctx[i], 0, sizeof(mach64_3d_ctx[i])); mach64_3d_ctx[i].mach64 = mach64; return; }
    fatal("ATI 3D RAGE: no free 3D context slot\n");
}
void mach64_3d_detach(mach64_t *mach64) { mach64_3d_ctx_t *ctx = r3d_find(mach64); if (ctx) memset(ctx, 0, sizeof(*ctx)); }

/*
 * The software rasterizer touches VRAM for every color and Z sample.  Keep the
 * byte-wise little-endian semantics, but do the address masking and dirty-page
 * bookkeeping once per pixel rather than once per byte.  The old helpers
 * marked a 32-bit color four times and a 16-bit Z value twice, which dominated
 * small Direct3D workloads such as Flip3D.
 */
static uint8_t r3d_vram_read8(mach64_t *m, uint32_t a) {
    return m->svga.vram[a & m->vram_mask];
}
static uint16_t r3d_vram_read16(mach64_t *m, uint32_t a) {
    uint32_t mask=m->vram_mask;uint8_t*v=m->svga.vram;
    return (uint16_t)(v[a&mask]|((uint16_t)v[(a+1)&mask]<<8));
}
static uint32_t r3d_vram_read32(mach64_t *m, uint32_t a) {
    uint32_t mask=m->vram_mask;uint8_t*v=m->svga.vram;
    return (uint32_t)v[a&mask]|((uint32_t)v[(a+1)&mask]<<8)|
           ((uint32_t)v[(a+2)&mask]<<16)|((uint32_t)v[(a+3)&mask]<<24);
}
static void r3d_mark_changed_span(mach64_t *m,uint32_t a,unsigned bytes) {
    uint32_t mask=m->vram_mask,first=a&mask,last=(a+bytes-1)&mask;
    int stamp=m->svga.monitor->mon_changeframecount;
    m->svga.changedvram[first>>12]=stamp;
    if((first>>12)!=(last>>12))m->svga.changedvram[last>>12]=stamp;
}
static void r3d_vram_write8(mach64_t *m, uint32_t a, uint8_t v) {
    uint32_t mask=m->vram_mask;m->svga.vram[a&mask]=v;r3d_mark_changed_span(m,a,1);
}
static void r3d_vram_write16(mach64_t *m, uint32_t a, uint16_t v) {
    uint32_t mask=m->vram_mask;uint8_t*p=m->svga.vram;
    p[a&mask]=(uint8_t)v;p[(a+1)&mask]=(uint8_t)(v>>8);r3d_mark_changed_span(m,a,2);
}
static void r3d_vram_write32(mach64_t *m, uint32_t a, uint32_t v) {
    uint32_t mask=m->vram_mask;uint8_t*p=m->svga.vram;
    p[a&mask]=(uint8_t)v;p[(a+1)&mask]=(uint8_t)(v>>8);
    p[(a+2)&mask]=(uint8_t)(v>>16);p[(a+3)&mask]=(uint8_t)(v>>24);
    r3d_mark_changed_span(m,a,4);
}

/* DP_DST_PIX_WIDTH is the framebuffer type.  DP_SCALE_PIX_WIDTH is the
 * independent 3D/texture source type. */
static int r3d_dst_format(mach64_t *m) { return m->dp_pix_width & 15; }
static int r3d_tex_format(mach64_t *m) { return (m->dp_pix_width >> 28) & 15; }
static int r3d_bytes_per_pixel(int f) { switch (f) { case 2: case 7: case 8: return 1; case 3: case 4: case 15: return 2; case 11: case 6: case 14: return 4; default: return 0; } }
