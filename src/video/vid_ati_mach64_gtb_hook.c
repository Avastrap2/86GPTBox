/*
 * ATI Mach64 GTB/GU compatibility hooks used by the Rage II+ path.
 *
 * ARS2D.bin uses the PCI block-decoded I/O BAR during POST and explicitly
 * reads back the integrated PLL through CLOCK_CNTL_ADDR/CLOCK_CNTL_DATA.
 * The VT2 core predates that usage pattern: BAR1 changes do not currently
 * force an I/O remap, and CLOCK_CNTL_DATA reads return the clock_cntl shadow
 * byte instead of the selected PLL register.  Keep those fixes isolated here
 * until GTB becomes a first-class Mach64 core type.
 */
#include "vid_ati_mach64.h"

#define GTB_PCI_ID          0x4755
#define GTB_PLL_REF_DIV     0x02
#define GTB_VCLK_POST_DIV   0x06
#define GTB_VCLK0_FB_DIV    0x07
#define GTB_PLL_EXT_CNTL    0x0b
#define GTB_IO_HOOKS_MAX    48

extern void ics2595_setclock(void *priv, double clock);
extern void mach64_pci_write_legacy(int func, int addr, int len, uint8_t val, void *priv);

typedef struct mach64_gtb_io_hook_t {
    int used;
    uint16_t base;
    uint16_t size;
    uint8_t (*inb)(uint16_t port, void *priv);
    uint16_t (*inw)(uint16_t port, void *priv);
    uint32_t (*inl)(uint16_t port, void *priv);
    void (*outb)(uint16_t port, uint8_t val, void *priv);
    void (*outw)(uint16_t port, uint16_t val, void *priv);
    void (*outl)(uint16_t port, uint32_t val, void *priv);
    void *priv;
} mach64_gtb_io_hook_t;

static mach64_gtb_io_hook_t gtb_io_hooks[GTB_IO_HOOKS_MAX];
static const uint8_t gtb_postdiv[8] = { 1, 2, 4, 8, 3, 5, 6, 12 };

static int
mach64_gtb_is_card(const mach64_t *mach64)
{
    return mach64 && mach64->pci_id == GTB_PCI_ID;
}

/* Return CLOCK_CNTL byte lane (0..3), or -1 for unrelated ports. */
static int
mach64_gtb_clock_lane(const mach64_gtb_io_hook_t *hook, uint16_t port)
{
    uint16_t offset;

    if (!hook)
        return -1;

    /* PCI block-decoded I/O: CLOCK_CNTL is at base + 0x90. */
    if (hook->size == 0x100) {
        offset = (uint16_t) (port - hook->base);
        if (offset >= 0x90 && offset <= 0x93)
            return offset - 0x90;
        return -1;
    }

    /* Sparse Mach64 layouts for 2EC, 1CC and 1C8 bases. */
    if (hook->size == 4 &&
        (hook->base == 0x4aec || hook->base == 0x49cc || hook->base == 0x49c8))
        return port - hook->base;

    return -1;
}

static int
mach64_gtb_calc_vclk(mach64_t *mach64, unsigned clock, double *freq)
{
    uint8_t ref_div;
    uint8_t fb_div;
    uint8_t post_index;
    uint8_t post_div;

    if (!mach64_gtb_is_card(mach64) || clock > 3 || !freq)
        return 0;

    ref_div = mach64->pll_regs[GTB_PLL_REF_DIV];
    fb_div  = mach64->pll_regs[GTB_VCLK0_FB_DIV + clock];
    if (!ref_div || !fb_div)
        return 0;

    post_index = (mach64->pll_regs[GTB_VCLK_POST_DIV] >> (clock * 2)) & 3;
    if (mach64->pll_regs[GTB_PLL_EXT_CNTL] & (0x10u << clock))
        post_index |= 4;
    post_div = gtb_postdiv[post_index];

    *freq = (2.0 * 14318184.0 * (double) fb_div) /
            ((double) ref_div * (double) post_div);
    return 1;
}

static void
mach64_gtb_refresh_clock(mach64_t *mach64)
{
    double freq;
    unsigned selected;

    if (!mach64_gtb_is_card(mach64) || !mach64->svga.clock_gen)
        return;

    for (unsigned c = 0; c < 4; c++) {
        if (mach64_gtb_calc_vclk(mach64, c, &freq))
            mach64->pll_freq[c] = freq;
    }

    selected = mach64->clock_cntl & 3;
    if (mach64_gtb_calc_vclk(mach64, selected, &freq))
        ics2595_setclock(mach64->svga.clock_gen, freq);
}

static uint8_t
mach64_gtb_hook_inb(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    int lane = mach64_gtb_clock_lane(hook, port);

    if (mach64_gtb_is_card(mach64) && lane == 2)
        return mach64->pll_regs[mach64->pll_addr & 0x0f];

    return hook && hook->inb ? hook->inb(port, hook->priv) : 0xff;
}

static uint16_t
mach64_gtb_hook_inw(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;

    if (mach64_gtb_clock_lane(hook, port) >= 0 ||
        mach64_gtb_clock_lane(hook, port + 1) >= 0)
        return (uint16_t) mach64_gtb_hook_inb(port, priv) |
               ((uint16_t) mach64_gtb_hook_inb(port + 1, priv) << 8);

    return hook && hook->inw ? hook->inw(port, hook->priv) : 0xffff;
}

static uint32_t
mach64_gtb_hook_inl(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_clock_lane(hook, port + i) >= 0) {
            return (uint32_t) mach64_gtb_hook_inb(port, priv) |
                   ((uint32_t) mach64_gtb_hook_inb(port + 1, priv) << 8) |
                   ((uint32_t) mach64_gtb_hook_inb(port + 2, priv) << 16) |
                   ((uint32_t) mach64_gtb_hook_inb(port + 3, priv) << 24);
        }
    }

    return hook && hook->inl ? hook->inl(port, hook->priv) : 0xffffffffu;
}

static void
mach64_gtb_hook_outb(uint16_t port, uint8_t val, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    int lane = mach64_gtb_clock_lane(hook, port);

    if (hook && hook->outb)
        hook->outb(port, val, hook->priv);

    if (!mach64_gtb_is_card(mach64) || lane < 0)
        return;

    if (lane == 1) {
        /* Make the core's CLOCK_CNTL_DATA shadow agree with PLL readback too. */
        uint8_t selected = mach64->pll_regs[mach64->pll_addr & 0x0f];
        mach64->clock_cntl = (mach64->clock_cntl & 0xff00ffffu) |
                             ((uint32_t) selected << 16);
    }

    if (lane == 0 || lane == 2)
        mach64_gtb_refresh_clock(mach64);
}

static void
mach64_gtb_hook_outw(uint16_t port, uint16_t val, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;

    if (mach64_gtb_clock_lane(hook, port) >= 0 ||
        mach64_gtb_clock_lane(hook, port + 1) >= 0) {
        mach64_gtb_hook_outb(port, val & 0xff, priv);
        mach64_gtb_hook_outb(port + 1, val >> 8, priv);
        return;
    }

    if (hook && hook->outw)
        hook->outw(port, val, hook->priv);
}

static void
mach64_gtb_hook_outl(uint16_t port, uint32_t val, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_clock_lane(hook, port + i) >= 0) {
            for (unsigned b = 0; b < 4; b++)
                mach64_gtb_hook_outb(port + b, (val >> (b * 8)) & 0xff, priv);
            return;
        }
    }

    if (hook && hook->outl)
        hook->outl(port, val, hook->priv);
}

static mach64_gtb_io_hook_t *
mach64_gtb_find_hook(uint16_t base, uint16_t size, void *priv)
{
    for (unsigned i = 0; i < GTB_IO_HOOKS_MAX; i++) {
        if (gtb_io_hooks[i].used && gtb_io_hooks[i].base == base &&
            gtb_io_hooks[i].size == size && gtb_io_hooks[i].priv == priv)
            return &gtb_io_hooks[i];
    }
    return NULL;
}

static mach64_gtb_io_hook_t *
mach64_gtb_alloc_hook(void)
{
    for (unsigned i = 0; i < GTB_IO_HOOKS_MAX; i++) {
        if (!gtb_io_hooks[i].used) {
            memset(&gtb_io_hooks[i], 0, sizeof(gtb_io_hooks[i]));
            gtb_io_hooks[i].used = 1;
            return &gtb_io_hooks[i];
        }
    }
    return NULL;
}

void
mach64_io_sethandler_dispatch(uint16_t base, uint16_t size,
                              uint8_t (*inb_cb)(uint16_t port, void *priv),
                              uint16_t (*inw_cb)(uint16_t port, void *priv),
                              uint32_t (*inl_cb)(uint16_t port, void *priv),
                              void (*outb_cb)(uint16_t port, uint8_t val, void *priv),
                              void (*outw_cb)(uint16_t port, uint16_t val, void *priv),
                              void (*outl_cb)(uint16_t port, uint32_t val, void *priv),
                              void *priv)
{
    mach64_gtb_io_hook_t *hook;

    /* Only sparse Mach64 registers (4 bytes) and the block BAR need wrapping. */
    if (size != 4 && size != 0x100) {
        io_sethandler(base, size, inb_cb, inw_cb, inl_cb,
                      outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    hook = mach64_gtb_alloc_hook();
    if (!hook) {
        io_sethandler(base, size, inb_cb, inw_cb, inl_cb,
                      outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    hook->base = base;
    hook->size = size;
    hook->inb = inb_cb;
    hook->inw = inw_cb;
    hook->inl = inl_cb;
    hook->outb = outb_cb;
    hook->outw = outw_cb;
    hook->outl = outl_cb;
    hook->priv = priv;

    io_sethandler(base, size,
                  inb_cb ? mach64_gtb_hook_inb : NULL,
                  inw_cb ? mach64_gtb_hook_inw : NULL,
                  inl_cb ? mach64_gtb_hook_inl : NULL,
                  outb_cb ? mach64_gtb_hook_outb : NULL,
                  outw_cb ? mach64_gtb_hook_outw : NULL,
                  outl_cb ? mach64_gtb_hook_outl : NULL,
                  hook);
}

void
mach64_io_removehandler_dispatch(uint16_t base, uint16_t size,
                                 uint8_t (*inb_cb)(uint16_t port, void *priv),
                                 uint16_t (*inw_cb)(uint16_t port, void *priv),
                                 uint32_t (*inl_cb)(uint16_t port, void *priv),
                                 void (*outb_cb)(uint16_t port, uint8_t val, void *priv),
                                 void (*outw_cb)(uint16_t port, uint16_t val, void *priv),
                                 void (*outl_cb)(uint16_t port, uint32_t val, void *priv),
                                 void *priv)
{
    mach64_gtb_io_hook_t *hook = mach64_gtb_find_hook(base, size, priv);

    if (!hook) {
        io_removehandler(base, size, inb_cb, inw_cb, inl_cb,
                         outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    io_removehandler(base, size,
                     hook->inb ? mach64_gtb_hook_inb : NULL,
                     hook->inw ? mach64_gtb_hook_inw : NULL,
                     hook->inl ? mach64_gtb_hook_inl : NULL,
                     hook->outb ? mach64_gtb_hook_outb : NULL,
                     hook->outw ? mach64_gtb_hook_outw : NULL,
                     hook->outl ? mach64_gtb_hook_outl : NULL,
                     hook);
    memset(hook, 0, sizeof(*hook));
}

void
mach64_ics2595_setclock_dispatch(void *priv, double clock)
{
    mach64_t *mach64 = NULL;
    double corrected;

    for (unsigned i = 0; i < GTB_IO_HOOKS_MAX; i++) {
        if (!gtb_io_hooks[i].used)
            continue;
        mach64_t *candidate = (mach64_t *) gtb_io_hooks[i].priv;
        if (mach64_gtb_is_card(candidate) && candidate->svga.clock_gen == priv) {
            mach64 = candidate;
            break;
        }
    }

    if (mach64 && mach64_gtb_calc_vclk(mach64, mach64->clock_cntl & 3, &corrected))
        clock = corrected;

    ics2595_setclock(priv, clock);
}

/*
 * The legacy PCI writer updates BAR1 but its remap guard is ineffective for
 * BAR1/IOCONFIG writes.  Rewriting the current COMMAND byte is a safe way to
 * make the mature core unmap/remap its I/O handlers after those values change.
 */
void
mach64_pci_write_gtb_legacy_dispatch(int func, int addr, int len, uint8_t val, void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;

    mach64_pci_write_legacy(func, addr, len, val, priv);

    if (!mach64_gtb_is_card(mach64))
        return;

    if (((addr >= PCI_REG_BAR1_BYTE1) && (addr <= PCI_REG_BAR1_BYTE3)) ||
        addr == MACH64_PCI_IOCONFIG) {
        uint8_t command = mach64->pci_regs[PCI_REG_COMMAND];
        mach64_pci_write_legacy(func, PCI_REG_COMMAND, 1, command, priv);
    }
}
