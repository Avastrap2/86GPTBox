#ifndef VID_ATI_MACH64_GTB_HOOK_H
#define VID_ATI_MACH64_GTB_HOOK_H

#include <stdint.h>

/*
 * This header is force-included only while compiling the legacy Mach64 core
 * and the Rage II+ integration shim.  It redirects legacy I/O registration,
 * timing recalculation and clock output through the GTB compatibility layer
 * without forcing GTB behavior onto ordinary VT/VT2 cards.
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
void mach64_svga_recalctimings_dispatch(void *priv);
void mach64_pci_write_gtb_legacy_dispatch(int func, int addr, int len, uint8_t val, void *priv);

/* Rage II+ lifecycle and PCI configuration state. */
void mach64_gtb_state_attach(void *priv);
void mach64_gtb_state_detach(void *priv);
uint8_t mach64_gtb_pci_ioconfig_read(void *priv);
void mach64_gtb_pci_ioconfig_write(void *priv, uint8_t val);

/*
 * The underlying GTB control-register helpers use a byte-sized register file.
 * Redirect only the Rage II+ integration shim to guarded entry points.  The
 * shim's existing extern declarations provide their mach64_t signatures after
 * macro expansion, avoiding a dependency on mach64_t in this force header.
 */
#define io_sethandler      mach64_io_sethandler_dispatch
#define io_removehandler   mach64_io_removehandler_dispatch
#define ics2595_setclock   mach64_ics2595_setclock_dispatch
#define svga_recalctimings mach64_svga_recalctimings_dispatch
#define mach64_gtb_cfg_readb  mach64_gtb_cfg_readb_block0
#define mach64_gtb_cfg_writeb mach64_gtb_cfg_writeb_block0

#endif
