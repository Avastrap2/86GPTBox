/*
 * 86GPTBox - ATI 3D RAGE (Mach64 GT/GTB) software 3D pipeline.
 *
 * This is intentionally kept separate from the mature Mach64 2D engine.
 * The FIFO wrapper dispatches only GT-only register writes here.
 */
#ifndef VID_ATI_MACH64_3D_H
#define VID_ATI_MACH64_3D_H

#include "vid_ati_mach64.h"

void mach64_3d_attach(mach64_t *mach64);
void mach64_3d_detach(mach64_t *mach64);

/* Lightweight shared trace entry point used by GTB POST/mode-set hooks. */
void mach64_3d_trace_external(mach64_t *mach64, char op, unsigned width,
                              uint32_t addr, uint32_t value, int claimed);

/*
 * Returns non-zero when the write belongs to the 3D RAGE register file and
 * has been consumed.  type is one of FIFO_WRITE_BYTE/WORD/DWORD.
 */
int mach64_3d_write(mach64_t *mach64, uint32_t addr, uint32_t val, uint32_t type);

/*
 * Returns non-zero when addr belongs to the GT/GTB 3D register file.  val is
 * the complete little-endian DWORD containing that register.  The MMIO shim
 * selects the requested byte/word lanes from it.
 */
int mach64_3d_read(mach64_t *mach64, uint32_t addr, uint32_t *val);

/* Behavior-neutral snapshots for correlating legacy ColorFill destinations
 * with the GT/GTB overlay fetch registers. */
void mach64_3d_rect_debug_begin(mach64_t *mach64);
void mach64_3d_rect_debug_end(mach64_t *mach64);

#endif
