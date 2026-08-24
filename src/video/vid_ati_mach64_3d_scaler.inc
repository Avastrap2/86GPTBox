/* ATI 3D RAGE front-end scaler (SCALE_3D_FCN = 1). */
#include "vid_ati_mach64_3d_scaler_math.h"

#define R3D_SCALE_PIX_REP (1u << 8)

static int
r3d_scaler_source_bpp(int format)
{
    if (format == 11)
        return 2; /* packed YUYV: four bytes for two pixels */
    return r3d_bytes_per_pixel(format);
}

static int
r3d_scaler_destination_supported(int format)
{
    switch (format) {
        case 2:
        case 3:
        case 4:
        case 6:
        case 7:
        case 8:
        case 15:
            return 1;
        default:
            return 0;
    }
}

static rgba_t
r3d_scaler_read_source(mach64_3d_ctx_t *ctx, int format, uint32_t base,
                       uint32_t pitch, int x, int y)
{
    mach64_t *m = ctx->mach64;
    uint32_t address;
    uint32_t raw;

    if (format == 11) {
        address = base + ((uint32_t) y * pitch + ((uint32_t) x & ~1u)) * 2u;
        raw = r3d_vram_read32(m, address);
        return r3d_yuv_to_rgba((x & 1) ? ((raw >> 16) & 0xff) : (raw & 0xff),
                               (raw >> 8) & 0xff, (raw >> 24) & 0xff, 255);
    }

    address = base + ((uint32_t) y * pitch + (uint32_t) x) *
              (uint32_t) r3d_scaler_source_bpp(format);
    switch (r3d_scaler_source_bpp(format)) {
        case 1:
            raw = r3d_vram_read8(m, address);
            break;
        case 2:
            raw = r3d_vram_read16(m, address);
            break;
        case 4:
            raw = r3d_vram_read32(m, address);
            break;
        default: {
            rgba_t black = { 0, 0, 0, 255 };
            return black;
        }
    }
    return r3d_unpack(ctx, format, raw, 1);
}

static rgba_t
r3d_scaler_lerp_rgba(rgba_t first, rgba_t second, unsigned coefficient)
{
    rgba_t result;

    result.r = mach64_scaler_lerp5((uint8_t) r3d_clamp8(first.r),
                                   (uint8_t) r3d_clamp8(second.r), coefficient);
    result.g = mach64_scaler_lerp5((uint8_t) r3d_clamp8(first.g),
                                   (uint8_t) r3d_clamp8(second.g), coefficient);
    result.b = mach64_scaler_lerp5((uint8_t) r3d_clamp8(first.b),
                                   (uint8_t) r3d_clamp8(second.b), coefficient);
    result.a = mach64_scaler_lerp5((uint8_t) r3d_clamp8(first.a),
                                   (uint8_t) r3d_clamp8(second.a), coefficient);
    return result;
}

static uint32_t
r3d_scaler_read_destination_raw(mach64_t *m, uint32_t address, int bpp)
{
    switch (bpp) {
        case 1:
            return r3d_vram_read8(m, address);
        case 2:
            return r3d_vram_read16(m, address);
        case 4:
            return r3d_vram_read32(m, address);
        default:
            return 0;
    }
}

static uint32_t
r3d_scaler_pack_destination(mach64_3d_ctx_t *ctx, int format, rgba_t color,
                            int x, int y)
{
    if (format == 2) {
        return ((uint32_t) (r3d_clamp8(color.r) >> 5) << 5) |
               ((uint32_t) (r3d_clamp8(color.g) >> 5) << 2) |
               (uint32_t) (r3d_clamp8(color.b) >> 6);
    }
    return r3d_pack(ctx, format, color, x, y);
}

static int
r3d_scaler_compare_inhibits(mach64_t *m, uint32_t source,
                            uint32_t destination)
{
    uint32_t selected = (m->clr_cmp_cntl & 0x01000000u) ?
                        source : destination;

    switch (m->clr_cmp_cntl & 7u) {
        case 1:
            return 1;
        case 4:
            return (selected & m->clr_cmp_mask) != m->clr_cmp_clr;
        case 5:
            return (selected & m->clr_cmp_mask) == m->clr_cmp_clr;
        default:
            return 0;
    }
}

static int
r3d_scaler_write_pixel(mach64_3d_ctx_t *ctx, uint32_t address, int format,
                       int x, int y, rgba_t source)
{
    mach64_t *m = ctx->mach64;
    int bpp = r3d_bytes_per_pixel(format);
    uint32_t pixel_mask = bpp == 1 ? 0xffu :
                          (bpp == 2 ? 0xffffu : 0xffffffffu);
    uint32_t destination = r3d_scaler_read_destination_raw(m, address, bpp);
    uint32_t packed_source;
    uint32_t result;
    uint32_t write_mask;

    if (((ctx->regs[R3D_SCALE_3D_CNTL >> 2] >> R3D_ALPHA_FOG_SHIFT) &
         R3D_ALPHA_FOG_MASK) == 1)
        source = r3d_blend(ctx->regs[R3D_SCALE_3D_CNTL >> 2], source,
                           r3d_read_dst(ctx, address, format));

    packed_source = r3d_scaler_pack_destination(ctx, format, source, x, y) &
                    pixel_mask;
    if (r3d_scaler_compare_inhibits(m, packed_source, destination))
        return 0;

    result = mach64_scaler_mix(packed_source, destination,
                               (m->dp_mix >> 16) & 0x1fu) & pixel_mask;
    write_mask = m->write_mask & pixel_mask;
    result = (result & write_mask) | (destination & ~write_mask);

    switch (bpp) {
        case 1:
            r3d_vram_write8(m, address, (uint8_t) result);
            break;
        case 2:
            r3d_vram_write16(m, address, (uint16_t) result);
            break;
        case 4:
            r3d_vram_write32(m, address, result);
            break;
        default:
            return 0;
    }
    return 1;
}

static void
r3d_scaler_set_byte(uint32_t *value, unsigned lane, uint8_t byte)
{
    unsigned shift = (lane & 3u) * 8u;

    *value = (*value & ~(0xffu << shift)) | ((uint32_t) byte << shift);
}

/* Apply only the destination register lanes handled by the legacy FIFO. */
static void
r3d_scaler_apply_destination_write(mach64_t *m, uint32_t address,
                                   uint32_t value, uint32_t type)
{
    int bytes = type == FIFO_WRITE_DWORD ? 4 :
                (type == FIFO_WRITE_WORD ? 2 : 1);

    for (int i = 0; i < bytes; i++) {
        uint32_t byte_address = (address + (uint32_t) i) & 0x3ffu;
        uint8_t byte = (uint8_t) (value >> (i * 8));

        if ((byte_address >= 0x104 && byte_address <= 0x105) ||
            (byte_address >= 0x11c && byte_address <= 0x11d))
            r3d_scaler_set_byte(&m->dst_y_x, byte_address + 2u, byte);
        else if ((byte_address >= 0x108 && byte_address <= 0x109) ||
                 (byte_address >= 0x10c && byte_address <= 0x10f))
            r3d_scaler_set_byte(&m->dst_y_x, byte_address, byte);
        else if (byte_address >= 0x110 && byte_address <= 0x111)
            r3d_scaler_set_byte(&m->dst_height_width,
                                byte_address + 2u, byte);
        else if ((byte_address >= 0x114 && byte_address <= 0x115) ||
                 (byte_address >= 0x118 && byte_address <= 0x11b) ||
                 (byte_address >= 0x11e && byte_address <= 0x11f))
            r3d_scaler_set_byte(&m->dst_height_width, byte_address, byte);
    }
}

static int
r3d_scaler_trigger_write(uint32_t address, uint32_t value, uint32_t type)
{
    uint32_t base = address & ~3u;
    unsigned terminal_shift;

    if (base != 0x110 && base != 0x118 && base != 0x11c)
        return 0;
    if (!r3d_write_complete(address, type))
        return 0;
    terminal_shift = type == FIFO_WRITE_DWORD ? 24u :
                     (type == FIFO_WRITE_WORD ? 8u : 0u);
    return !((value >> terminal_shift) & 0x80u);
}

static int
r3d_scaler_active(mach64_3d_ctx_t *ctx)
{
    uint32_t control = ctx->regs[R3D_SCALE_3D_CNTL >> 2];

    return (((control >> R3D_FCN_SHIFT) & R3D_FCN_MASK) == 1u) &&
           !(control & R3D_SRC_3D_SEL) &&
           (((ctx->mach64->dp_src >> 8) & 7u) == R3D_DP_SRC_SCALE_3D);
}

static void
r3d_draw_scaler_rectangle(mach64_3d_ctx_t *ctx)
{
    mach64_t *m = ctx->mach64;
    uint32_t control = ctx->regs[R3D_SCALE_3D_CNTL >> 2];
    uint32_t source_base = ctx->regs[R3D_SCALE_Y_OFF >> 2] & ~7u;
    uint32_t source_width = ctx->regs[R3D_SCALE_WIDTH >> 2] & 0x1fffu;
    uint32_t source_height = ctx->regs[R3D_SCALE_HEIGHT >> 2] & 0x1fffu;
    uint32_t source_pitch = ctx->regs[R3D_SCALE_Y_PITCH >> 2] & 0x1fffu;
    uint32_t x_increment = ctx->regs[R3D_SCALE_X_INC >> 2] & 0x001ffff0u;
    uint32_t y_increment = ctx->regs[R3D_SCALE_Y_INC >> 2] & 0x001ffff0u;
    int32_t h_start = r3d_sign_extend(ctx->regs[R3D_SCALE_HACC >> 2], 21);
    int32_t v_start = r3d_sign_extend(ctx->regs[R3D_SCALE_VACC >> 2], 21);
    int source_format = r3d_tex_format(m);
    int destination_format = r3d_dst_format(m);
    int destination_bpp = r3d_bytes_per_pixel(destination_format);
    int destination_pitch = ((m->dst_off_pitch >> 22) & 0x3ff) << 3;
    uint32_t destination_base = (m->dst_off_pitch & 0xfffff) << 3;
    int destination_x = r3d_sign_extend(m->dst_y_x >> 16, 13);
    int destination_y = r3d_sign_extend(m->dst_y_x, 15);
    int destination_width = (m->dst_height_width >> 16) & 0x1fff;
    int destination_height = m->dst_height_width & 0x1fff;
    int x_direction = (m->dst_cntl & DST_X_DIR) ? 1 : -1;
    int y_direction = (m->dst_cntl & DST_Y_DIR) ? 1 : -1;
    int scissor_left = m->sc_left_right & 0x1fff;
    int scissor_right = (m->sc_left_right >> 16) & 0x1fff;
    int scissor_top = m->sc_top_bottom & 0x7fff;
    int scissor_bottom = (m->sc_top_bottom >> 16) & 0x7fff;
    int replicate = !!(control & R3D_SCALE_PIX_REP);
    uint64_t vram_bytes = (uint64_t) m->vram_mask + 1u;

    if (!source_width || !source_height || !source_pitch ||
        !destination_width || !destination_height ||
        !r3d_scaler_source_bpp(source_format) || !destination_bpp ||
        !r3d_scaler_destination_supported(destination_format))
        goto complete;

    for (int dy = 0; dy < destination_height; dy++) {
        int y = destination_y + dy * y_direction;
        int32_t v_accumulator = v_start + (int32_t) ((uint32_t) dy * y_increment);
        int source_y = mach64_scaler_accum_pixel(v_accumulator);
        int source_y_next;
        unsigned v_fraction = mach64_scaler_accum_fraction5(v_accumulator);

        if (source_y < 0)
            source_y = 0;
        if (source_y >= (int) source_height)
            source_y = (int) source_height - 1;
        source_y_next = source_y + 1;
        if (source_y_next >= (int) source_height)
            source_y_next = (int) source_height - 1;

        for (int dx = 0; dx < destination_width; dx++) {
            int x = destination_x + dx * x_direction;
            int32_t h_accumulator = h_start +
                                    (int32_t) ((uint32_t) dx * x_increment);
            int source_x = mach64_scaler_accum_pixel(h_accumulator);
            int source_x_next;
            unsigned h_fraction =
                mach64_scaler_accum_fraction5(h_accumulator);
            rgba_t source;
            int64_t destination_address;

            if (x < 0 || y < 0 || x < scissor_left || x > scissor_right ||
                y < scissor_top || y > scissor_bottom) {
                continue;
            }

            if (source_x < 0)
                source_x = 0;
            if (source_x >= (int) source_width)
                source_x = (int) source_width - 1;
            source_x_next = source_x + 1;
            if (source_x_next >= (int) source_width)
                source_x_next = (int) source_width - 1;

            source = r3d_scaler_read_source(ctx, source_format, source_base,
                                            source_pitch, source_x, source_y);
            if (!replicate) {
                rgba_t bottom_left =
                    r3d_scaler_read_source(ctx, source_format, source_base,
                                           source_pitch, source_x,
                                           source_y_next);
                rgba_t top_right =
                    r3d_scaler_read_source(ctx, source_format, source_base,
                                           source_pitch, source_x_next,
                                           source_y);
                rgba_t bottom_right =
                    r3d_scaler_read_source(ctx, source_format, source_base,
                                           source_pitch, source_x_next,
                                           source_y_next);
                rgba_t vertical_left =
                    r3d_scaler_lerp_rgba(source, bottom_left, v_fraction);
                rgba_t vertical_right =
                    r3d_scaler_lerp_rgba(top_right, bottom_right, v_fraction);
                source = r3d_scaler_lerp_rgba(vertical_left, vertical_right,
                                               h_fraction);
            }

            destination_address = (int64_t) destination_base +
                                  ((int64_t) y * destination_pitch + x) *
                                  destination_bpp;
            if (destination_address < 0 ||
                (uint64_t) destination_address +
                    (uint64_t) destination_bpp > vram_bytes) {
                continue;
            }
            r3d_scaler_write_pixel(ctx, (uint32_t) destination_address,
                                   destination_format, x, y, source);
        }
    }

complete:
    {
        uint32_t old_vacc = ctx->regs[R3D_SCALE_VACC >> 2];
        uint32_t advanced = (uint32_t) ((int64_t) v_start +
                            (int64_t) destination_height * y_increment);

        ctx->regs[R3D_SCALE_VACC >> 2] =
            (old_vacc & ~0x001ffff0u) | (advanced & 0x001ffff0u);
    }
}

/*
 * Claim the destination trigger before the legacy rectangle worker sees
 * DP_SRC foreground selector 5.  That selector belongs to the scaler/3D pipe,
 * so the legacy worker has no valid source pixel for this operation.
 */
static int
r3d_try_scaler_destination_write(mach64_3d_ctx_t *ctx, uint32_t address,
                                 uint32_t value, uint32_t type)
{
    mach64_t *m;

    if (!ctx || !(m = ctx->mach64) ||
        !r3d_scaler_trigger_write(address, value, type))
        return 0;

    /* SCALE_3D_FCN is GT-only state and is already ordered here. */
    if (((ctx->regs[R3D_SCALE_3D_CNTL >> 2] >> R3D_FCN_SHIFT) &
         R3D_FCN_MASK) != 1u)
        return 0;

    /* DP_SRC and destination state use the legacy FIFO. */
    r3d_sync_legacy_fifo(ctx);
    if (!r3d_scaler_active(ctx))
        return 0;

    address &= 0x3ffu;
    r3d_scaler_apply_destination_write(m, address, value, type);
    r3d_draw_scaler_rectangle(ctx);
    return 1;
}
