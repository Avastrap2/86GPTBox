#define R3D_GTB_DST_BRES_SIGN     (1u << 11)
#define R3D_GTB_POLYGON_RTEDGE_DIS (1u << 12)
#define R3D_GTB_TRAIL_X_DIR        (1u << 13)
#define R3D_GTB_TRAP_FILL_DIR      (1u << 14)
#define R3D_GTB_TRAIL_BRES_SIGN    (1u << 15)

static int64_t r3d_quad_at(int64_t start,int64_t inc,int64_t inc2,int n){
    return start+(int64_t)n*inc+((int64_t)n*(n-1)/2)*inc2;
}
static int64_t r3d_quad_step(int64_t inc,int64_t inc2,int n){
    return inc+(int64_t)n*inc2;
}

/*
 * GT/GTB trapezoid edges use the Bresenham form programmed by the HAL:
 * ERR starts below zero, INC is accumulated once per scanline, and DEC is
 * accumulated once for every horizontal edge step required to bring ERR back
 * below the selected zero threshold.  A shallow edge can therefore move more
 * than one X pixel during a single Y step.  The previous one-if-per-row walker
 * capped the edge slope at |dX/dY| <= 1 and turned near-horizontal cube edges
 * into the characteristic bow-tie/wedge geometry seen in Flip3D.
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
    /* GT/GTB places the polygon controls in DST_CNTL[12:15].  Bits 11 and 15
     * also select the zero-error side of the lead/trail Bresenham walkers. */
    int fill_lr=!!(m->dst_cntl&R3D_GTB_TRAP_FILL_DIR),rtedge_dis=!!(m->dst_cntl&R3D_GTB_POLYGON_RTEDGE_DIS),lead_zero_neg=!!(m->dst_cntl&R3D_GTB_DST_BRES_SIGN),trail_zero_neg=!!(m->dst_cntl&R3D_GTB_TRAIL_BRES_SIGN);
    if((cmd&0x80000000u)||!ctx->trail_valid){ctx->trail_x=r3d_sign_extend(cmd>>16,13);ctx->trail_valid=1;}int trail=ctx->trail_x,le=(int32_t)m->dst_bres_err,li=(int32_t)m->dst_bres_inc,ld=(int32_t)m->dst_bres_dec,te=(int32_t)ctx->regs[R3D_TRAIL_BRES_ERR>>2],ti=(int32_t)ctx->regs[R3D_TRAIL_BRES_INC>>2],td=(int32_t)ctx->regs[R3D_TRAIL_BRES_DEC>>2];
    int left=m->sc_left_right&0x1fff,right=(m->sc_left_right>>16)&0x1fff,top=m->sc_top_bottom&0x7fff,bottom=(m->sc_top_bottom>>16)&0x7fff;if(right<left){left=0;right=0x1fff;}if(bottom<top){top=0;bottom=0x7fff;}
    uint32_t db=(m->dst_off_pitch&0xfffff)<<3;int dp=((m->dst_off_pitch>>22)&0x3ff)<<3;if(!dp)return;
    /* X outside the programmed pitch would only spill into adjacent rows and
     * can turn a malformed/default 8191-pixel scissor into millions of host
     * pixel operations.  Likewise, rows beyond the physical destination
     * allocation only wrap VRAM and cannot be a valid DirectDraw surface. */
    if(left<0)left=0;if(right>=dp)right=dp-1;if(right<left)return;
    {uint32_t stride=(uint32_t)dp*(uint32_t)dbpp,base=db&m->vram_mask;if(stride&&m->vram_size>base){uint32_t rows=(m->vram_size-base)/stride;if(rows&&bottom>=(int)rows)bottom=(int)rows-1;}if(bottom<top)return;}
    uint32_t zc=ctx->regs[R3D_Z_CNTL>>2],zo=ctx->regs[R3D_Z_OFF_PITCH>>2],zb=(zo&0xfffff)<<3;int zp=((zo>>22)&0x3ff)<<3;
#define RG(name) int64_t name##x=(int32_t)ctx->regs[R3D_##name##_X_INC>>2],name##y=(int32_t)ctx->regs[R3D_##name##_Y_INC>>2],name##0=(int32_t)ctx->regs[R3D_##name##_START>>2]
    RG(RED);RG(GREEN);RG(BLUE);RG(Z);RG(ALPHA);
#undef RG
    /* GT/GTB S/T START and all quadratic increments share the same
     * texture-size-normalized register unit. */
    int64_t s0=(int32_t)ctx->regs[R3D_S_START>>2],sxi=(int32_t)ctx->regs[R3D_S_XINC_START>>2],syi=(int32_t)ctx->regs[R3D_S_Y_INC>>2],sx2=(int32_t)ctx->regs[R3D_S_X_INC2>>2],sy2=(int32_t)ctx->regs[R3D_S_Y_INC2>>2],sxy2=(int32_t)ctx->regs[R3D_S_XY_INC2>>2];
    int64_t t0=(int32_t)ctx->regs[R3D_T_START>>2],txi=(int32_t)ctx->regs[R3D_T_XINC_START>>2],tyi=(int32_t)ctx->regs[R3D_T_Y_INC>>2],tx2=(int32_t)ctx->regs[R3D_T_X_INC2>>2],ty2=(int32_t)ctx->regs[R3D_T_Y_INC2>>2],txy2=(int32_t)ctx->regs[R3D_T_XY_INC2>>2];
    int tex=fcn==2,bilinear=!!(ctl&R3D_BILINEAR_TEX_EN),af=(ctl>>R3D_ALPHA_FOG_SHIFT)&3,z_enabled=!!((zc&R3D_Z_EN)&&zp),z_write=!!(z_enabled&&(zc&R3D_Z_MASK));rgba_t fog={(m->dp_frgd_clr>>16)&255,(m->dp_frgd_clr>>8)&255,m->dp_frgd_clr&255,255};
    r3d_tex_state_t ts;if(tex)r3d_tex_state_init(ctx,tf,bilinear,&ts);
    r3d_debug_draw_t dbg;r3d_debug_draw_begin(ctx,&dbg,cmd,lead,trail,y,left,right,top,bottom,dp,len,df,dbpp,tf,tbpp);
    for(int row=0;row<len;row++){
        /*
         * TRAP_FILL_DIR is directional state, not a hint that the two edge
         * coordinates may be sorted.  On real GT/GTB hardware a left-to-right
         * span exists only while trailing X is on or to the right of leading X;
         * right-to-left is the converse.  Sorting crossed edges turns the
         * invalid portion after an apex into a second mirrored triangle, which
         * is exactly the bow-tie/wedge artifact produced by the software path.
         */
        int lo=fill_lr?lead:trail,hi=fill_lr?trail:lead;
        int span_valid=lo<=hi;
        if(span_valid&&y>=top&&y<=bottom){
            int dl=lo<left?left:lo;
            int right_limit=rtedge_dis?hi:(hi+1);
            int dh=right_limit>right+1?right+1:right_limit;
            if(dl<dh){
                /*
                 * START is anchored at the current leading-edge X, while every
                 * *_X_INC is defined in screen-X coordinates.  DST_X_DIR only
                 * tells the Bresenham lead edge which way it walks as Y changes;
                 * it must not reverse interpolation across a span.  Using ldir
                 * here mirrored S/T and colour/Z on every negative-slope lead
                 * edge, so Flip3D could look correct only at special angles.
                 */
                int n=dl-lead;
                int64_t rr=RED0+REDx*n,gg=GREEN0+GREENx*n,bb=BLUE0+BLUEx*n,aa=ALPHA0+ALPHAx*n,zz=Z0+Zx*n;
                int64_t rrstep=REDx,ggstep=GREENx,bbstep=BLUEx,aastep=ALPHAx,zzstep=Zx;
                int64_t ss=0,tt=0,ssstep=0,ttstep=0;
                if(tex){ss=r3d_quad_at(s0,sxi,sx2,n);tt=r3d_quad_at(t0,txi,tx2,n);ssstep=r3d_quad_step(sxi,sx2,n);ttstep=r3d_quad_step(txi,tx2,n);}
                uint32_t da=db+(uint32_t)((y*dp+dl)*dbpp),za=z_enabled?zb+(uint32_t)((y*zp+dl)*2):0;
                for(int x=dl;x<dh;x++){
                    rgba_t shade=r3d_shade(rr,gg,bb,aa),src=shade;int draw=1;uint16_t nz=0;
                    dbg.candidate_pixels++;
                    if(tex){rgba_t texel=r3d_sample_texel_cached(ctx,&ts,ss,tt);dbg.texture_samples+=bilinear?4:1;if(!r3d_apply_tex_alpha(ctl,&texel,shade.a)){draw=0;dbg.alpha_rejects++;}else src=r3d_texture_light(ctl,texel,shade);}
                    if(draw&&af==2)src=r3d_lerp(fog,src,shade.a);
                    if(draw&&z_enabled){nz=r3d_clamp16(zz>>12);if(!r3d_z_test(nz,r3d_vram_read16(m,za),(zc>>R3D_Z_TEST_SHIFT)&7)){draw=0;dbg.z_rejects++;}}
                    if(draw){if(af==1)src=r3d_blend(ctl,src,r3d_read_dst(ctx,da,df));r3d_write_dst(ctx,da,df,src);if(z_write)r3d_vram_write16(m,za,nz);dbg.drawn_pixels++;}
                    da+=dbpp;if(z_enabled)za+=2;
                    rr+=rrstep;gg+=ggstep;bb+=bbstep;aa+=aastep;zz+=zzstep;
                    if(tex){ss+=ssstep;tt+=ttstep;ssstep+=sx2;ttstep+=tx2;}
                }
            }
        }

        /*
         * Move the interpolation origin by the same signed (dx,dy) that the
         * leading edge actually moved.  The earlier code took abs(lead_dx) and
         * always added X increments, which is correct only for DST_X_DIR=1.
         * Negative-slope edges therefore accumulated their START and quadratic
         * derivative state in the wrong direction and became angle dependent.
         */
        int lead_dx=r3d_edge_step_scanline(&lead,&le,li,ld,ldir,lead_zero_neg);
        r3d_edge_step_scanline(&trail,&te,ti,td,tdir,trail_zero_neg);

        RED0+=REDy*ydir;GREEN0+=GREENy*ydir;BLUE0+=BLUEy*ydir;ALPHA0+=ALPHAy*ydir;Z0+=Zy*ydir;
        s0=r3d_quad_at(s0,syi,sy2,ydir);syi+=sy2*ydir;sxi+=sxy2*ydir;
        t0=r3d_quad_at(t0,tyi,ty2,ydir);tyi+=ty2*ydir;txi+=txy2*ydir;

        if(lead_dx){
            RED0+=REDx*lead_dx;GREEN0+=GREENx*lead_dx;BLUE0+=BLUEx*lead_dx;ALPHA0+=ALPHAx*lead_dx;Z0+=Zx*lead_dx;
            s0=r3d_quad_at(s0,sxi,sx2,lead_dx);sxi+=sx2*lead_dx;syi+=sxy2*lead_dx;
            t0=r3d_quad_at(t0,txi,tx2,lead_dx);txi+=tx2*lead_dx;tyi+=txy2*lead_dx;
        }
        y+=ydir;
    }
    r3d_debug_draw_end(ctx,&dbg,lead,trail,y);
    ctx->trail_x=trail;m->dst_bres_err=le;ctx->regs[R3D_TRAIL_BRES_ERR>>2]=te;
    ctx->regs[R3D_RED_START>>2]=RED0;ctx->regs[R3D_GREEN_START>>2]=GREEN0;ctx->regs[R3D_BLUE_START>>2]=BLUE0;ctx->regs[R3D_ALPHA_START>>2]=ALPHA0;ctx->regs[R3D_Z_START>>2]=Z0;
    ctx->regs[R3D_S_START>>2]=(uint32_t)s0;ctx->regs[R3D_S_Y_INC>>2]=(uint32_t)syi;ctx->regs[R3D_S_XINC_START>>2]=(uint32_t)sxi;
    ctx->regs[R3D_T_START>>2]=(uint32_t)t0;ctx->regs[R3D_T_Y_INC>>2]=(uint32_t)tyi;ctx->regs[R3D_T_XINC_START>>2]=(uint32_t)txi;
}
