static rgba_t r3d_unpack(mach64_3d_ctx_t *ctx, int f, uint32_t raw, int texture) {
    rgba_t c={0,0,0,255};
    switch(f) {
        case 2: { uint32_t p=texture?ctx->tex_palette[raw&255]:ctx->mach64->svga.pallook[raw&255]; c.r=getcolr(p); c.g=getcolg(p); c.b=getcolb(p); if(texture && (p>>24)) c.a=(p>>24)&255; break; }
        case 3: c.a=(raw&0x8000)?255:0; c.r=((raw>>10)&31)*255/31; c.g=((raw>>5)&31)*255/31; c.b=(raw&31)*255/31; break;
        case 4: c.r=((raw>>11)&31)*255/31; c.g=((raw>>5)&63)*255/63; c.b=(raw&31)*255/31; break;
        case 6: c.a=(raw>>24)&255; c.r=(raw>>16)&255; c.g=(raw>>8)&255; c.b=raw&255; break;
        case 7: c.r=((raw>>5)&7)*255/7; c.g=((raw>>2)&7)*255/7; c.b=(raw&3)*255/3; break;
        case 8: c.r=c.g=c.b=raw&255; break;
        case 15: c.a=((raw>>12)&15)*17; c.r=((raw>>8)&15)*17; c.g=((raw>>4)&15)*17; c.b=(raw&15)*17; break;
        default: break;
    }
    return c;
}
static uint32_t r3d_pack(int f, rgba_t c) {
    c.r=r3d_clamp8(c.r); c.g=r3d_clamp8(c.g); c.b=r3d_clamp8(c.b); c.a=r3d_clamp8(c.a);
    switch(f) {
        case 3: return (c.a>=128?0x8000u:0)|((uint32_t)(c.r>>3)<<10)|((uint32_t)(c.g>>3)<<5)|(c.b>>3);
        case 4: return ((uint32_t)(c.r>>3)<<11)|((uint32_t)(c.g>>2)<<5)|(c.b>>3);
        case 6: return ((uint32_t)c.a<<24)|((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;
        case 7: return ((uint32_t)(c.r>>5)<<5)|((uint32_t)(c.g>>5)<<2)|(c.b>>6);
        case 8: return (c.r*77+c.g*150+c.b*29)>>8;
        case 15:return ((uint32_t)(c.a>>4)<<12)|((uint32_t)(c.r>>4)<<8)|((uint32_t)(c.g>>4)<<4)|(c.b>>4);
        default:return 0;
    }
}
static rgba_t r3d_read_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f) {
    switch(r3d_bytes_per_pixel(f)){case 1:return r3d_unpack(ctx,f,r3d_vram_read8(ctx->mach64,a),0);case 2:return r3d_unpack(ctx,f,r3d_vram_read16(ctx->mach64,a),0);case 4:return r3d_unpack(ctx,f,r3d_vram_read32(ctx->mach64,a),0);default:{rgba_t z={0,0,0,255};return z;}}
}
static void r3d_write_dst(mach64_3d_ctx_t *ctx,uint32_t a,int f,rgba_t c) {
    mach64_t *m=ctx->mach64; if(f==2){uint8_t p=((r3d_clamp8(c.r)>>5)<<5)|((r3d_clamp8(c.g)>>5)<<2)|(r3d_clamp8(c.b)>>6);r3d_vram_write8(m,a,p);return;}
    uint32_t raw=r3d_pack(f,c),old=0,mask=m->write_mask; switch(r3d_bytes_per_pixel(f)){case 1:old=r3d_vram_read8(m,a);raw=(old&~(mask&255))|(raw&(mask&255));r3d_vram_write8(m,a,raw);break;case 2:old=r3d_vram_read16(m,a);raw=(old&~(mask&65535))|(raw&(mask&65535));r3d_vram_write16(m,a,raw);break;case 4:old=r3d_vram_read32(m,a);raw=(old&~mask)|(raw&mask);r3d_vram_write32(m,a,raw);break;}
}
static rgba_t r3d_lerp(rgba_t a,rgba_t b,int t){rgba_t r;t=r3d_clamp8(t);r.r=(a.r*(255-t)+b.r*t+127)/255;r.g=(a.g*(255-t)+b.g*t+127)/255;r.b=(a.b*(255-t)+b.b*t+127)/255;r.a=(a.a*(255-t)+b.a*t+127)/255;return r;}
static int r3d_component(rgba_t c,int ch){return ch==0?c.r:(ch==1?c.g:(ch==2?c.b:c.a));}
static int r3d_blend_factor(int code,rgba_t s,rgba_t d,int ch,int sf){switch(code){case 0:return 0;case 1:return 255;case 2:return sf?r3d_component(d,ch):r3d_component(s,ch);case 3:return 255-(sf?r3d_component(d,ch):r3d_component(s,ch));case 4:return s.a;case 5:return 255-s.a;default:return 255;}}
static rgba_t r3d_blend(uint32_t ctl,rgba_t s,rgba_t d){rgba_t o;int sf=(ctl>>R3D_BLEND_SRC_SHIFT)&7,df=(ctl>>R3D_BLEND_DST_SHIFT)&7,*p[4]={&o.r,&o.g,&o.b,&o.a};for(int ch=0;ch<4;ch++){int sv=r3d_component(s,ch),dv=r3d_component(d,ch);*p[ch]=r3d_clamp8((sv*r3d_blend_factor(sf,s,d,ch,1)+dv*r3d_blend_factor(df,s,d,ch,0)+127)/255);}return o;}
static uint32_t r3d_texture_raw(mach64_3d_ctx_t *ctx,int f,uint32_t a){switch(r3d_bytes_per_pixel(f)){case 1:return r3d_vram_read8(ctx->mach64,a);case 2:return r3d_vram_read16(ctx->mach64,a);case 4:return r3d_vram_read32(ctx->mach64,a);default:return 0;}}
static rgba_t r3d_sample_texel(mach64_3d_ctx_t *ctx,int64_t s,int64_t t,int f,int bilinear){
    uint32_t tsp=ctx->regs[R3D_TEX_SIZE_PITCH>>2];int pl=tsp&15,sl=(tsp>>4)&15,hl=(tsp>>8)&15;if(pl>10)pl=10;if(sl>10)sl=10;if(hl>10)hl=10;if(!pl)pl=sl;if(!hl)hl=sl;int w=1<<pl,h=1<<hl,bpp=r3d_bytes_per_pixel(f),u=(int)(s>>16)&(w-1),v=(int)(t>>16)&(h-1);uint32_t base=ctx->regs[(R3D_TEX_0_OFF+((uint32_t)sl<<2))>>2];if(!bpp){rgba_t z={255,255,255,255};return z;}rgba_t c00=r3d_unpack(ctx,f,r3d_texture_raw(ctx,f,base+(v*w+u)*bpp),1);if(!bilinear)return c00;int u1=(u+1)&(w-1),v1=(v+1)&(h-1),fu=(s>>8)&255,fv=(t>>8)&255;rgba_t c10=r3d_unpack(ctx,f,r3d_texture_raw(ctx,f,base+(v*w+u1)*bpp),1),c01=r3d_unpack(ctx,f,r3d_texture_raw(ctx,f,base+(v1*w+u)*bpp),1),c11=r3d_unpack(ctx,f,r3d_texture_raw(ctx,f,base+(v1*w+u1)*bpp),1);return r3d_lerp(r3d_lerp(c00,c10,fu),r3d_lerp(c01,c11,fu),fv);
}
static int r3d_z_test(uint16_t s,uint16_t d,int t){switch(t&7){case 0:return 0;case 1:return s<d;case 2:return s<=d;case 3:return s==d;case 4:return s>=d;case 5:return s>d;case 6:return s!=d;default:return 1;}}
static int r3d_edge_step(int *x,int *e,int inc,int dec,int dir){int old=*x;if(*e<0)*e+=inc;else{*x+=dir;*e+=dec;}return *x-old;}
static rgba_t r3d_shade(int64_t r,int64_t g,int64_t b,int64_t a){rgba_t c={r3d_clamp8(r>>12),r3d_clamp8(g>>12),r3d_clamp8(b>>12),r3d_clamp8(a>>12)};return c;}
static rgba_t r3d_texture_light(uint32_t ctl,rgba_t t,rgba_t s){int mode=(ctl>>R3D_TEX_LIGHT_SHIFT)&3;if(mode==1){t.r=t.r*s.r/255;t.g=t.g*s.g/255;t.b=t.b*s.b/255;t.a=t.a*s.a/255;}else if(mode==2){rgba_t d=r3d_lerp(s,t,t.a);d.a=s.a;t=d;}return t;}
