#include "vid_ati_mach64_3d_tex_key.h"
#include "vid_ati_mach64_3d_data_path.h"
#include "vid_ati_mach64_3d_reg_fields.h"
#include "vid_ati_mach64_3d_trapezoid.h"

#define R3D_GTB_DST_BRES_SIGN      (1u << 11)
#define R3D_GTB_TRAIL_X_DIR         (1u << 13)
#define R3D_GTB_TRAP_FILL_DIR       (1u << 14)
#define R3D_GTB_TRAIL_BRES_SIGN     (1u << 15)

#define R3D_TEX_INT_S_XINC (1u << 0)
#define R3D_TEX_INT_S_YINC (1u << 1)
#define R3D_TEX_INT_S_START (1u << 2)
#define R3D_TEX_INT_T_XINC (1u << 3)
#define R3D_TEX_INT_T_YINC (1u << 4)
#define R3D_TEX_INT_T_START (1u << 5)
#define R3D_TEX_INT_ALL 0x3fu

/*
 * S/T START exposes only 11 fractional bits, while its first derivatives use
 * 16.  The trajectory therefore has five hidden fractional accumulator bits
 * after the first scanline.  The ATI HAL relies on that live state when it
 * launches the second trapezoid of a triangle without rewriting S/T.  Track
 * which shadows contain that internal value so a new guest write can return a
 * register to its documented physical-field interpretation independently.
 */
static uint8_t r3d_tex_internal_mask[MACH64_3D_CONTEXTS];

static unsigned
r3d_tex_ctx_slot(mach64_3d_ctx_t *ctx)
{
    unsigned slot = (unsigned) (ctx - mach64_3d_ctx);
    return slot < MACH64_3D_CONTEXTS ? slot : 0;
}

static uint8_t
r3d_tex_internal_bit(uint32_t b)
{
    switch (b) {
        case R3D_S_XINC_START: return R3D_TEX_INT_S_XINC;
        case R3D_S_Y_INC:      return R3D_TEX_INT_S_YINC;
        case R3D_S_START:      return R3D_TEX_INT_S_START;
        case R3D_T_XINC_START: return R3D_TEX_INT_T_XINC;
        case R3D_T_Y_INC:      return R3D_TEX_INT_T_YINC;
        case R3D_T_START:      return R3D_TEX_INT_T_START;
        default:               return 0;
    }
}

static int
r3d_tex_reg_is_internal(mach64_3d_ctx_t *ctx, uint32_t b)
{
    uint8_t bit = r3d_tex_internal_bit(b);
    return bit && (r3d_tex_internal_mask[r3d_tex_ctx_slot(ctx)] & bit);
}

static void
r3d_tex_note_guest_write(mach64_3d_ctx_t *ctx, uint32_t b)
{
    uint8_t bit = r3d_tex_internal_bit(b);
    if (bit)
        r3d_tex_internal_mask[r3d_tex_ctx_slot(ctx)] &= (uint8_t) ~bit;
}

static void
r3d_tex_mark_internal(mach64_3d_ctx_t *ctx)
{
    r3d_tex_internal_mask[r3d_tex_ctx_slot(ctx)] |= R3D_TEX_INT_ALL;
}

/* Return the guest-visible physical register image, never hidden trajectory
 * precision or reserved bits.  part4 uses this for readback and partial writes. */
static uint32_t
r3d_gtb_visible_3d_reg(uint32_t b, uint32_t raw)
{
    switch (b) {
        case R3D_S_X_INC2:
        case R3D_S_Y_INC2:
        case R3D_S_XY_INC2:
        case R3D_T_X_INC2:
        case R3D_T_Y_INC2:
        case R3D_T_XY_INC2:
            return mach64_3d_s10_16_encode((int32_t) raw);

        case R3D_S_XINC_START:
        case R3D_S_Y_INC:
        case R3D_T_XINC_START:
        case R3D_T_Y_INC:
            return mach64_3d_s11_16_encode((int32_t) raw);

        case R3D_S_START:
        case R3D_T_START:
            return mach64_3d_u10_11_encode((uint32_t) raw);

        case R3D_RED_X_INC:
        case R3D_RED_Y_INC:
        case R3D_RED_START:
        case R3D_GREEN_X_INC:
        case R3D_GREEN_Y_INC:
        case R3D_GREEN_START:
        case R3D_BLUE_X_INC:
        case R3D_BLUE_Y_INC:
        case R3D_BLUE_START:
        case R3D_ALPHA_X_INC:
        case R3D_ALPHA_Y_INC:
        case R3D_ALPHA_START:
            return mach64_3d_s8_12_encode((int32_t) raw);

        case R3D_Z_X_INC:
        case R3D_Z_Y_INC:
        case R3D_Z_START:
            return mach64_3d_s16_12_encode((int32_t) raw);

        default:
            return raw;
    }
}

static int64_t
r3d_tex_dynamic_decode(mach64_3d_ctx_t *ctx, uint32_t b, uint32_t raw)
{
    if (r3d_tex_reg_is_internal(ctx, b))
        return (int32_t) raw;

    switch (b) {
        case R3D_S_XINC_START:
        case R3D_S_Y_INC:
        case R3D_T_XINC_START:
        case R3D_T_Y_INC:
            return mach64_3d_s11_16_decode(raw);
        case R3D_S_START:
        case R3D_T_START:
            return mach64_3d_u10_11_decode(raw);
        default:
            return (int32_t) raw;
    }
}

static int64_t r3d_quad_at(int64_t start,int64_t inc,int64_t inc2,int n){
    return start+(int64_t)n*inc+((int64_t)n*(n-1)/2)*inc2;
}
static int64_t r3d_quad_step(int64_t inc,int64_t inc2,int n,int dir){
    return dir>0?inc+(int64_t)n*inc2:-inc-(int64_t)(n-1)*inc2;
}

/*
 * Collapse the GT/GTB Bresenham edge engine to one scanline operation.  An
 * axial Y step adds INC, then zero or more X-direction diagonal steps add DEC
 * until the next span boundary is reached.  This is required for shallow edges
 * where X can move more than one pixel between adjacent scanlines.
 */
static int
r3d_edge_step_scanline(int *x, int *e, int inc, int dec, int dir,
                       int zero_negative)
{
    int old = *x;
    int steps = 0;

    *e += inc;
    while (((*e > 0) || ((*e == 0) && !zero_negative)) && steps < 8192) {
        *x += dir;
        *e += dec;
        steps++;

        /* Valid GT triangle setup uses a negative DEC.  Do not let a malformed
         * command lock the emulator in this compatibility software path. */
        if (dec >= 0 && ((*e > 0) || ((*e == 0) && !zero_negative)))
            break;
    }

    return *x - old;
}

static void r3d_draw_trapezoid(mach64_3d_ctx_t *ctx,uint32_t cmd){
    mach64_t*m=ctx->mach64;uint32_t ctl=ctx->regs[R3D_SCALE_3D_CNTL>>2];int fcn=(ctl>>R3D_FCN_SHIFT)&3,df=r3d_dst_format(m),dbpp=r3d_bytes_per_pixel(df),tf=r3d_tex_format(m),tbpp=r3d_bytes_per_pixel(tf),len=cmd&0x7fff;if(!len||!dbpp||!fcn)return;if(fcn==2&&!tbpp)return;
    int lead=r3d_sign_extend(m->dst_y_x>>16,13),y=r3d_sign_extend(m->dst_y_x,15),ydir=(m->dst_cntl&DST_Y_DIR)?1:-1,ldir=(m->dst_cntl&DST_X_DIR)?1:-1,tdir=(m->dst_cntl&R3D_GTB_TRAIL_X_DIR)?1:-1;
    /* GT/GTB places the trapezoid controls in DST_CNTL[13:15].  Bits 11 and
     * 15 select the zero-error side of the lead/trail Bresenham walkers. */
    int fill_lr=!!(m->dst_cntl&R3D_GTB_TRAP_FILL_DIR),lead_zero_neg=!!(m->dst_cntl&R3D_GTB_DST_BRES_SIGN),trail_zero_neg=!!(m->dst_cntl&R3D_GTB_TRAIL_BRES_SIGN);
    if((cmd&0x80000000u)||!ctx->trail_valid){ctx->trail_x=r3d_sign_extend(cmd>>16,13);ctx->trail_valid=1;}int trail=ctx->trail_x,le=(int32_t)m->dst_bres_err,li=(int32_t)m->dst_bres_inc,ld=(int32_t)m->dst_bres_dec,te=(int32_t)ctx->regs[R3D_TRAIL_BRES_ERR>>2],ti=(int32_t)ctx->regs[R3D_TRAIL_BRES_INC>>2],td=(int32_t)ctx->regs[R3D_TRAIL_BRES_DEC>>2];
    int left=m->sc_left_right&0x1fff,right=(m->sc_left_right>>16)&0x1fff,top=m->sc_top_bottom&0x7fff,bottom=(m->sc_top_bottom>>16)&0x7fff;if(right<left){left=0;right=0x1fff;}if(bottom<top){top=0;bottom=0x7fff;}
    uint32_t db=(m->dst_off_pitch&0xfffff)<<3;int dp=((m->dst_off_pitch>>22)&0x3ff)<<3;if(!dp)return;
    /* X outside the programmed pitch would only spill into adjacent rows and
     * can turn a malformed/default 8191-pixel scissor into millions of host
     * pixel operations.  Likewise, rows beyond the physical destination
     * allocation only wrap VRAM and cannot be a valid DirectDraw surface. */
    if(left<0)left=0;
    if(right>=dp)right=dp-1;
    if(right<left)return;
    {uint32_t stride=(uint32_t)dp*(uint32_t)dbpp,base=db&m->vram_mask;if(stride&&m->vram_size>base){uint32_t rows=(m->vram_size-base)/stride;if(rows&&bottom>=(int)rows)bottom=(int)rows-1;}if(bottom<top)return;}
    uint32_t zc=ctx->regs[R3D_Z_CNTL>>2],zo=ctx->regs[R3D_Z_OFF_PITCH>>2],zb=(zo&0xfffff)<<3;int zp=((zo>>22)&0x3ff)<<3;
#define RG(name) int64_t name##x=mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_X_INC>>2]),name##y=mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_Y_INC>>2]),name##0=mach64_3d_s8_12_decode(ctx->regs[R3D_##name##_START>>2])
    RG(RED);RG(GREEN);RG(BLUE);RG(ALPHA);
#undef RG
    int64_t Zx=mach64_3d_s16_12_decode(ctx->regs[R3D_Z_X_INC>>2]),Zy=mach64_3d_s16_12_decode(ctx->regs[R3D_Z_Y_INC>>2]),Z0=mach64_3d_s16_12_decode(ctx->regs[R3D_Z_START>>2]);
    /*
     * Decode guest-programmed S/T using the physical GT field widths.  Once a
     * trapezoid advances the trajectory, START/XINC/YINC shadows retain the
     * hidden 16-fractional-bit accumulator state until that individual register
     * is written again by the guest.
     */
    int64_t s0=r3d_tex_dynamic_decode(ctx,R3D_S_START,ctx->regs[R3D_S_START>>2]),sxi=r3d_tex_dynamic_decode(ctx,R3D_S_XINC_START,ctx->regs[R3D_S_XINC_START>>2]),syi=r3d_tex_dynamic_decode(ctx,R3D_S_Y_INC,ctx->regs[R3D_S_Y_INC>>2]),sx2=mach64_3d_s10_16_decode(ctx->regs[R3D_S_X_INC2>>2]),sy2=mach64_3d_s10_16_decode(ctx->regs[R3D_S_Y_INC2>>2]),sxy2=mach64_3d_s10_16_decode(ctx->regs[R3D_S_XY_INC2>>2]);
    int64_t t0=r3d_tex_dynamic_decode(ctx,R3D_T_START,ctx->regs[R3D_T_START>>2]),txi=r3d_tex_dynamic_decode(ctx,R3D_T_XINC_START,ctx->regs[R3D_T_XINC_START>>2]),tyi=r3d_tex_dynamic_decode(ctx,R3D_T_Y_INC,ctx->regs[R3D_T_Y_INC>>2]),tx2=mach64_3d_s10_16_decode(ctx->regs[R3D_T_X_INC2>>2]),ty2=mach64_3d_s10_16_decode(ctx->regs[R3D_T_Y_INC2>>2]),txy2=mach64_3d_s10_16_decode(ctx->regs[R3D_T_XY_INC2>>2]);
    int tex=fcn==2,bilinear=!!(ctl&R3D_BILINEAR_TEX_EN),af=(ctl>>R3D_ALPHA_FOG_SHIFT)&3,z_enabled=!!((zc&R3D_Z_EN)&&zp),z_write=!!(z_enabled&&(zc&R3D_Z_MASK));rgba_t fog={(m->dp_frgd_clr>>16)&255,(m->dp_frgd_clr>>8)&255,m->dp_frgd_clr&255,255};
    unsigned dp_mono_source=(m->dp_src>>16)&3u,dp_frgd_source=(m->dp_src>>8)&7u;int dp_constant_source=dp_mono_source==MONO_SRC_1&&(dp_frgd_source==MACH64_3D_DP_SRC_BKGD_CLR||dp_frgd_source==MACH64_3D_DP_SRC_FRGD_CLR),destination_compare=mach64_3d_destination_compare_enabled(m->clr_cmp_cntl);uint32_t compare_pixel_mask=dbpp==1?0xffu:(dbpp==2?0xffffu:0xffffffffu);
    r3d_tex_state_t ts;if(tex)r3d_tex_state_init(ctx,tf,bilinear,&ts);
    r3d_debug_draw_t dbg;r3d_debug_draw_begin(ctx,&dbg,cmd,lead,trail,y,left,right,top,bottom,dp,len,df,dbpp,tf,tbpp);
    for(int row=0;row<len;row++){
        /* TRAP_FILL_DIR defines which programmed edge is the left/right side.
         * Do not sort crossed edges into a mirrored continuation. */
        if(y>=top&&y<=bottom){
            int dl,dh;
            if(mach64_3d_trapezoid_clip_span(lead,trail,fill_lr,left,right,&dl,&dh)){
                /* X interpolation registers are defined for steps in the
                 * programmed DST_X_DIR direction, not absolute screen +X. */
                int n=(dl-lead)*ldir;
                int64_t rr=RED0+REDx*n,gg=GREEN0+GREENx*n,bb=BLUE0+BLUEx*n,aa=ALPHA0+ALPHAx*n,zz=Z0+Zx*n;
                int64_t rrstep=REDx*ldir,ggstep=GREENx*ldir,bbstep=BLUEx*ldir,aastep=ALPHAx*ldir,zzstep=Zx*ldir;
                int64_t tex_s=0,tt=0,tex_sstep=0,ttstep=0,tex_systep=0,ttystep=0;
                if(tex){tex_s=r3d_quad_at(s0,sxi,sx2,n);tt=r3d_quad_at(t0,txi,tx2,n);tex_sstep=r3d_quad_step(sxi,sx2,n,ldir);ttstep=r3d_quad_step(txi,tx2,n,ldir);tex_systep=syi+(int64_t)n*sxy2;ttystep=tyi+(int64_t)n*txy2;}
                uint32_t da=db+(uint32_t)((y*dp+dl)*dbpp),za=z_enabled?zb+(uint32_t)((y*zp+dl)*2):0;
                for(int x=dl;x<dh;x++){
                    rgba_t shade=r3d_shade(rr,gg,bb,aa),src=shade;int draw=1;uint16_t nz=0;
                    dbg.candidate_pixels++;
                    if(tex){int texture_samples=0,texture_draw=1,texture_key_inhibit=0;rgba_t texel=r3d_sample_texture_cached(ctx,&ts,ctl,tex_s,tt,tex_sstep,ttstep,tex_systep,ttystep,&texture_samples,&texture_draw,&texture_key_inhibit);dbg.texture_samples+=texture_samples;if(!texture_draw){draw=0;}else if(texture_key_inhibit){draw=0;dbg.texel_key_rejects++;}else if(!r3d_apply_tex_alpha(ctl,&texel,shade.a)){draw=0;dbg.alpha_rejects++;}else src=r3d_texture_light(ctl,texel,shade);}
                    if(draw&&af==2)src=r3d_lerp(fog,src,shade.a);
                    if(draw&&z_enabled){nz=r3d_clamp16(zz>>12);if(!r3d_z_test(nz,r3d_vram_read16(m,za),(zc>>R3D_Z_TEST_SHIFT)&7)){draw=0;dbg.z_rejects++;}}
                    if(draw){
                        uint32_t selected_raw=0;
                        int raw_source=0;
                        if(af==1)src=r3d_blend(ctl,src,r3d_read_dst(ctx,da,df));
                        if(dp_constant_source){uint32_t scale_raw=r3d_pack(ctx,df,src,x,y);selected_raw=mach64_3d_dp_select_source(m->dp_src,m->dp_bkgd_clr,m->dp_frgd_clr,scale_raw);src=r3d_unpack(ctx,df,selected_raw,0);raw_source=1;}
                        if(destination_compare&&mach64_3d_destination_compare_inhibits(m->clr_cmp_cntl,m->clr_cmp_clr,m->clr_cmp_mask,r3d_read_dst_raw(ctx,da,df),compare_pixel_mask))draw=0;
                        if(draw){
                            uint32_t source_rgb=((uint32_t)r3d_clamp8(src.r)<<16)|
                                                ((uint32_t)r3d_clamp8(src.g)<<8)|
                                                (uint32_t)r3d_clamp8(src.b);
                            dbg.source_rgb_or|=source_rgb;
                            if(source_rgb)dbg.nonzero_source_pixels++;
                            if(src.r>dbg.source_r_max)dbg.source_r_max=(uint8_t)r3d_clamp8(src.r);
                            if(src.g>dbg.source_g_max)dbg.source_g_max=(uint8_t)r3d_clamp8(src.g);
                            if(src.b>dbg.source_b_max)dbg.source_b_max=(uint8_t)r3d_clamp8(src.b);
                            if(raw_source)r3d_write_dst_raw(ctx,da,df,selected_raw);else r3d_write_dst(ctx,da,df,src);if(z_write)r3d_vram_write16(m,za,nz);dbg.drawn_pixels++;
                        }
                    }
                    da+=dbpp;if(z_enabled)za+=2;
                    rr+=rrstep;gg+=ggstep;bb+=bbstep;aa+=aastep;zz+=zzstep;
                    if(tex){tex_s+=tex_sstep;tt+=ttstep;tex_sstep+=sx2;ttstep+=tx2;tex_systep+=(int64_t)sxy2*ldir;ttystep+=(int64_t)txy2*ldir;}
                }
            }
        }

        /* Y_INC is defined for one step in DST_Y_DIR and X_INC for one step in
         * DST_X_DIR.  Therefore signs are already encoded by the programmed
         * directions: add Y once, then add X once for every Bresenham X step. */
        int lead_dx=r3d_edge_step_scanline(&lead,&le,li,ld,ldir,lead_zero_neg);
        r3d_edge_step_scanline(&trail,&te,ti,td,tdir,trail_zero_neg);
        int lead_steps=lead_dx<0?-lead_dx:lead_dx;

        RED0+=REDy;GREEN0+=GREENy;BLUE0+=BLUEy;ALPHA0+=ALPHAy;Z0+=Zy;
        s0+=syi;syi+=sy2;sxi+=sxy2;
        t0+=tyi;tyi+=ty2;txi+=txy2;

        for(int edge_step=0;edge_step<lead_steps;edge_step++){
            RED0+=REDx;GREEN0+=GREENx;BLUE0+=BLUEx;ALPHA0+=ALPHAx;Z0+=Zx;
            s0+=sxi;sxi+=sx2;syi+=sxy2;
            t0+=txi;txi+=tx2;tyi+=txy2;
        }
        y+=ydir;
    }
    r3d_debug_draw_end(ctx,&dbg,lead,trail,y);

    /*
     * A GT trapezoid is driven by two Bresenham line engines.  Like an ordinary
     * line trajectory, the destination pointer is a live trajectory register,
     * not a write-only start latch.  The Win9x HAL relies on that side effect
     * when it emits the second trapezoid of a triangle: it programs a new
     * TRAIL_X but deliberately leaves DST_Y_X untouched so the leading edge
     * continues from the first trapezoid's endpoint.  Keeping the old start
     * coordinate here restarted every continuation at the apex while carrying
     * forward the updated Bresenham error/interpolators, producing the
     * angle-dependent bow-tie/wedge geometry seen in Flip3D.
     */
    m->dst_y_x=((uint32_t)(lead&0x1fff)<<16)|((uint32_t)y&0x7fff);
    ctx->trail_x=trail;m->dst_bres_err=le;ctx->regs[R3D_TRAIL_BRES_ERR>>2]=te;
    ctx->regs[R3D_RED_START>>2]=mach64_3d_s8_12_encode(RED0);ctx->regs[R3D_GREEN_START>>2]=mach64_3d_s8_12_encode(GREEN0);ctx->regs[R3D_BLUE_START>>2]=mach64_3d_s8_12_encode(BLUE0);ctx->regs[R3D_ALPHA_START>>2]=mach64_3d_s8_12_encode(ALPHA0);ctx->regs[R3D_Z_START>>2]=mach64_3d_s16_12_encode(Z0);
    ctx->regs[R3D_S_START>>2]=(uint32_t)s0;ctx->regs[R3D_S_Y_INC>>2]=(uint32_t)syi;ctx->regs[R3D_S_XINC_START>>2]=(uint32_t)sxi;
    ctx->regs[R3D_T_START>>2]=(uint32_t)t0;ctx->regs[R3D_T_Y_INC>>2]=(uint32_t)tyi;ctx->regs[R3D_T_XINC_START>>2]=(uint32_t)txi;
    r3d_tex_mark_internal(ctx);
}
