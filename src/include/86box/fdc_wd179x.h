/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the Western Digital FD179X and FD1771 floppy
 *          disk formatter/controllers.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#ifndef EMU_FDC_WD179X_H
#define EMU_FDC_WD179X_H

typedef struct wd179x_t wd179x_t;

enum {
    WD_FD179X = 0, /* FD1791 to FD1797 */
    WD_FD1771      /* single density only */
};

/* clk_hz is the CLK input: 2 MHz for 8" drives, 1 MHz for minifloppies.
   The chip drives 86Box floppy drives 0 to 3 through their bit cells. */
extern wd179x_t *wd179x_create(int chip, double clk_hz);
extern void      wd179x_close(wd179x_t *dev);
/* Master reset: loads 03h into the command register (a Restore follows
   when MR goes inactive) and 01h into the sector register. */
extern void      wd179x_reset(wd179x_t *dev);

/* A1, A0: 0 status/command, 1 track, 2 sector, 3 data. */
extern uint8_t   wd179x_read(wd179x_t *dev, int reg);
extern void      wd179x_write(wd179x_t *dev, int reg, uint8_t val);

extern int       wd179x_get_drq(wd179x_t *dev);
extern int       wd179x_get_intrq(wd179x_t *dev);
extern void      wd179x_set_callbacks(wd179x_t *dev, void (*intrq)(void *priv, int level),
                                      void (*drq)(void *priv, int level), void *priv);

/* Board signals: the selected drive (-1 = none) and side, DDEN (1 = single
   density, FM) and the head load one-shot that drives HLT, in microseconds
   after HLD rises (0 = HLT always true). */
extern void      wd179x_set_drive(wd179x_t *dev, int drive, int side);
extern void      wd179x_set_dden(wd179x_t *dev, int fm);
extern void      wd179x_set_hlt_delay(wd179x_t *dev, double us);
extern void      wd179x_set_clock(wd179x_t *dev, double clk_hz);
/* TEST (1 = normal; 0 = fast stepping, no head settling) and, on the
   FD1771, DINT (0 inhibits Write Track). */
extern void      wd179x_set_test(wd179x_t *dev, int test);
extern void      wd179x_set_dint(wd179x_t *dev, int dint);
/* Board wiring: the READY input for a drive, step pulses, and whether the
   drive's head is settled on its track (it reads nothing otherwise), when
   the board does not just connect the selected drive; NULL for any keeps
   the plain connection. */
extern void      wd179x_set_board(wd179x_t *dev, int (*ready)(void *priv, int drive),
                                  void (*step)(void *priv, int drive, int dir),
                                  int (*settled)(void *priv, int drive), void *priv);
extern int       wd179x_get_hld(wd179x_t *dev);

#endif /*EMU_FDC_WD179X_H*/
