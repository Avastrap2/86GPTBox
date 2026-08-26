#include "vid_ati_mach64_3d_control.h"
#include "vid_ati_mach64_3d_expand.h"
#include "vid_ati_mach64_3d_mipmap.h"
#include "vid_ati_mach64_3d_tex_key.h"
#include "vid_ati_mach64_3d_yuv_math.h"

#define R3D_SCALE_PIX_EXPAND   (1u << 0)
#define R3D_SCALE_DITHER_TABLE (1u << 1)
#define R3D_DITHER_EN          (1u << 2)
#define R3D_DITHER_INIT        (1u << 3)
#define R3D_ROUND_EN           (1u << 4)
#define R3D_NEAREST_TEX_VIS    (1u << 9)
#define R3D_APPLE_YUV_MODE     (1u << 10)
#define R3D_RED_DITHER_MAX     (1u << 14)

typedef struct r3d_dither_state_t {
    int valid;
    int y;
    int r_err;
    int g_err;
    int b_err;
} r3d_dither_state_t;

static r3d_dither_state_t r3d_dither_state[MACH64_3D_CONTEXTS];

static rgba_t r3d_yuv_centered_to_rgba(int y,int u,int v,int a){
    mach64_yuv_rgb_t rgb=mach64_yuv_centered_to_rgb(y,u,v);rgba_t c;
    c.r=rgb.r;c.g=rgb.g;c.b=rgb.b;c.a=r3d_clamp8(a);return c;
}
static rgba_t r3d_yuv_to_rgba_ctx(mach64_3d_ctx_t *ctx,int y,int u,int v,int a){
    int signed_uv=ctx&&!!(ctx->regs[R3D_SCALE_3D_CNTL>>2]&R3D_APPLE_YUV_MODE);
    mach64_yuv_rgb_t rgb=mach64_yuv_to_rgb((uint8_t)y,(uint8_t)u,(uint8_t)v,signed_uv);rgba_t c;
    c.r=rgb.r;c.g=rgb.g;c.b=rgb.b;c.a=r3d_clamp8(a);return c;
}
static rgba_t r3d_unpack(mach64_3d_ctx_t *ctx,int f,uint32_t raw,int texture){
    rgba_t c={0,0,0,255};
    int dynamic=!texture||!!(ctx->regs[R3D_SCALE_3D_CNTL>>2]&R3D_SCALE_PIX_EXPAND);
    switch(f){
        case 2:{uint32_t p;if(texture){uint8_t index=mach64_3d_texture_palette_texel_index(ctx->mach64->dp_pix_width,(uint8_t)raw);p=mach64_3d_texture_palette_lookup(ctx->texture_palette,index);c.r=mach64_3d_texture_palette_red(p);c.g=mach64_3d_texture_palette_green(p);c.b=mach64_3d_texture_palette_blue(p);}else{p=ctx->mach64->svga.pallook[raw&255];c.r=getcolr(p);c.g=getcolg(p);c.b=getcolb(p);}break;}
        case 3:c.a=(raw&0x8000)?255:0;c.r=mach64_3d_expand_component((raw>>10)&31,5,dynamic);c.g=mach64_3d_expand_component((raw>>5)&31,5,dynamic);c.b=mach64_3d_expand_component(raw&31,5,dynamic);break;
        case 4:c.r=mach64_3d_expand_component((raw>>11)&31,5,dynamic);c.g=mach64_3d_expand_component((raw>>5)&63,6,dynamic);c.b=mach64_3d_expand_component(raw&31,5,dynamic);break;
        case 6:c.a=(raw>>24)&255;c.r=(raw>>16)&255;c.g=(raw>>8)&255;c.b=raw&255;break;
        case 7:c.r=mach64_3d_expand_component((raw>>5)&7,3,dynamic);c.g=mach64_3d_expand_component((raw>>2)&7,3,dynamic);c.b=mach64_3d_expand_component(raw&3,2,dynamic);break;
        case 8:c.r=c.g=c.b=raw&255;break;
        case 14:return r3d_yuv_to_rgba_ctx(ctx,(raw>>16)&255,(raw>>8)&255,raw&255,(raw>>24)&255);
        case 15:c.a=((raw>>12)&15)*17;c.r=mach64_3d_expand_component((raw>>8)&15,4,dynamic);c.g=mach64_3d_expand_component((raw>>4)&15,4,dynamic);c.b=mach64_3d_expand_component(raw&15,4,dynamic);break;
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
    r3d_dither_state_t *dstate;
    if(ci<0||ci>=MACH64_3D_CONTEXTS)ci=0;
    dstate=&r3d_dither_state[ci];
    c.r=r3d_clamp8(c.r);c.g=r3d_clamp8(c.g);c.b=r3d_clamp8(c.b);c.a=r3d_clamp8(c.a);
    if(!dither)dstate->valid=0;
    if(dither&&!table){
        if(!dstate->valid){dstate->valid=1;dstate->y=y;dstate->r_err=dstate->g_err=dstate->b_err=0;}
        else if(y!=dstate->y){if(ctl&R3D_DITHER_INIT)dstate->r_err=dstate->g_err=dstate->b_err=0;dstate->y=y;}
    }
#define QCOMP(v,bits,err) (dither?(table?r3d_quant_ordered((v),(bits),x,y):r3d_quant_xerr((v),(bits),&(err))):(round?r3d_quant_round((v),(bits)):((v)>>(8-(bits)))))
    switch(f){
        case 3:{int r=QCOMP(c.r,5,dstate->r_err),g=QCOMP(c.g,5,dstate->g_err),b=QCOMP(c.b,5,dstate->b_err);return(c.a>=128?0x8000u:0)|((uint32_t)r<<10)|((uint32_t)g<<5)|(uint32_t)b;}
        case 4:{int r=QCOMP(c.r,5,dstate->r_err),g=QCOMP(c.g,6,dstate->g_err),b=QCOMP(c.b,5,dstate->b_err);return((uint32_t)r<<11)|((uint32_t)g<<5)|(uint32_t)b;}
        case 6:return((uint32_t)c.a<<24)|((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;
        case 7:{int r=QCOMP(c.r,3,dstate->r_err),g=QCOMP(c.g,3,dstate->g_err),b=QCOMP(c.b,2,dstate->b_err);r=(int)mach64_3d_rgb8_red_code((unsigned)r,dither,!!(ctl&R3D_RED_DITHER_MAX));return((uint32_t)r<<5)|((uint32_t)g<<2)|(uint32_t)b;}
        case 8:return(c.r*77+c.g*150+c.b*29)>>8;
        case 15:{int r=QCOMP(c.r,4,dstate->r_err),g=QCOMP(c.g,4,dstate->g_err),b=QCOMP(c.b,4,dstate->b_err);return((uint32_t)(c.a>>4)<<12)|((uint32_t)r<<8)|((uint32_t)g<<4)|(uint32_t)b;}
        default:return 0;
    }
#undef QCOMP
}
static uint32_t r3d_read_dst_raw(mach64_3d_ctx_t *ctx,uint32_t a,int f){
    switch(r3d_bytes_per_pixel(f)){case 1:return r3d_vram_read8(ctx->mach64,a);case 2:return r3d_vram_read16(ctx->mach64,a);case 4:return r3d_vram_read32(ctx->mach64,a);default:return 0;}
}
static rgba_t r3d_read_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f){
    if(r3d_bytes_per_pixel(f))return r3d_unpack(ctx,f,r3d_read_dst_raw(ctx,a,f),0);
    {rgba_t z={0,0,0,255};return z;}
}
static void r3d_write_dst_raw(mach64_3d_ctx_t *ctx,uint32_t a,int f,uint32_t raw){
    mach64_t*m=ctx->mach64;int bpp=r3d_bytes_per_pixel(f);uint32_t old,mask=m->write_mask;
    switch(bpp){
        case 1:{uint32_t pm=mask&0xffu;if(pm!=0xffu){old=r3d_vram_read8(m,a);raw=(old&~pm)|(raw&pm);}r3d_vram_write8(m,a,(uint8_t)raw);break;}
        case 2:{uint32_t pm=mask&0xffffu;if(pm!=0xffffu){old=r3d_vram_read16(m,a);raw=(old&~pm)|(raw&pm);}r3d_vram_write16(m,a,(uint16_t)raw);break;}
        case 4:if(mask!=0xffffffffu){old=r3d_vram_read32(m,a);raw=(old&~mask)|(raw&mask);}r3d_vram_write32(m,a,raw);break;
    }
}
/* Avoid a destination read when WRITE_MASK enables the complete pixel.  The
 * Windows HAL normally leaves it at all ones, so the former read/modify/write
 * doubled framebuffer traffic for the common path. */
static void r3d_write_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f,rgba_t c){
    mach64_t*m=ctx->mach64;int bpp=r3d_bytes_per_pixel(f),x=0,y=0;
    uint32_t base=(m->dst_off_pitch&0xfffff)<<3;int pitch=((m->dst_off_pitch>>22)&0x3ff)<<3;
    if(bpp&&pitch&&a>=base){uint32_t p=(a-base)/(uint32_t)bpp;x=(int)(p%(uint32_t)pitch);y=(int)(p/(uint32_t)pitch);}
    if(f==2){uint8_t p=((r3d_clamp8(c.r)>>5)<<5)|((r3d_clamp8(c.g)>>5)<<2)|(r3d_clamp8(c.b)>>6);r3d_vram_write8(m,a,p);return;}
    r3d_write_dst_raw(ctx,a,f,r3d_pack(ctx,f,c,x,y));
}
static rgba_t r3d_lerp(rgba_t a,rgba_t b,int t){rgba_t r;t=r3d_clamp8(t);r.r=(a.r*(255-t)+b.r*t+127)/255;r.g=(a.g*(255-t)+b.g*t+127)/255;r.b=(a.b*(255-t)+b.b*t+127)/255;r.a=(a.a*(255-t)+b.a*t+127)/255;return r;}
static int r3d_component(rgba_t c,int ch){return ch==0?c.r:(ch==1?c.g:(ch==2?c.b:c.a));}
static int r3d_blend_factor(int code,rgba_t s,rgba_t d,int ch,int sf){switch(code){case 0:return 0;case 1:return 255;case 2:return sf?r3d_component(d,ch):r3d_component(s,ch);case 3:return 255-(sf?r3d_component(d,ch):r3d_component(s,ch));case 4:return s.a;case 5:return 255-s.a;default:return 255;}}
static rgba_t r3d_blend(uint32_t ctl,rgba_t s,rgba_t d){rgba_t o;int sf=(ctl>>R3D_BLEND_SRC_SHIFT)&7,df=(ctl>>R3D_BLEND_DST_SHIFT)&7,*p[4]={&o.r,&o.g,&o.b,&o.a};for(int ch=0;ch<4;ch++){int sv=r3d_component(s,ch),dv=r3d_component(d,ch);*p[ch]=r3d_clamp8((sv*r3d_blend_factor(sf,s,d,ch,1)+dv*r3d_blend_factor(df,s,d,ch,0)+127)/255);}return o;}

typedef struct r3d_tex_state_t {
    int f,bpp,pl,sl,hl,w,wmask,hmask,bilinear,coord_shift;
    uint32_t base;
} r3d_tex_state_t;

typedef struct r3d_texel_sample_t {
    rgba_t color;
    int nearest_inhibits;
    int any_inhibits;
} r3d_texel_sample_t;

/* TEX_SIZE_PITCH is invariant for a complete draw command.  Cache the largest
 * map geometry once; mip-enabled draws derive the selected map from S/T
 * derivatives at the pixel being sampled. */
static void r3d_tex_state_init(mach64_3d_ctx_t *ctx,int f,int bilinear,r3d_tex_state_t *ts){
    uint32_t tsp=ctx->regs[R3D_TEX_SIZE_PITCH>>2];int pl=tsp&15,sl=(tsp>>4)&15,hl=(tsp>>8)&15;
    if(pl>10)pl=10;
    if(sl>10)sl=10;
    if(hl>10)hl=10;
    if(!hl)hl=sl;
    ts->f=f;ts->bpp=r3d_bytes_per_pixel(f);ts->pl=pl;ts->sl=sl;ts->hl=hl;ts->w=1<<pl;ts->wmask=ts->w-1;ts->hmask=(1<<hl)-1;ts->bilinear=bilinear;
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
static void r3d_tex_state_select_lod(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *largest,int lod,int bilinear,r3d_tex_state_t *selected){
    int pl,sl,hl;
    if(lod<0)lod=0;
    if(lod>largest->sl)lod=largest->sl;
    *selected=*largest;
    pl=largest->pl-lod;if(pl<0)pl=0;
    sl=largest->sl-lod;if(sl<0)sl=0;
    hl=largest->hl-lod;if(hl<0)hl=0;
    selected->pl=pl;selected->sl=sl;selected->hl=hl;
    selected->w=1<<pl;selected->wmask=selected->w-1;selected->hmask=(1<<hl)-1;
    selected->bilinear=bilinear;selected->coord_shift=26-sl;
    selected->base=ctx->regs[(R3D_TEX_0_OFF+((uint32_t)sl<<2))>>2];
}
static uint32_t r3d_texture_raw_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,uint32_t a){
    switch(ts->bpp){case 1:return r3d_vram_read8(ctx->mach64,a);case 2:return r3d_vram_read16(ctx->mach64,a);case 4:return r3d_vram_read32(ctx->mach64,a);default:return 0;}
}
static r3d_texel_sample_t r3d_texel_at_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,int u,int v){
    mach64_t *m=ctx->mach64;r3d_texel_sample_t sample;uint32_t raw;int inhibits;
    memset(&sample,0,sizeof(sample));sample.color.a=255;
    if(ts->f==11){uint32_t a=ts->base+(uint32_t)((v*ts->w+(u&~1))*2);raw=r3d_vram_read32(m,a);sample.color=r3d_yuv_to_rgba_ctx(ctx,mach64_yuyv_y(raw,(unsigned)u),mach64_yuyv_u(raw),mach64_yuyv_v(raw),255);inhibits=mach64_3d_texel_key_match(m->clr_cmp_cntl,m->clr_cmp_clr,m->clr_cmp_mask,(uint8_t)sample.color.r,(uint8_t)sample.color.g,(uint8_t)sample.color.b);}
    else{raw=r3d_texture_raw_cached(ctx,ts,ts->base+(uint32_t)((v*ts->w+u)*ts->bpp));sample.color=r3d_unpack(ctx,ts->f,raw,1);if(ts->f==2){uint8_t index=mach64_3d_texture_palette_texel_index(m->dp_pix_width,(uint8_t)raw);inhibits=mach64_3d_texel_key_match_index(m->clr_cmp_cntl,m->clr_cmp_clr,m->clr_cmp_mask,index);}else inhibits=mach64_3d_texel_key_match(m->clr_cmp_cntl,m->clr_cmp_clr,m->clr_cmp_mask,(uint8_t)sample.color.r,(uint8_t)sample.color.g,(uint8_t)sample.color.b);}
    sample.nearest_inhibits=sample.any_inhibits=inhibits;return sample;
}
/*
 * Keep S/T in the native GT normalized-coordinate scale through the quadratic
 * recurrence and convert to texels only at the sampler.  START and derivative
 * values share the raw register alignment expected by the HAL setup stream.
 */
static r3d_texel_sample_t r3d_sample_texel_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *ts,int64_t s,int64_t t){
    int sh=ts->coord_shift;int u=(int)(s>>sh)&ts->wmask,v=(int)(t>>sh)&ts->hmask;
    r3d_texel_sample_t c00=r3d_texel_at_cached(ctx,ts,u,v),result=c00;if(!ts->bilinear)return result;
    int u1=(u+1)&ts->wmask,v1=(v+1)&ts->hmask;int fu=(int)(s>>(sh-8))&255,fv=(int)(t>>(sh-8))&255;
    r3d_texel_sample_t c10=r3d_texel_at_cached(ctx,ts,u1,v),c01=r3d_texel_at_cached(ctx,ts,u,v1),c11=r3d_texel_at_cached(ctx,ts,u1,v1);
    result.color=r3d_lerp(r3d_lerp(c00.color,c10.color,fu),r3d_lerp(c01.color,c11.color,fu),fv);
    result.any_inhibits=c00.any_inhibits||c10.any_inhibits||c01.any_inhibits||c11.any_inhibits;
    if(fv>=128)result.nearest_inhibits=(fu>=128)?c11.nearest_inhibits:c01.nearest_inhibits;
    else result.nearest_inhibits=(fu>=128)?c10.nearest_inhibits:c00.nearest_inhibits;
    return result;
}
static int r3d_texture_visibility(uint32_t ctl,r3d_texel_sample_t sample){
    return mach64_3d_texel_visibility_inhibits(!!(ctl&R3D_NEAREST_TEX_VIS),sample.nearest_inhibits,sample.any_inhibits);
}
static rgba_t r3d_sample_texture_cached(mach64_3d_ctx_t *ctx,const r3d_tex_state_t *largest,uint32_t ctl,int64_t s,int64_t t,int64_t dsdx,int64_t dtdx,int64_t dsdy,int64_t dtdy,int *samples,int *draw,int *key_inhibit){
    mach64_3d_mip_lod_t lod;mach64_3d_texture_filter_t filter;r3d_tex_state_t a,b;r3d_texel_sample_t sample;rgba_t empty={0,0,0,0};int blend=(ctl>>R3D_TEX_BLEND_SHIFT)&R3D_TEX_BLEND_MASK;
    if(samples)*samples=1;
    if(draw)*draw=1;
    if(key_inhibit)*key_inhibit=0;
    lod=mach64_3d_mip_lod(dsdx,dtdx,dsdy,dtdy,largest->coord_shift,largest->sl);
    filter=mach64_3d_texture_filter(lod.minifying,(unsigned)blend,!!(ctl&R3D_BILINEAR_TEX_EN));
    if(filter==MACH64_3D_TEXTURE_FILTER_NONE){if(samples)*samples=0;if(draw)*draw=0;return empty;}
    if(!lod.minifying){a=*largest;a.bilinear=filter==MACH64_3D_TEXTURE_FILTER_BILINEAR;if(samples)*samples=a.bilinear?4:1;sample=r3d_sample_texel_cached(ctx,&a,s,t);if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,sample);return sample.color;}
    if(ctl&R3D_MIP_MAP_DISABLE){/* With one map present, modes 0/1 use nearest sampling and the two documented 2x2 modes use that same map bilinearly. */a=*largest;a.bilinear=filter==MACH64_3D_TEXTURE_FILTER_BILINEAR;if(samples)*samples=a.bilinear?4:1;sample=r3d_sample_texel_cached(ctx,&a,s,t);if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,sample);return sample.color;}
    if(blend==1&&lod.floor_lod<largest->sl){r3d_texel_sample_t ca,cb,combined;r3d_tex_state_select_lod(ctx,largest,lod.floor_lod,0,&a);r3d_tex_state_select_lod(ctx,largest,lod.floor_lod+1,0,&b);ca=r3d_sample_texel_cached(ctx,&a,s,t);cb=r3d_sample_texel_cached(ctx,&b,s,t);combined.color=r3d_lerp(ca.color,cb.color,lod.fraction);combined.any_inhibits=ca.any_inhibits||cb.any_inhibits;combined.nearest_inhibits=(lod.nearest_lod==lod.floor_lod)?ca.nearest_inhibits:cb.nearest_inhibits;if(samples)*samples=2;if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,combined);return combined.color;}
    if(blend==2){r3d_tex_state_select_lod(ctx,largest,lod.nearest_lod,1,&a);if(samples)*samples=4;sample=r3d_sample_texel_cached(ctx,&a,s,t);if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,sample);return sample.color;}
    if(blend==3){int next=lod.nearest_lod==lod.floor_lod?lod.floor_lod+1:lod.floor_lod;if(next>largest->sl)next=largest->sl;r3d_tex_state_select_lod(ctx,largest,next,1,&a);sample=r3d_sample_texel_cached(ctx,&a,s,t);sample.color.a=lod.nearest_lod==lod.floor_lod?lod.fraction:255-lod.fraction;if(samples)*samples=4;if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,sample);return sample.color;}
    r3d_tex_state_select_lod(ctx,largest,lod.nearest_lod,0,&a);sample=r3d_sample_texel_cached(ctx,&a,s,t);if(key_inhibit)*key_inhibit=r3d_texture_visibility(ctl,sample);return sample.color;
}
static int r3d_apply_tex_alpha(uint32_t ctl,rgba_t *tex,int interp_alpha){
    if(mach64_3d_uses_lod_alpha((ctl>>R3D_ALPHA_FOG_SHIFT)&R3D_ALPHA_FOG_MASK,(ctl>>R3D_TEX_BLEND_SHIFT)&R3D_TEX_BLEND_MASK))return 1;
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
