#ifndef VID_ATI_MACH64_GTB_HOOK_H
#define VID_ATI_MACH64_GTB_HOOK_H

#include <stdint.h>

/* Shared GTB I/O, clock, PCI, and lifecycle dispatch entry points. */
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
uint8_t mach64_pci_read_legacy(int func, int addr, int len, void *priv);
void mach64_pci_write_legacy(int func, int addr, int len, uint8_t val, void *priv);
void mach64_pci_write_gtb_legacy_dispatch(int func, int addr, int len, uint8_t val, void *priv);
void mach64_pci_add_card_dispatch(uint8_t add_type,
                                  uint8_t (*read)(int func, int addr, int len, void *priv),
                                  void (*write)(int func, int addr, int len, uint8_t val, void *priv),
                                  void *priv, uint8_t *slot);

/* Rage II+ lifecycle and PCI configuration state. */
void mach64_gtb_state_attach(void *priv);
void mach64_gtb_state_detach(void *priv);
uint8_t mach64_gtb_pci_ioconfig_read(void *priv);
void mach64_gtb_pci_ioconfig_write(void *priv, uint8_t val);

#endif
