#ifndef VID_ATI_MACH64_GTB_HOOK_H
#define VID_ATI_MACH64_GTB_HOOK_H

#include <stdint.h>

/*
 * This header is force-included only while compiling the legacy Mach64 core
 * and the Rage II+ integration shim.  It redirects legacy I/O registration
 * and clock output through the GTB compatibility layer without forcing GTB
 * behavior onto ordinary VT/VT2 cards.
 */
void mach64_io_sethandler_dispatch(uint16_t base, uint16_t size,
                                   uint8_t (*inb)(uint16_t port, void *priv),
                                   uint16_t (*inw)(uint16_t port, void *priv),
                                   uint32_t (*inl)(uint16_t port, void *priv),
                                   void (*outb)(uint16_t port, uint8_t val, void *priv),
                                   void (*outw)(uint16_t port, uint16_t val, void *priv),
                                   void (*outl)(uint16_t port, uint32_t val, void *priv),
                                   void *priv);
void mach64_io_removehandler_dispatch(uint16_t base, uint16_t size,
                                      uint8_t (*inb)(uint16_t port, void *priv),
                                      uint16_t (*inw)(uint16_t port, void *priv),
                                      uint32_t (*inl)(uint16_t port, void *priv),
                                      void (*outb)(uint16_t port, uint8_t val, void *priv),
                                      void (*outw)(uint16_t port, uint16_t val, void *priv),
                                      void (*outl)(uint16_t port, uint32_t val, void *priv),
                                      void *priv);
void mach64_ics2595_setclock_dispatch(void *priv, double clock);
void mach64_pci_write_gtb_legacy_dispatch(int func, int addr, int len, uint8_t val, void *priv);
void mach64_pci_write_gtb_bar_dispatch(int func, int addr, int len, uint8_t val, void *priv);

/* Rage II+ lifecycle and PCI configuration state. */
void mach64_gtb_state_attach(void *priv);
void mach64_gtb_state_detach(void *priv);
uint8_t mach64_gtb_pci_ioconfig_read(void *priv);
void mach64_gtb_pci_ioconfig_write(void *priv, uint8_t val);

/*
 * Keep the core-level I/O and PLL redirects here.  GTB control-register
 * Block-0 gating is performed explicitly by the Rage II+ shim, so GUI offsets
 * 0x100..0x3ff cannot alias the 0x00..0xff control shadow.
 */
#define io_sethandler      mach64_io_sethandler_dispatch
#define io_removehandler   mach64_io_removehandler_dispatch
#define ics2595_setclock   mach64_ics2595_setclock_dispatch

/*
 * CMake first maps mach64_pci_write_legacy to
 * mach64_pci_write_gtb_legacy_dispatch for the Rage II+ shim.  Chain that
 * token to the implemented BAR guard in vid_ati_mach64_3d.c.  The guard keeps
 * GU BAR1 at its real 0x100-byte alignment before forwarding to the legacy GTB
 * dispatcher; without this chain the VT2 core rounds BAR1 to 1 KiB and Windows
 * can address a different block-I/O base than the one 86Box has mapped.
 *
 * vid_ati_mach64_gtb_hook.c and vid_ati_mach64_3d.c are not force-included
 * with this header, so their real dispatcher symbols are unaffected.
 */
#define mach64_pci_write_gtb_legacy_dispatch mach64_pci_write_gtb_bar_dispatch

#endif
