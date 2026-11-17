/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the AMD Am9513 System Timing Controller.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#ifndef EMU_AM9513_H
#define EMU_AM9513_H

typedef struct am9513_t am9513_t;

/* The C/D input is decoded from A0: 0 = data port, 1 = control port. */
extern uint8_t am9513_read(uint16_t addr, void *priv);
extern void    am9513_write(uint16_t addr, uint8_t val, void *priv);

/* Level changes on OUT n (1-5), as the pin drives the board. */
extern void am9513_set_out_callback(am9513_t *dev, int n,
                                    void (*callback)(void *priv, int level), void *priv);
/* Called when OUT n stops, starts or changes its rate. */
extern void am9513_set_clock_callback(am9513_t *dev, int n,
                                      void (*callback)(void *priv), void *priv);
/* When OUT n is a free-running clock, returns 1 with its period and the time
   of one rising edge, both in TSC units. */
extern int  am9513_get_out_clock(am9513_t *dev, int n, double *period, double *rise);

#ifdef EMU_DEVICE_H
extern const device_t am9513_device;
#endif

#endif /*EMU_AM9513_H*/
