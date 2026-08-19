#define R3D_SCALE_DITHER_TABLE (1u << 1)
#define R3D_DITHER_EN          (1u << 2)
#define R3D_DITHER_INIT        (1u << 3)
#define R3D_ROUND_EN           (1u << 4)

typedef struct r3d_dither_state_t {
    int valid;
    int y;
    int r_err;
    int g_err;
    int b_err;
} r3d_dither_state_t;

static r3d_dither_state_t r3d_dither_state[MACH64_3D_CONTEXTS];

static rgba_t r3d_yuv_to_rgba(int y,int u,int v,int a){
    int d=u-128,e=v-128;rgba_t c;
    c.r=r3d_clamp8(y+((359*e)>>8));
    c.g=r3d_clamp8(y-((88*d+183*e)>>8));
    c.b=r3d_clamp8(y+((454*d)>>8));
    c.a=r3d_clamp8(a);return c;
}
static rgba_t r3d_unpack(mach64_3d_ctx_t *ctx,int f,uint32_t raw,int texture){
    rgba_t c={0,0,0,255};
    switch(f){
        case 2:{uint32_t p=ctx->mach64->svga.pallook[raw&255];c.r=getcolr(p);c.g=getcolg(p);c.b=getcolb(p);break;}
        case 3:c.a=(raw&0x8000)?255:0;c.r=((raw>>10)&31)*255/31;c.g=((raw>>5)&31)*255/31;c.b=(raw&31)*255/31;break;
        case 4:c.r=((raw>>11)&31)*255/31;c.g=((raw>>5)&63)*255/63;c.b=(raw&31)*255/31;break;
        case 6:c.a=(raw>>24)&255;c.r=(raw>>16)&255;c.g=(raw>>8)&255;c.b=raw&255;break;
        case 7:c.r=((raw>>5)&7)*255/7;c.g=((raw>>2)&7)*255/7;c.b=(raw&3)*255/3;break;
        case 8:c.r=c.g=c.b=raw&255;break;
        case 14:return r3d_yuv_to_rgba((raw>>16)&255,(raw>>8)&255,raw&255,(raw>>24)&255);
        case 15:c.a=((raw>>12)&15)*17;c.r=((raw>>8)&15)*17;c.g=((raw>>4)&15)*17;c.b=(raw&15)*17;break;
        default:break;
    }
    return c;
}

/*
 * SCALE_3D_CNTL[4:1] controls the final 24-bit-to-framebuffer reduction:
 * bit 1 selects X error diffusion or the fixed two-dimensional table, bit 2
 * enables dithering, bit 3 resets the X-diffusion error at each scanline, and
 * bit 4 selects rounding when dithering is disabled.  The ATI register guide
 * documents the modes but not the contents of the fixed 2-D table, so use a
 * conventional 4x4 Bayer ordering for that fixed-table path.
 */
static int r3d_quant_ordered(int v,int bits,int x,int y){
    static const uint8_t bayer4[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    int drop=8-bits,max=(1<<bits)-1,span=(1<<drop)-1;
    int add=(bayer4[((y&3)<<2)|(x&3)]*span+7)/15;
    int q=(r3d_clamp8(v)+add)>>drop;
    return q>max?max:q;
}
static int r3d_quant_round(int v,int bits){
    int drop=8-bits,max=(1<<bits)-1;
    int q=(r3d_clamp8(v)+(1<<(drop-1)))>>drop;
    return q>max?max:q;
}
static int r3d_quant_xerr(int v,int bits,int *err){
    int max=(1<<bits)-1;
    int a=r3d_clamp8(v+*err);
    int q=(a>>(8-bits));
    if(q>max)q=max;
    /* Feed the signed quantization error into the following X pixel. */
    int reconstructed=(q*255+max/2)/max;
    *err=a-reconstructed;
    return q;
}
static uint32_t r3d_pack(mach64_3d_ctx_t *ctx,int f,rgba_t c,int x,int y){
    uint32_t ctl=ctx->regs[R3D_SCALE_3D_CNTL>>2];
    int ci=(int)(ctx-mach64_3d_ctx),dither=!!(ctl&R3D_DITHER_EN),table=!!(ctl&R3D_SCALE_DITHER_TABLE),round=!!(ctl&R3D_ROUND_EN);
    r3d_dither_state_t *ds;
    if(ci<0||ci>=MACH64_3D_CONTEXTS)ci=0;
    ds=&r3d_dither_state[ci];
    c.r=r3d_clamp8(c.r);c.g=r3d_clamp8(c.g);c.b=r3d_clamp8(c.b);c.a=r3d_clamp8(c.a);
    if(!dither)ds->valid=0;
    if(dither&&!table){
        if(!ds->valid){ds->valid=1;ds->y=y;ds->r_err=ds->g_err=ds->b_err=0;}
        else if(y!=ds->y){if(ctl&R3D_DITHER_INIT)ds->r_err=ds->g_err=ds->b_err=0;ds->y=y;}
    }
#define QCOMP(v,bits,err) (dither?(table?r3d_quant_ordered((v),(bits),x,y):r3d_quant_xerr((v),(bits),&(err))):(round?r3d_quant_round((v),(bits)):((v)>>(8-(bits)))))
    switch(f){
        case 3:{int r=QCOMP(c.r,5,ds->r_err),g=QCOMP(c.g,5,ds->g_err),b=QCOMP(c.b,5,ds->b_err);return(c.a>=128?0x8000u:0)|((uint32_t)r<<10)|((uint32_t)g<<5)|(uint32_t)b;}
        case 4:{int r=QCOMP(c.r,5,ds->r_err),g=QCOMP(c.g,6,ds->g_err),b=QCOMP(c.b,5,ds->b_err);return((uint32_t)r<<11)|((uint32_t)g<<5)|(uint32_t)b;}
        case 6:return((uint32_t)c.a<<24)|((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;
        case 7:{int r=QCOMP(c.r,3,ds->r_err),g=QCOMP(c.g,3,ds->g_err),b=QCOMP(c.b,2,ds->b_err);return((uint32_t)r<<5)|((uint32_t)g<<2)|(uint32_t)b;}
        case 8:return(c.r*77+c.g*150+c.b*29)>>8;
        case 15:{int r=QCOMP(c.r,4,ds->r_err),g=QCOMP(c.g,4,ds->g_err),b=QCOMP(c.b,4,ds->b_err);return((uint32_t)(c.a>>4)<<12)|((uint32_t)r<<8)|((uint32_t)g<<4)|(uint32_t)b;}
        default:return 0;
    }
#undef QCOMP
}
static rgba_t r3d_read_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f){
    switch(r3d_bytes_per_pixel(f)){case 1:return r3d_unpack(ctx,f,r3d_vram_read8(ctx->mach64,a),0);case 2:return r3d_unpack(ctx,f,r3d_vram_read16(ctx->mach64,a),0);case 4:return r3d_unpack(ctx,f,r3d_vram_read32(ctx->mach64,a),0);default:{rgba_t z={0,0,0,255};return z;}}
}
/* Avoid a destination read when WRITE_MASK enables the complete pixel.  The
 * Windows HAL normally leaves it at all ones, so the former read/modify/write
 * doubled framebuffer traffic for the common path. */
static void r3d_write_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f,rgba_t c){
    mach64_t*m=ctx->mach64;int bpp=r3d_bytes_per_pixel(f),x=0,y=0;
    uint32_t base=(m->dst_off_pitch&0xfffff)<<3;int pitch=((m->dst_off_pitch>>22)&0x3ff)<<3;
    if(bpp&&pitch&&a>=base){uint32_t p=(a-base)/(uint32_t)bpp;x=(int)(p%(uint32_t)pitch);y=(int)(p/(uint32_t)pitch);}
    if(f==2){uint8_t p=((r3d_clamp8(c.r)>>5)<<5)|((r3d_clamp8(c.g)>>5)<<2)|(r3d_clamp8(c.b)>>6);r3d_vram_write8(m,a,p);return;}
    uint32_t raw=r3d_pack(ctx,f,c,x,y),old,mask=m->write_mask;
    switch(bpp){
        case 1:{uint32_t pm=mask&0xffu;if(pm!=0xffu){old=r3d_vram_read8(m,a);raw=(old&~pm)|(raw&pm);}r3d_vram_write8(m,a,(uint8_t)raw);break;}
        case 2:{uint32_t pm=mask&0xffffu;if(pm!=0xffffu){old=r3d_vram_read16(m,a);raw=(old&~pm)|(raw&pm);}r3d_vram_write16(m,a,(uint16_t)raw);break;}
        case 4:if(mask!=0xffffffffu){old=r3d_vram_read32(m,a);raw=(old&~mask)|(raw&mask);}r3d_vram_write32(m,a,raw);break;
    }
}
static rgba_t r3d_lerp(rgba_t a,rgba_t b,int t){rgba_t r;t=r3d_clamp8(t);r.r=(a.r*(255-t)+b.r*t+127)/255;r.g=(a.g*(255-t)+b.g*t+127)/255;r.b=(a.b*(255-t)+b.b*t+127)/255;r.a=(a.a*(255-t)+b.a*t+127)/255;return r;}
static int r3d_component(rgba_t c,int ch){return ch==0?c.r:(ch==1?c.g:(ch==2?c.b:c.a));}
static int r3d_blend_factor(int code,rgba_t s,rgba_t d,int ch,int sf){switch(code){case 0:return 0;case 1:return 255;case 2:return sf?r3d_component(d,ch):r3d_component(s,ch);case 3:return 255-(sf?r3d_component(d,ch):r3d_component(s,ch));case 4:return s.a;case 5:return 255-s.a;default:return 255;}}
static rgba_t r3d_blend(uint32_t ctl,rgba_t s,rgba_t d){rgba_t o;int sf=(ctl>>R3D_BLEND_SRC_SHIFT)&7,df=(ctl>>R3D_BLEND_DST_SHIFT)&7,*p[4]={&o.r,&o.g,&o.b,&o.a};for(int ch=0;ch<4;ch++){int sv=r3d_component(s,ch),dv=r3d_component(d,ch);*p[ch]=r3d_clamp8((sv*r3d_blend_factor(sf,s,d,ch,1)+dv*r3d_blend_factor(df,s,d,ch,0)+127)/255);}return o;}

typedef struct r3d_tex_state_t {
    int f,bpp,w,wmask,hmask,bilinear,coord_shift;
    uint32_t base;
} r3d_tex_state_t;

/* TEX_SIZE_PITCH and the selected mip base are invariant for a complete draw
 * command.  Cache them once per trapezoid rather than decoding them for every
 * texel. */
static void r3d_tex_state_init(mach64_3d_ctx_t *ctx,int f,int bilinear,r3d_tex_state_t *ts){
    uint32_t tsp=ctx->regs[R3D_TEX_SIZE_PITCH>>2];int pl=tsp&15,sl=(tsp>>4)&15,hl=(tsp>>8)&15;
    if(pl>10)pl=10;if(sl>10)sl=10;if(hl>10)hl=10;if(!hl)hl=sl;
    ts->f=f;ts->bpp=r3d_bytes_per_pixel(f);ts->w=1<<pl;ts->wmask=ts->w-1;ts->hmask=(1<<hl)-1;ts->bilinear=bilinear;
    /*
     * GT/GTB S/T is a normalized 10.11 coordinate domain, not a direct texel
     * index.  The full texture spans 1024 coordinate units regardless of mip
     * size.  In the raw DWORD image START therefore has an effective 16-bit
     * binary point (the 10.11 field occupies bits 25:5), and converting that
     * normalized coordinate to a TEX_SIZE=2^sl texel index requires
     * shifting by 16 + 10 - sl = 26 - sl.
     *
     * The captured Windows 95 HAL stream confirms this directly: with
     * TEX_SIZE_PITCH=0x888 (256x256), cube vertices program S/T START values
     * near 0x00000000 or 0x04000000.  A >>18 decode maps those endpoints to
     * texels 0 and 256; >>17 maps the same face across 512 texels and repeats
     * the logo twice.
     */
    ts->coord_shift=26-sl;
    ts->base=ctx->regs[(R3D_TEX_0_OFF+((uint32_t)sl<<2))>>2];
}
static uint32_t r3d_texture_raw_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,uint32_t a){
    switch(ts->bpp){case 1:return r3d_vram_read8(ctx->mach64,a);case 2:return r3d_vram_read16(ctx->mach64,a);case 4:return r3d_vram_read32(ctx->mach64,a);default:return 0;}
}
static rgba_t r3d_texel_at_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,int u,int v){
    if(ts->f==11){uint32_t a=ts->base+(uint32_t)((v*ts->w+(u&~1))*2);int y0=r3d_vram_read8(ctx->mach64,a),uu=r3d_vram_read8(ctx->mach64,a+1),y1=r3d_vram_read8(ctx->mach64,a+2),vv=r3d_vram_read8(ctx->mach64,a+3);return r3d_yuv_to_rgba((u&1)?y1:y0,uu,vv,255);}
    return r3d_unpack(ctx,ts->f,r3d_texture_raw_cached(ctx,ts,ts->base+(uint32_t)((v*ts->w+u)*ts->bpp)),1);
}
/*
 * Keep S/T in the native GT normalized-coordinate scale through the quadratic
 * recurrence and convert to texels only at the sampler.  START and derivative
 * values share the raw register alignment expected by the HAL setup stream.
 */
static rgba_t r3d_sample_texel_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,int64_t s,int64_t t){
    int sh=ts->coord_shift;
    int u=(int)(s>>sh)&ts->wmask,v=(int)(t>>sh)&ts->hmask;rgba_t c00=r3d_texel_at_cached(ctx,ts,u,v);if(!ts->bilinear)return c00;
    int u1=(u+1)&ts->wmask,v1=(v+1)&ts->hmask;
    int fu=(int)(s>>(sh-8))&255,fv=(int)(t>>(sh-8))&255;
    rgba_t c10=r3d_texel_at_cached(ctx,ts,u1,v),c01=r3d_texel_at_cached(ctx,ts,u,v1),c11=r3d_texel_at_cached(ctx,ts,u1,v1);
    return r3d_lerp(r3d_lerp(c00,c10,fu),r3d_lerp(c01,c11,fu),fv);
}
static int r3d_apply_tex_alpha(uint32_t ctl,rgba_t *tex,int interp_alpha){
    if(!(ctl&R3D_TEX_MAP_AEN)){tex->a=interp_alpha;return 1;}
    if(ctl&R3D_TEX_AMASK_AEN){int mask=tex->a&1;if(!(ctl&R3D_TEX_AMASK_MODE)){if(!mask)return 0;tex->a=interp_alpha;}else tex->a=mask?255:interp_alpha;}
    return 1;
}
static int r3d_z_test(uint16_t s,uint16_t d,int t){switch(t&7){case 0:return 0;case 1:return s<d;case 2:return s<=d;case 3:return s==d;case 4:return s>=d;case 5:return s>d;case 6:return s!=d;default:return 1;}}
/* GT color/alpha registers store signed S.8.12 values in bits 24:4.  The
 * register image is therefore four bits left of a conventional S.8.12 integer:
 * bits 23:16 are the eight color integer bits and bits 15:4 are fractional.
 * Decode an 8-bit interpolated component from the raw trajectory state at
 * bit 16, matching the register diagrams rather than treating bit 12 as the
 * DWORD binary point. */
static rgba_t r3d_shade(int64_t r,int64_t g,int64_t b,int64_t a){rgba_t c={r3d_clamp8(r>>16),r3d_clamp8(g>>16),r3d_clamp8(b>>16),r3d_clamp8(a>>16)};return c;}
static rgba_t r3d_texture_light(uint32_t ctl,rgba_t t,rgba_t s){
    if(ctl&R3D_COLOR_OVERRIDE){int a=t.a;t=s;t.a=a;return t;}
    int mode=(ctl>>R3D_TEX_LIGHT_SHIFT)&3;if(mode==1){t.r=t.r*s.r/255;t.g=t.g*s.g/255;t.b=t.b*s.b/255;}else if(mode==2){rgba_t d=r3d_lerp(s,t,t.a);d.a=s.a;t=d;}return t;
}