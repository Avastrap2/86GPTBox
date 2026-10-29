/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the Lear Siegler ADM-3A terminal.
 *
 * Authors: Avastrap2
 *
 *          Copyright 2026 Avastrap2.
 */
#ifndef VIDEO_ADM3A_H
#define VIDEO_ADM3A_H

/* Wire the terminal's MODEM port: 'tx' carries what it sends (its TxD),
   'rx' what it receives. Needs <86box/serial_line.h>. */
extern void adm3a_attach(void *priv, serial_line_t *tx, serial_line_t *rx);

#endif /*VIDEO_ADM3A_H*/
