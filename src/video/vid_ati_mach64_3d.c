/* ATI 3D RAGE / Rage II+ software renderer. */
#include "vid_ati_mach64_3d.h"

#include "vid_ati_mach64_3d_part1.inc"
#include "vid_ati_mach64_3d_part2.inc"
#include "vid_ati_mach64_3d_line.inc"
#include "vid_ati_mach64_3d_part3.inc"

/*
 * Keep the established part4 register decoder as the base implementation, then
 * wrap its shared DST_BRES_LNTH entry points below.  GT/GTB 3D lines use the
 * normal Bresenham trajectory register but may select Scaler/3D data as the
 * foreground pixel source; that source does not exist in the legacy 2D blitter.
 */
#define mach64_3d_write mach64_3d_write_base
#include "vid_ati_mach64_3d_part4.inc"
#undef mach64_3d_write

#include "vid_ati_mach64_3d_scaler.inc"

int
mach64_3d_write(mach64_t *m, uint32_t a, uint32_t v, uint32_t type)
{
    mach64_3d_ctx_t *ctx = r3d_find(m);

    if (ctx) {
        uint32_t aa = a & 0x3ffu;
        uint32_t b = aa & ~3u;

        if (r3d_try_scaler_destination_write(ctx, aa, v, type))
            return 1;

        /*
         * 3D RAGE exposes the lead/Bresenham length register at both MM
         * offsets 0_48 and 0_51 (byte offsets 0x120 and 0x144).  part4 already
         * mirrors both addresses into the same shadow for trapezoids.  Shaded
         * line commands must be intercepted at both aliases too; otherwise a
         * line written through 0x144 is consumed as GT-only state without ever
         * starting the line walker, leaving holes in connected LINESTRIPs.
         */
        if (b == R3D_LEAD_BRES_LNTH || b == R3D_LEAD_BRES_LNTH_ALIAS) {
            uint32_t i = R3D_LEAD_BRES_LNTH >> 2;
            uint32_t cmd = r3d_merge_write(ctx->regs[i], aa, v, type);

            /*
             * For byte/word command programming, let incomplete writes keep
             * following the ordinary shared-register path.  The final write is
             * claimed only after the full command selects the 3D shaded-line
             * source.  This prevents the legacy 2D line engine from consuming
             * source selector 5 and substituting black.
             */
            if (r3d_write_complete(aa, type) &&
                r3d_is_shaded_line_command(ctx, cmd)) {
                ctx->regs[i] = cmd;
                m->dst_bres_lnth = r3d_merge_write(m->dst_bres_lnth,
                                                    aa, v, type);

                r3d_sync_legacy_fifo(ctx);
                r3d_draw_shaded_line(ctx, cmd);
                return 1;
            }
        }
    }

    return mach64_3d_write_base(m, a, v, type);
}

/*
 * Rage II+ reuses the mature VT2 initializer, but it must not enter the core as
 * MACH64_VT2.  The core has one hardware-visible VT/VT2 special case in
 * CONFIG_CNTL aperture-base readback; GT/GTB belongs to the modern integrated
 * layout.  Keep a persistent copy of the device descriptor because svga/device
 * initialization is allowed to retain descriptor pointers beyond this call.
 */
extern const device_t mach64vt2_device;
static device_t mach64gtb_core_info;

void *
mach64_gtb_core_init(const device_t *info)
{
    mach64gtb_core_info = *info;
    mach64gtb_core_info.local = (mach64gtb_core_info.local & ~0xffu) | MACH64_GTB;
    return mach64vt2_device.init(&mach64gtb_core_info);
}
