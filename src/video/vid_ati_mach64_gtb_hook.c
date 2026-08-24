/*
 * ATI Mach64 GTB/GU compatibility hooks used by the Rage II+ path.
 *
 * The ARS2D GU option ROM uses GTB control registers that the VT2 core does
 * not currently model.  Its SGRAM sizing loop toggles EXT_MEM_CNTL[29] and
 * requires the value to read back; dropping that write can leave POST in the
 * sizing loop.  Keep GTB-only latches here while reusing the mature VT2 VGA,
 * CRTC, framebuffer and 2D implementation.
 */
#include "vid_ati_mach64.h"
#include "vid_ati_mach64_gtb_hook.h"

#define GTB_PCI_ID        0x4755
#define GTB_PLL_REF_DIV   0x02
#define GTB_VCLK_POST_DIV 0x06
#define GTB_VCLK0_FB_DIV  0x07
#define GTB_PLL_EXT_CNTL  0x0b
#define GTB_IO_HOOKS_MAX  48
#define GTB_STATES_MAX    4
#define GTB_BLOCK_SIZE    0x0100

extern void ics2595_setclock(void *priv, double clock);
extern void mach64_3d_trace_external(mach64_t *mach64, char op, unsigned width,
                                     uint32_t addr, uint32_t value, int claimed);

typedef struct mach64_gtb_state_t {
    int used;
    int ports_attached;
    mach64_t *dev;
    uint8_t control[0x100];
    uint8_t genena;
    uint8_t genvs;
    uint8_t pci_ioconfig;
} mach64_gtb_state_t;

typedef struct mach64_gtb_io_hook_t {
    int used;
    int block;
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

static mach64_gtb_state_t gtb_states[GTB_STATES_MAX];
static mach64_gtb_io_hook_t gtb_io_hooks[GTB_IO_HOOKS_MAX];
static const uint8_t gtb_postdiv[8] = { 1, 2, 4, 8, 3, 5, 6, 12 };

static int
mach64_gtb_is_card(const mach64_t *mach64)
{
    return mach64 && mach64->pci_id == GTB_PCI_ID;
}

/*
 * The legacy core has historically used io_base in two different forms:
 * mach64_common_init() stores the actual sparse port (02ECh/01CCh/01C8h),
 * while PCI IOCONFIG writes replace it with the 2-bit selector (0/1/2).
 * Keep the GTB state in the hardware-visible selector form and translate only
 * at the callback boundary.  This avoids changing ordinary VT/VT2 behavior.
 */
static uint8_t
mach64_gtb_sparse_selector(uint32_t io_base)
{
    switch (io_base) {
        case MACH64_IO_BASE_2EC:
        case 0:
            return 0;
        case MACH64_IO_BASE_1CC:
        case 1:
            return 1;
        case MACH64_IO_BASE_1C8:
        case 2:
            return 2;
        default:
            return 0;
    }
}

static uint32_t
mach64_gtb_sparse_port_base(uint8_t selector)
{
    switch (selector & 3) {
        case 1:
            return MACH64_IO_BASE_1CC;
        case 2:
            return MACH64_IO_BASE_1C8;
        default:
            return MACH64_IO_BASE_2EC;
    }
}

static mach64_gtb_state_t *
mach64_gtb_get_state(mach64_t *mach64, int create)
{
    mach64_gtb_state_t *free_state = NULL;

    if (!mach64)
        return NULL;

    for (unsigned i = 0; i < GTB_STATES_MAX; i++) {
        if (gtb_states[i].used && gtb_states[i].dev == mach64)
            return &gtb_states[i];
        if (!gtb_states[i].used && !free_state)
            free_state = &gtb_states[i];
    }

    if (!create || !free_state)
        return NULL;

    memset(free_state, 0, sizeof(*free_state));
    free_state->used = 1;
    free_state->dev = mach64;
    free_state->genena = 0x08;
    free_state->genvs = 0x01;
    free_state->pci_ioconfig = (uint8_t) ((mach64->use_block_decoded_io & 0x04) |
                                          mach64_gtb_sparse_selector(mach64->io_base));
    return free_state;
}

/* GT/GTB non-GUI registers absent from the current VT2 register switch. */
static int
mach64_gtb_shadow_offset(uint16_t offset)
{
    if (offset >= 0x100)
        return 0;

    switch (offset & 0xfc) {
        case 0x28: /* TIMER_CONFIG */
        case 0x2c: /* MEM_BUF_CNTL */
        case 0x34: /* MEM_ADDR_CONFIG */
        case 0x38: /* CRT_TRAP */
        case 0x3c: /* I2C_CNTL_0 */
        case 0x54: /* DSP2_CONFIG */
        case 0x58: /* DSP2_ON_OFF */
        case 0x5c: /* CRTC2_OFF_PITCH / reserved on GTB */
        case 0x74:
        case 0x78: /* GP_IO; handled electrically below */
        case 0x7c: /* HW_DEBUG */
        case 0x88: /* SCRATCH_REG2 */
        case 0x8c: /* SCRATCH_REG3 */
        case 0x94: /* CNFG_STAT1 */
        case 0x98: /* CNFG_STAT2 */
        case 0x9c:
        case 0xa0: /* BUS_CNTL */
        case 0xa4: /* LCD_INDEX / reserved on GTB */
        case 0xa8: /* LCD_DATA / reserved on GTB */
        case 0xac: /* EXT_MEM_CNTL */
        case 0xbc: /* I2C_CNTL_1 */
        case 0xc8: /* EXT_DAC_REGS */
        case 0xcc:
        case 0xd4: /* CUSTOM_MACRO_CNTL */
        case 0xd8:
        case 0xe8: /* CRC_SIG */
        case 0xec:
        case 0xf0:
        case 0xf4:
        case 0xf8:
        case 0xfc:
            return 1;
        default:
            return 0;
    }
}

static int
mach64_gtb_block_offset(const mach64_gtb_io_hook_t *hook, uint16_t port, uint16_t *offset)
{
    if (!hook || !hook->block || port < hook->base ||
        (uint32_t) port >= (uint32_t) hook->base + hook->size)
        return 0;

    if (offset)
        *offset = (uint16_t) (port - hook->base);
    return 1;
}

static uint16_t
mach64_gtb_callback_port(const mach64_gtb_io_hook_t *hook, uint16_t port)
{
    uint16_t offset;

    if (mach64_gtb_block_offset(hook, port, &offset))
        return offset;
    return port;
}

static uint32_t
mach64_gtb_callback_enter(const mach64_gtb_io_hook_t *hook, mach64_t *mach64)
{
    uint32_t old;
    mach64_gtb_state_t *state;

    if (!mach64)
        return 0;

    old = mach64->io_base;
    if (!mach64_gtb_is_card(mach64) || !hook || hook->block)
        return old;

    state = mach64_gtb_get_state(mach64, 1);
    if (state)
        mach64->io_base = mach64_gtb_sparse_port_base(state->pci_ioconfig & 3);
    return old;
}

static void
mach64_gtb_callback_leave(const mach64_gtb_io_hook_t *hook, mach64_t *mach64,
                          uint32_t old)
{
    if (mach64_gtb_is_card(mach64) && hook && !hook->block)
        mach64->io_base = old;
}

static void
mach64_gtb_trace_io(const mach64_gtb_io_hook_t *hook, mach64_t *mach64,
                    char op, unsigned width, uint16_t port, uint32_t value)
{
    uint16_t offset;
    uint32_t addr;

    if (!mach64_gtb_is_card(mach64) || !hook)
        return;

    if (mach64_gtb_block_offset(hook, port, &offset))
        addr = 0x2000u | offset;
    else
        addr = 0x10000u | port;

    mach64_3d_trace_external(mach64, op, width, addr, value, 1);
}

/* Return CLOCK_CNTL byte lane (0..3), or -1 for unrelated ports. */
static int
mach64_gtb_clock_lane(const mach64_gtb_io_hook_t *hook, uint16_t port)
{
    uint16_t offset;

    if (!hook)
        return -1;

    if (mach64_gtb_block_offset(hook, port, &offset)) {
        if (offset >= 0x90 && offset <= 0x93)
            return offset - 0x90;
        return -1;
    }

    if (hook->size == 4 &&
        (hook->base == 0x4aec || hook->base == 0x49cc || hook->base == 0x49c8))
        return port - hook->base;

    return -1;
}

static int
mach64_gtb_bank_readb(const mach64_t *mach64, uint16_t offset, uint8_t *val)
{
    uint32_t page;

    if (!mach64 || offset < 0xb4 || offset > 0xbb)
        return 0;

    switch (offset) {
        case 0xb4:
            page = (uint32_t) mach64->bank_w[0] >> 15;
            break;
        case 0xb6:
            page = (uint32_t) mach64->bank_w[1] >> 15;
            break;
        case 0xb8:
            page = (uint32_t) mach64->bank_r[0] >> 15;
            break;
        case 0xba:
            page = (uint32_t) mach64->bank_r[1] >> 15;
            break;
        default:
            page = 0;
            break;
    }

    if (val)
        *val = page & 0xff;
    return 1;
}

/*
 * GTB/GU exposes GP_IO through both block I/O (register 0x78) and sparse I/O
 * selector 0x1e.  The inherited Mach64 sparse decoder has no 0x7Axx case, so a
 * sparse GP_IO access otherwise falls through to register 0x00 and can corrupt
 * CRTC_H_TOTAL_DISP while a utility is merely probing DDC/GPIO state.
 *
 * GTB-family software uses GP_IO pairs B/4 (pins 11/4) and A/C (pins 10/12)
 * for bit-banged I2C probing.  Direction bits are the corresponding pin bits
 * plus 16.  A cleared direction bit releases the open-drain line; when driven,
 * the data bit selects low/high.  Do not reuse VT3's later pins 13/12 here.
 */
#define GTB_GP_IO_SDA_4      (1u << 4)
#define GTB_GP_IO_SCL_A      (1u << 10)
#define GTB_GP_IO_SCL_B      (1u << 11)
#define GTB_GP_IO_SDA_C      (1u << 12)
#define GTB_GP_IO_DIR_SDA_4  (1u << 20)
#define GTB_GP_IO_DIR_SCL_A  (1u << 26)
#define GTB_GP_IO_DIR_SCL_B  (1u << 27)
#define GTB_GP_IO_DIR_SDA_C  (1u << 28)

static int
mach64_gtb_gp_io_offset(uint16_t offset)
{
    return offset >= 0x78 && offset <= 0x7b;
}

static int
mach64_gtb_sparse_gp_io_offset(const mach64_gtb_io_hook_t *hook,
                                uint16_t port, uint16_t *offset)
{
    uint16_t sparse_base;

    if (!hook || hook->block || hook->size != 4 || port < hook->base ||
        (uint32_t) port >= (uint32_t) hook->base + 4u)
        return 0;

    /* Sparse selector 0x1e is GP_IO.  Accept all three IOCONFIG base choices. */
    sparse_base = (uint16_t) (hook->base - (0x1eu << 10));
    if (sparse_base != MACH64_IO_BASE_2EC &&
        sparse_base != MACH64_IO_BASE_1CC &&
        sparse_base != MACH64_IO_BASE_1C8)
        return 0;

    if (offset)
        *offset = (uint16_t) (0x78u + (port - hook->base));
    return 1;
}

static int
mach64_gtb_gp_io_pin_high(uint32_t gp_io, uint32_t data_bit, uint32_t dir_bit)
{
    return !(gp_io & dir_bit) || !!(gp_io & data_bit);
}

static void
mach64_gtb_gp_io_drive(mach64_t *mach64)
{
    int scl_a;
    int scl_b;
    int sda_4;
    int sda_c;

    if (!mach64 || !mach64->i2c)
        return;

    scl_a = mach64_gtb_gp_io_pin_high(mach64->gp_io, GTB_GP_IO_SCL_A, GTB_GP_IO_DIR_SCL_A);
    scl_b = mach64_gtb_gp_io_pin_high(mach64->gp_io, GTB_GP_IO_SCL_B, GTB_GP_IO_DIR_SCL_B);
    sda_4 = mach64_gtb_gp_io_pin_high(mach64->gp_io, GTB_GP_IO_SDA_4, GTB_GP_IO_DIR_SDA_4);
    sda_c = mach64_gtb_gp_io_pin_high(mach64->gp_io, GTB_GP_IO_SDA_C, GTB_GP_IO_DIR_SDA_C);

    /* The emulator has one external I2C/DDC bus.  Either GTB candidate pin pair
     * may drive it; an unused pair is input/released and therefore contributes
     * a logical high to this wired-AND combination. */
    i2c_gpio_set(mach64->i2c, scl_a && scl_b, sda_4 && sda_c);
}

static uint8_t
mach64_gtb_gp_io_readb(mach64_t *mach64, uint16_t offset)
{
    uint8_t ret;
    unsigned lane;

    if (!mach64 || !mach64_gtb_gp_io_offset(offset))
        return 0xff;

    lane = offset & 3;
    ret = (mach64->gp_io >> (lane * 8)) & 0xff;

    if (mach64->i2c) {
        int scl = i2c_gpio_get_scl(mach64->i2c);
        int sda = i2c_gpio_get_sda(mach64->i2c);

        if (lane == 0) {
            /* GP_IO_4 is the first GTB data pin. */
            ret &= ~0x10u;
            if (sda)
                ret |= 0x10u;
        } else if (lane == 1) {
            /* GP_IO_A/B are clock candidates and GP_IO_C is data. */
            ret &= ~0x1cu;
            if (scl)
                ret |= 0x0cu;
            if (sda)
                ret |= 0x10u;
        }
    }

    return ret;
}

static void
mach64_gtb_gp_io_writeb(mach64_t *mach64, uint16_t offset, uint8_t val)
{
    unsigned lane;
    unsigned shift;
    uint32_t mask;

    if (!mach64 || !mach64_gtb_gp_io_offset(offset))
        return;

    lane = offset & 3;
    shift = lane * 8;
    mask = 0xffu << shift;
    mach64->gp_io = (mach64->gp_io & ~mask) | ((uint32_t) val << shift);
    mach64_gtb_gp_io_drive(mach64);
}

static int
mach64_gtb_calc_vclk(mach64_t *mach64, unsigned clock, double *freq)
{
    uint8_t ref_div;
    uint8_t fb_div;
    uint8_t post_index;

    if (!mach64_gtb_is_card(mach64) || clock > 3 || !freq)
        return 0;

    ref_div = mach64->pll_regs[GTB_PLL_REF_DIV];
    fb_div = mach64->pll_regs[GTB_VCLK0_FB_DIV + clock];
    if (!ref_div || !fb_div)
        return 0;

    post_index = (mach64->pll_regs[GTB_VCLK_POST_DIV] >> (clock * 2)) & 3;
    if (mach64->pll_regs[GTB_PLL_EXT_CNTL] & (0x10u << clock))
        post_index |= 4;

    *freq = (2.0 * 14318184.0 * (double) fb_div) /
            ((double) ref_div * (double) gtb_postdiv[post_index]);

    if (!(*freq > 1000000.0 && *freq < 250000000.0))
        return 0;
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

static int
mach64_gtb_hook_special(const mach64_gtb_io_hook_t *hook, uint16_t port)
{
    uint16_t offset;

    if (mach64_gtb_clock_lane(hook, port) >= 0)
        return 1;
    if (mach64_gtb_sparse_gp_io_offset(hook, port, &offset))
        return 1;
    if (!mach64_gtb_block_offset(hook, port, &offset))
        return 0;
    if (offset >= 0xb4 && offset <= 0xbb)
        return 1;
    return mach64_gtb_shadow_offset(offset);
}

/*
 * Return one byte without adding a trace event.  Word/dword reads use this to
 * compose special GTB registers while still emitting exactly one event for
 * the guest-visible I/O transaction instead of two/four synthetic byte reads.
 */
static uint8_t
mach64_gtb_hook_inb_raw(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t offset;
    uint16_t callback_port;
    uint32_t old_io_base;
    int lane = mach64_gtb_clock_lane(hook, port);
    uint8_t ret;

    if (mach64_gtb_is_card(mach64) && lane == 2)
        return mach64->pll_regs[mach64->pll_addr & 0x0f];

    if (mach64_gtb_is_card(mach64) && mach64_gtb_sparse_gp_io_offset(hook, port, &offset))
        return mach64_gtb_gp_io_readb(mach64, offset);

    if (mach64_gtb_is_card(mach64) && mach64_gtb_block_offset(hook, port, &offset)) {
        uint8_t bank;
        if (mach64_gtb_bank_readb(mach64, offset, &bank))
            return bank;
        if (mach64_gtb_gp_io_offset(offset))
            return mach64_gtb_gp_io_readb(mach64, offset);
    }

    if (mach64_gtb_is_card(mach64) &&
        mach64_gtb_block_offset(hook, port, &offset) &&
        mach64_gtb_shadow_offset(offset)) {
        mach64_gtb_state_t *state = mach64_gtb_get_state(mach64, 1);
        return state ? state->control[offset] : 0;
    }

    if (!hook || !hook->inb)
        return 0xff;

    callback_port = mach64_gtb_callback_port(hook, port);
    old_io_base = mach64_gtb_callback_enter(hook, mach64);
    ret = hook->inb(callback_port, hook->priv);
    mach64_gtb_callback_leave(hook, mach64, old_io_base);
    return ret;
}

static uint8_t
mach64_gtb_hook_inb(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint8_t ret = mach64_gtb_hook_inb_raw(port, priv);

    /* Lower-case 'i' is guest I/O read; upper-case 'I' is guest I/O write. */
    mach64_gtb_trace_io(hook, mach64, 'i', 1, port, ret);
    return ret;
}

static uint16_t
mach64_gtb_hook_inw(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t callback_port;
    uint32_t old_io_base;
    uint16_t ret;

    if (mach64_gtb_hook_special(hook, port) || mach64_gtb_hook_special(hook, port + 1)) {
        ret = (uint16_t) mach64_gtb_hook_inb_raw(port, priv) |
              ((uint16_t) mach64_gtb_hook_inb_raw(port + 1, priv) << 8);
    } else if (!hook || !hook->inw) {
        ret = 0xffff;
    } else {
        callback_port = mach64_gtb_callback_port(hook, port);
        old_io_base = mach64_gtb_callback_enter(hook, mach64);
        ret = hook->inw(callback_port, hook->priv);
        mach64_gtb_callback_leave(hook, mach64, old_io_base);
    }

    mach64_gtb_trace_io(hook, mach64, 'i', 2, port, ret);
    return ret;
}

static uint32_t
mach64_gtb_hook_inl(uint16_t port, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t callback_port;
    uint32_t old_io_base;
    uint32_t ret;
    int special = 0;

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_hook_special(hook, port + i)) {
            special = 1;
            break;
        }
    }

    if (special) {
        ret = (uint32_t) mach64_gtb_hook_inb_raw(port, priv) |
              ((uint32_t) mach64_gtb_hook_inb_raw(port + 1, priv) << 8) |
              ((uint32_t) mach64_gtb_hook_inb_raw(port + 2, priv) << 16) |
              ((uint32_t) mach64_gtb_hook_inb_raw(port + 3, priv) << 24);
    } else if (!hook || !hook->inl) {
        ret = 0xffffffffu;
    } else {
        callback_port = mach64_gtb_callback_port(hook, port);
        old_io_base = mach64_gtb_callback_enter(hook, mach64);
        ret = hook->inl(callback_port, hook->priv);
        mach64_gtb_callback_leave(hook, mach64, old_io_base);
    }

    mach64_gtb_trace_io(hook, mach64, 'i', 4, port, ret);
    return ret;
}

static void
mach64_gtb_hook_outb(uint16_t port, uint8_t val, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t offset;
    uint16_t callback_port;
    uint32_t old_io_base;
    int lane = mach64_gtb_clock_lane(hook, port);

    mach64_gtb_trace_io(hook, mach64, 'I', 1, port, val);

    if (mach64_gtb_is_card(mach64) && mach64_gtb_sparse_gp_io_offset(hook, port, &offset)) {
        mach64_gtb_gp_io_writeb(mach64, offset, val);
        return;
    }

    if (mach64_gtb_is_card(mach64) && mach64_gtb_block_offset(hook, port, &offset)) {
        if (mach64_gtb_gp_io_offset(offset)) {
            mach64_gtb_gp_io_writeb(mach64, offset, val);
            return;
        }
        if (mach64_gtb_shadow_offset(offset)) {
            mach64_gtb_state_t *state = mach64_gtb_get_state(mach64, 1);
            if (state)
                state->control[offset] = val;
            return;
        }
    }

    if (hook && hook->outb) {
        callback_port = mach64_gtb_callback_port(hook, port);
        old_io_base = mach64_gtb_callback_enter(hook, mach64);
        hook->outb(callback_port, val, hook->priv);
        mach64_gtb_callback_leave(hook, mach64, old_io_base);
    }

    if (!mach64_gtb_is_card(mach64) || lane < 0)
        return;

    if (lane == 1) {
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
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t callback_port;
    uint32_t old_io_base;

    mach64_gtb_trace_io(hook, mach64, 'I', 2, port, val);

    if (mach64_gtb_hook_special(hook, port) || mach64_gtb_hook_special(hook, port + 1)) {
        mach64_gtb_hook_outb(port, val & 0xff, priv);
        mach64_gtb_hook_outb(port + 1, val >> 8, priv);
        return;
    }
    if (!hook || !hook->outw)
        return;

    callback_port = mach64_gtb_callback_port(hook, port);
    old_io_base = mach64_gtb_callback_enter(hook, mach64);
    hook->outw(callback_port, val, hook->priv);
    mach64_gtb_callback_leave(hook, mach64, old_io_base);
}

static void
mach64_gtb_hook_outl(uint16_t port, uint32_t val, void *priv)
{
    mach64_gtb_io_hook_t *hook = (mach64_gtb_io_hook_t *) priv;
    mach64_t *mach64 = hook ? (mach64_t *) hook->priv : NULL;
    uint16_t callback_port;
    uint32_t old_io_base;

    mach64_gtb_trace_io(hook, mach64, 'I', 4, port, val);

    for (unsigned i = 0; i < 4; i++) {
        if (mach64_gtb_hook_special(hook, port + i)) {
            for (unsigned b = 0; b < 4; b++)
                mach64_gtb_hook_outb(port + b, (val >> (b * 8)) & 0xff, priv);
            return;
        }
    }
    if (!hook || !hook->outl)
        return;

    callback_port = mach64_gtb_callback_port(hook, port);
    old_io_base = mach64_gtb_callback_enter(hook, mach64);
    hook->outl(callback_port, val, hook->priv);
    mach64_gtb_callback_leave(hook, mach64, old_io_base);
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
                              uint8_t (*inb_cb)(uint16_t, void *),
                              uint16_t (*inw_cb)(uint16_t, void *),
                              uint32_t (*inl_cb)(uint16_t, void *),
                              void (*outb_cb)(uint16_t, uint8_t, void *),
                              void (*outw_cb)(uint16_t, uint16_t, void *),
                              void (*outl_cb)(uint16_t, uint32_t, void *),
                              void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    mach64_gtb_io_hook_t *hook;

    if (!mach64 || mach64->type != MACH64_GTB) {
        io_sethandler(base, size, inb_cb, inw_cb, inl_cb, outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    if (size != 4 && size != GTB_BLOCK_SIZE) {
        io_sethandler(base, size, inb_cb, inw_cb, inl_cb, outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    hook = mach64_gtb_alloc_hook();
    if (!hook) {
        io_sethandler(base, size, inb_cb, inw_cb, inl_cb, outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    hook->block = (size == GTB_BLOCK_SIZE);
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
                                 uint8_t (*inb_cb)(uint16_t, void *),
                                 uint16_t (*inw_cb)(uint16_t, void *),
                                 uint32_t (*inl_cb)(uint16_t, void *),
                                 void (*outb_cb)(uint16_t, uint8_t, void *),
                                 void (*outw_cb)(uint16_t, uint16_t, void *),
                                 void (*outl_cb)(uint16_t, uint32_t, void *),
                                 void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    mach64_gtb_io_hook_t *hook;

    if (!mach64 || mach64->type != MACH64_GTB) {
        io_removehandler(base, size, inb_cb, inw_cb, inl_cb, outb_cb, outw_cb, outl_cb, priv);
        return;
    }

    hook = mach64_gtb_find_hook(base, size, priv);

    if (!hook) {
        io_removehandler(base, size, inb_cb, inw_cb, inl_cb, outb_cb, outw_cb, outl_cb, priv);
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
        mach64_t *candidate;
        if (!gtb_io_hooks[i].used)
            continue;
        candidate = (mach64_t *) gtb_io_hooks[i].priv;
        if (mach64_gtb_is_card(candidate) && candidate->svga.clock_gen == priv) {
            mach64 = candidate;
            break;
        }
    }

    if (mach64) {
        if (mach64_gtb_calc_vclk(mach64, mach64->clock_cntl & 3, &corrected))
            clock = corrected;
        else if (!(clock > 1000000.0 && clock < 250000000.0))
            return;
    }
    ics2595_setclock(priv, clock);
}

/* Optional MMIO accessors for the same GTB-only control latches. */
int
mach64_gtb_cfg_readb(mach64_t *mach64, uint32_t addr, uint8_t *val)
{
    uint16_t offset;
    mach64_gtb_state_t *state;

    if (!mach64_gtb_is_card(mach64) || !(addr & 0x400))
        return 0;

    offset = addr & 0xff;

    if (offset == 0x92) {
        if (val)
            *val = mach64->pll_regs[mach64->pll_addr & 0x0f];
        return 1;
    }

    if (offset >= 0xb4 && offset <= 0xbb)
        return mach64_gtb_bank_readb(mach64, offset, val);

    if (mach64_gtb_gp_io_offset(offset)) {
        if (val)
            *val = mach64_gtb_gp_io_readb(mach64, offset);
        return 1;
    }

    if (!mach64_gtb_shadow_offset(offset))
        return 0;

    state = mach64_gtb_get_state(mach64, 1);
    if (val)
        *val = state ? state->control[offset] : 0;
    return 1;
}

int
mach64_gtb_cfg_writeb(mach64_t *mach64, uint32_t addr, uint8_t val)
{
    uint16_t offset;
    mach64_gtb_state_t *state;

    if (!mach64_gtb_is_card(mach64) || !(addr & 0x400))
        return 0;

    offset = addr & 0xff;

    if (mach64_gtb_gp_io_offset(offset)) {
        mach64_gtb_gp_io_writeb(mach64, offset, val);
        return 1;
    }

    if (!mach64_gtb_shadow_offset(offset))
        return 0;

    state = mach64_gtb_get_state(mach64, 1);
    if (state)
        state->control[offset] = val;
    return 1;
}

/*
 * Fixed VGA setup registers used by ARS2D before it touches standard VGA I/O.
 * GENENA[4] unlocks GENVS writes; GENENA[3] and GENVS[0] together enable VGA.
 */
static uint8_t
mach64_gtb_genvs_in(uint16_t port, void *priv)
{
    mach64_gtb_state_t *state = (mach64_gtb_state_t *) priv;
    (void) port;
    return state ? state->genvs : 0xff;
}

static void
mach64_gtb_genvs_out(uint16_t port, uint8_t val, void *priv)
{
    mach64_gtb_state_t *state = (mach64_gtb_state_t *) priv;
    (void) port;
    if (state && (state->genena & 0x10))
        state->genvs = val & 0x01;
}

static uint8_t
mach64_gtb_genena_in(uint16_t port, void *priv)
{
    mach64_gtb_state_t *state = (mach64_gtb_state_t *) priv;
    (void) port;
    if (!state)
        return 0xff;
    return (state->genena & ~0x08u) | (state->genvs ? 0x08u : 0x00u);
}

static void
mach64_gtb_genena_out(uint16_t port, uint8_t val, void *priv)
{
    mach64_gtb_state_t *state = (mach64_gtb_state_t *) priv;
    (void) port;
    if (!state || (state->pci_ioconfig & 0x08))
        return;
    state->genena = val;
}

void
mach64_gtb_state_attach(void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    mach64_gtb_state_t *state = mach64_gtb_get_state(mach64, 1);

    if (!state || state->ports_attached)
        return;

    /*
     * CNFG_CHIP_ID belongs to the Rage II+ device model, not this I/O hook.
     * PCI Revision ID and the Mach64 chip register are independent hardware
     * fields even when this target uses the same 0x9a revision value for both.
     * Keep identity initialization centralized in mach64rage2p_init().
     */

    io_sethandler(0x0102, 1, mach64_gtb_genvs_in, NULL, NULL,
                  mach64_gtb_genvs_out, NULL, NULL, state);
    io_sethandler(0x46e8, 1, mach64_gtb_genena_in, NULL, NULL,
                  mach64_gtb_genena_out, NULL, NULL, state);
    state->ports_attached = 1;

    /* GP_IO powers up with both candidate I2C pin pairs released. */
    mach64_gtb_gp_io_drive(mach64);

    if (mach64 && mach64->svga.clock_gen)
        ics2595_setclock(mach64->svga.clock_gen, 25175000.0);
}

void
mach64_gtb_state_detach(void *priv)
{
    mach64_t *mach64 = (mach64_t *) priv;
    mach64_gtb_state_t *state = mach64_gtb_get_state(mach64, 0);

    if (!state)
        return;
    if (state->ports_attached) {
        io_removehandler(0x0102, 1, mach64_gtb_genvs_in, NULL, NULL,
                         mach64_gtb_genvs_out, NULL, NULL, state);
        io_removehandler(0x46e8, 1, mach64_gtb_genena_in, NULL, NULL,
                         mach64_gtb_genena_out, NULL, NULL, state);
    }
    memset(state, 0, sizeof(*state));
}

uint8_t
mach64_gtb_pci_ioconfig_read(void *priv)
{
    mach64_gtb_state_t *state = mach64_gtb_get_state((mach64_t *) priv, 1);
    return state ? state->pci_ioconfig : 0;
}

void
mach64_gtb_pci_ioconfig_write(void *priv, uint8_t val)
{
    mach64_t *mach64 = (mach64_t *) priv;
    mach64_gtb_state_t *state = mach64_gtb_get_state(mach64, 1);

    mach64_3d_trace_external(mach64, 'Q', 1, 0x1040u, val, 1);
    if (state)
        state->pci_ioconfig = val & 0x0f;
}

/* Force a remap after BAR1/IOCONFIG writes; the legacy guard misses this. */
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
