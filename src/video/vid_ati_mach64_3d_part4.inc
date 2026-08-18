static int r3d_unique_register(uint32_t b){return (b>=R3D_TRAIL_BRES_ERR&&b<=R3D_ALPHA_TST_CNTL)||(b>=R3D_TEX_0_OFF&&b<=R3D_SCALE_3D_CNTL)||(b>=R3D_TEX_PALETTE_INDEX&&b<=R3D_TEX_PALETTE)||(b>=R3D_RED_X_INC&&b<=R3D_ALPHA_START);}
static uint32_t r3d_merge_write(uint32_t old,uint32_t a,uint32_t v,uint32_t type){int n=type==FIFO_WRITE_DWORD?4:(type==FIFO_WRITE_WORD?2:1);for(int i=0;i<n;i++){unsigned s=((a+i)&3)*8;old=(old&~(0xffu<<s))|(((v>>(i*8))&255u)<<s);}return old;}
static int r3d_write_complete(uint32_t a,uint32_t type){return type==FIFO_WRITE_DWORD?((a&3)==0):(type==FIFO_WRITE_WORD?((a&3)==2):((a&3)==3));}
int mach64_3d_write(mach64_t *m,uint32_t a,uint32_t v,uint32_t type){
    mach64_3d_ctx_t *ctx=r3d_find(m);if(!ctx)return 0;a&=0x3ff;uint32_t b=a&~3u;
    if(b==R3D_LEAD_BRES_LNTH||b==R3D_LEAD_BRES_LNTH_ALIAS){uint32_t i=R3D_LEAD_BRES_LNTH>>2,cmd=r3d_merge_write(ctx->regs[i],a,v,type);ctx->regs[i]=cmd;if(cmd&R3D_DRAW_TRAP){if(r3d_write_complete(a,type))r3d_draw_trapezoid(ctx,cmd);return 1;}return b==R3D_LEAD_BRES_LNTH_ALIAS;}
    if(!r3d_unique_register(b))return 0;uint32_t i=b>>2;ctx->regs[i]=r3d_merge_write(ctx->regs[i],a,v,type);if(b==R3D_TEX_PALETTE_INDEX)ctx->tex_palette_index=(uint8_t)ctx->regs[i];if(b==R3D_TEX_PALETTE&&r3d_write_complete(a,type)){uint32_t p=ctx->regs[i];ctx->tex_palette[ctx->tex_palette_index++]=0xff000000u|(p&0xffffffu);}return 1;
}
