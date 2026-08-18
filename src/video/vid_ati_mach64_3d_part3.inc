static int64_t r3d_quad_at(int64_t start,int64_t inc,int64_t inc2,int n){
    return start+(int64_t)n*inc+((int64_t)n*(n-1)/2)*inc2;
}

static void r3d_draw_trapezoid(mach64_3d_ctx_t *ctx,uint32_t cmd){
    mach64_t*m=ctx->mach64;uint32_t ctl=ctx->regs[R3D_SCALE_3D_CNTL>>2];int fcn=(ctl>>R3D_FCN_SHIFT)&3,df=r3d_dst_format(m),dbpp=r3d_bytes_per_pixel(df),tf=r3d_tex_format(m),tbpp=r3d_bytes_per_pixel(tf),len=cmd&0x7fff;if(!len||!dbpp||!fcn)return;if(fcn==2&&!tbpp)return;
    int lead=r3d_sign_extend(m->dst_y_x>>16,13),y=r3d_sign_extend(m->dst_y_x,15),ydir=(m->dst_cntl&DST_Y_DIR)?1:-1,ldir=(m->dst_cntl&DST_X_DIR)?1:-1,tdir=(m->dst_cntl&R3D_TRAIL_X_DIR)?1:-1;
    int fill_lr=!!(m->dst_cntl&(1u<<29)),rtedge_dis=!!(m->dst_cntl&(1u<<27));
    if((cmd&0x80000000u)||!ctx->trail_valid){ctx->trail_x=r3d_sign_extend(cmd>>16,13);ctx->trail_valid=1;}int trail=ctx->trail_x,le=(int32_t)m->dst_bres_err,li=(int32_t)m->dst_bres_inc,ld=(int32_t)m->dst_bres_dec,te=(int32_t)ctx->regs[R3D_TRAIL_BRES_ERR>>2],ti=(int32_t)ctx->regs[R3D_TRAIL_BRES_INC>>2],td=(int32_t)ctx->regs[R3D_TRAIL_BRES_DEC>>2];
    int left=m->sc_left_right&0x1fff,right=(m->sc_left_right>>16)&0x1fff,top=m->sc_top_bottom&0x7fff,bottom=(m->sc_top_bottom>>16)&0x7fff;if(right<left){left=0;right=0x1fff;}if(bottom<top){top=0;bottom=0x7fff;}
    uint32_t db=(m->dst_off_pitch&0xfffff)<<3;int dp=((m->dst_off_pitch>>22)&0x3ff)<<3;if(!dp)return;uint32_t zc=ctx->regs[R3D_Z_CNTL>>2],zo=ctx->regs[R3D_Z_OFF_PITCH>>2],zb=(zo&0xfffff)<<3;int zp=((zo>>22)&0x3ff)<<3;
#define RG(name) int64_t name##x=(int32_t)ctx->regs[R3D_##name##_X_INC>>2],name##y=(int32_t)ctx->regs[R3D_##name##_Y_INC>>2],name##0=(int32_t)ctx->regs[R3D_##name##_START>>2]
    RG(RED);RG(GREEN);RG(BLUE);RG(Z);RG(ALPHA);
#undef RG
    int64_t s0=(int64_t)(int32_t)ctx->regs[R3D_S_START>>2]<<5,sxi=(int32_t)ctx->regs[R3D_S_XINC_START>>2],syi=(int32_t)ctx->regs[R3D_S_Y_INC>>2],sx2=(int32_t)ctx->regs[R3D_S_X_INC2>>2],sy2=(int32_t)ctx->regs[R3D_S_Y_INC2>>2],sxy2=(int32_t)ctx->regs[R3D_S_XY_INC2>>2];
    int64_t t0=(int64_t)(int32_t)ctx->regs[R3D_T_START>>2]<<5,txi=(int32_t)ctx->regs[R3D_T_XINC_START>>2],tyi=(int32_t)ctx->regs[R3D_T_Y_INC>>2],tx2=(int32_t)ctx->regs[R3D_T_X_INC2>>2],ty2=(int32_t)ctx->regs[R3D_T_Y_INC2>>2],txy2=(int32_t)ctx->regs[R3D_T_XY_INC2>>2];
    int tex=fcn==2,bilinear=!!(ctl&R3D_BILINEAR_TEX_EN),af=(ctl>>R3D_ALPHA_FOG_SHIFT)&3;rgba_t fog={(m->dp_frgd_clr>>16)&255,(m->dp_frgd_clr>>8)&255,m->dp_frgd_clr&255,255};
    for(int row=0;row<len;row++){
        /* TRAP_FILL_DIR tells which edge is geometrically left/right.  Trust
         * it when the programmed edges agree, otherwise fall back to sorting
         * so malformed/partially initialized commands cannot walk backwards
         * through VRAM. */
        int lo=fill_lr?lead:trail,hi=fill_lr?trail:lead;
        if(lo>hi){int q=lo;lo=hi;hi=q;}
        if(y>=top&&y<=bottom){
            int dl=lo<left?left:lo;
            int right_limit=rtedge_dis?hi:(hi+1);
            int dh=right_limit>right+1?right+1:right_limit;
            for(int x=dl;x<dh;x++){
                int n=(x-lead)*ldir;rgba_t shade=r3d_shade(RED0+REDx*n,GREEN0+GREENx*n,BLUE0+BLUEx*n,ALPHA0+ALPHAx*n),src=shade;
                if(tex){int64_t tex_s=r3d_quad_at(s0,sxi,sx2,n),tex_t=r3d_quad_at(t0,txi,tx2,n);rgba_t texel=r3d_sample_texel(ctx,tex_s,tex_t,tf,bilinear);if(!r3d_apply_tex_alpha(ctl,&texel,shade.a))continue;src=r3d_texture_light(ctl,texel,shade);}
                if(af==2)src=r3d_lerp(fog,src,shade.a);
                uint32_t da = db + (uint32_t)((y * dp + x) * dbpp);
                int pass = 1;
                uint32_t za = 0;
                uint16_t nz = r3d_clamp16((Z0 + Zx * n) >> 12);
                if ((zc & R3D_Z_EN) && zp) {
                    za = zb + (uint32_t)((y * zp + x) * 2);
                    pass = r3d_z_test(nz, r3d_vram_read16(m, za), (zc >> R3D_Z_TEST_SHIFT) & 7);
                }
                if (!pass)
                    continue;

                if (af == 1)
                    src = r3d_blend(ctl, src, r3d_read_dst(ctx, da, df));
                r3d_write_dst(ctx, da, df, src);
                if ((zc & R3D_Z_EN) && (zc & R3D_Z_MASK) && zp)
                    r3d_vram_write16(m, za, nz);
            }
        }
        r3d_edge_step(&lead,&le,li,ld,ldir);r3d_edge_step(&trail,&te,ti,td,tdir);
        RED0+=REDy;GREEN0+=GREENy;BLUE0+=BLUEy;ALPHA0+=ALPHAy;Z0+=Zy;
        s0+=syi;syi+=sy2;sxi+=sxy2;
        t0+=tyi;tyi+=ty2;txi+=txy2;
        y+=ydir;
    }
    ctx->trail_x=trail;m->dst_bres_err=le;ctx->regs[R3D_TRAIL_BRES_ERR>>2]=te;
    ctx->regs[R3D_RED_START>>2]=RED0;ctx->regs[R3D_GREEN_START>>2]=GREEN0;ctx->regs[R3D_BLUE_START>>2]=BLUE0;ctx->regs[R3D_ALPHA_START>>2]=ALPHA0;ctx->regs[R3D_Z_START>>2]=Z0;
    ctx->regs[R3D_S_START>>2]=(uint32_t)(s0>>5);ctx->regs[R3D_S_Y_INC>>2]=(uint32_t)syi;ctx->regs[R3D_S_XINC_START>>2]=(uint32_t)sxi;
    ctx->regs[R3D_T_START>>2]=(uint32_t)(t0>>5);ctx->regs[R3D_T_Y_INC>>2]=(uint32_t)tyi;ctx->regs[R3D_T_XINC_START>>2]=(uint32_t)txi;
}
