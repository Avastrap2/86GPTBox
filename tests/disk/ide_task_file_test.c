/* An IDE hard disc runs a command on the task file the host wrote for it,
   also when that command ends a PIO data-in transfer the host left
   unfinished, while a transfer that is finished keeps its own registers. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the implementation to drive a board and a hard disc through the
   task-file registers without the rest of the machine. */
#include "../../src/disk/hdc_ide.c"

#define SPT    63
#define HPC    16
#define TRACKS 100

hard_disk_t     hdd[HDD_NUM];
int             hdc_current[HDC_MAX];
int             sound_card_current[SOUND_CARD_MAX];
int             machine;
const machine_t machines[1];

int machine_at_lgibmx61_init(const machine_t *model) { (void) model; return 0; }

void
fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

void warning(const char *fmt, ...) { (void) fmt; }
void device_context_inst(const device_t *dev, int inst) { (void) dev; (void) inst; }
void device_context_restore(void) { }
void *device_add_params(const device_t *dev, void *params) { (void) dev; (void) params; return NULL; }
int device_get_config_int(const char *name) { (void) name; return 0; }
const device_t *hdc_get_device(int hdc) { (void) hdc; return NULL; }
const device_t *sound_card_getdevice(int card) { (void) card; return NULL; }

/* Every sector starts with its own number, so a read shows which one it was. */
int
hdd_image_read(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    (void) id;
    for (uint32_t s = 0; s < count; s++) {
        for (int i = 0; i < 512; i++)
            buffer[s * 512 + i] = (uint8_t) (sector + s + i);
        memcpy(&buffer[s * 512], &(uint32_t) { sector + s }, 4);
    }
    return 0;
}

int hdd_image_load(int id) { (void) id; return 1; }
int hdd_image_write(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer) { (void) id; (void) sector; (void) count; (void) buffer; return 0; }
int hdd_image_zero(uint8_t id, uint32_t sector, uint32_t count) { (void) id; (void) sector; (void) count; return 0; }
uint32_t hdd_image_get_last_sector(uint8_t id) { (void) id; return SPT * HPC * TRACKS - 1; }
void hdd_image_close(uint8_t id) { (void) id; }
void hdd_preset_apply(int hdd_id) { (void) hdd_id; }
double hdd_timing_write(hard_disk_t *disk, uint32_t addr, uint32_t len) { (void) disk; (void) addr; (void) len; return 0.0; }
double hdd_timing_read(hard_disk_t *disk, uint32_t addr, uint32_t len) { (void) disk; (void) addr; (void) len; return 0.0; }
double hdd_seek_get_time(hard_disk_t *disk, uint32_t dst_addr, uint8_t operation, uint8_t continuous, double max_seek_time)
{
    (void) disk; (void) dst_addr; (void) operation; (void) continuous; (void) max_seek_time;
    return 0.0;
}

void
io_handler(uint8_t set, uint16_t base, uint16_t size,
           uint8_t (*inb)(uint16_t port, void *priv), uint16_t (*inw)(uint16_t port, void *priv),
           uint32_t (*inl)(uint16_t port, void *priv), void (*outb)(uint16_t port, uint8_t val, void *priv),
           void (*outw)(uint16_t port, uint16_t val, void *priv), void (*outl)(uint16_t port, uint32_t val, void *priv),
           void *priv)
{
    (void) set; (void) base; (void) size; (void) inb; (void) inw; (void) inl;
    (void) outb; (void) outw; (void) outl; (void) priv;
}

void *
isapnp_add_card(uint8_t *rom, uint16_t rom_size,
                void (*config_changed)(uint8_t ld, isapnp_device_config_t *config, void *priv),
                void (*csn_changed)(uint8_t csn, void *priv),
                uint8_t (*read_vendor_reg)(uint8_t ld, uint8_t reg, void *priv),
                void (*write_vendor_reg)(uint8_t ld, uint8_t reg, uint8_t val, void *priv), void *priv)
{
    (void) rom; (void) rom_size; (void) config_changed; (void) csn_changed;
    (void) read_vendor_reg; (void) write_vendor_reg; (void) priv;
    return NULL;
}

uint8_t
mca_add(uint8_t (*read)(uint16_t port, void *priv), void (*write)(uint16_t port, uint8_t val, void *priv),
        uint8_t (*feedb)(void *priv), void (*reset)(void *priv), void *priv)
{
    (void) read; (void) write; (void) feedb; (void) reset; (void) priv;
    return 0;
}

void mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size) { (void) map; (void) base; (void) size; }
void mem_mapping_disable(mem_mapping_t *map) { (void) map; }
void picint_common(uint16_t num, int level, int set, uint8_t *irq_state) { (void) num; (void) level; (void) set; (void) irq_state; }
int rom_present(const char *fn) { (void) fn; return 0; }
int rom_init(rom_t *rom, const char *fn, uint32_t address, int size, int mask, int file_offset, uint32_t flags)
{
    (void) rom; (void) fn; (void) address; (void) size; (void) mask; (void) file_offset; (void) flags;
    return 0;
}

/* No worker thread: image reads happen when the command completes. */
thread_t *thread_create_named(void (*thread_func)(void *param), void *param, const char *name) { (void) thread_func; (void) param; (void) name; return NULL; }
int thread_wait(thread_t *arg) { (void) arg; return 0; }
event_t *thread_create_event(void) { static int event; return &event; }
void thread_set_event(event_t *arg) { (void) arg; }
void thread_reset_event(event_t *arg) { (void) arg; }
int thread_wait_event(event_t *arg, int timeout) { (void) arg; (void) timeout; return 0; }
void thread_destroy_event(event_t *arg) { (void) arg; }

void
timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer)
{
    timer->callback = callback;
    timer->priv     = priv;
    timer->flags    = start_timer;
}

void timer_stop(pc_timer_t *timer) { timer->flags = 0; }
void timer_on_auto(pc_timer_t *timer, double period) { timer->flags = 1; timer->period = period; }
void ui_sb_update_icon(int tag, int active) { (void) tag; (void) active; }
void ui_sb_update_icon_write(int tag, int write) { (void) tag; (void) write; }

static int failures;

#define CHECK(name, got, want) do { \
    if ((got) != (want)) { \
        fprintf(stderr, "%s: got %ld, expected %ld\n", (name), (long) (got), (long) (want)); \
        failures++; \
    } \
} while (0)

static ide_board_t *board;

static void reg_out(int reg, uint8_t val) { ide_writeb(0x1f0 + reg, val, board); }
static uint8_t status(void) { return ide_readb(0x1f7, board); }

/* Let the disc finish whatever it scheduled. */
static void
settle(void)
{
    for (int i = 0; i < 64; i++) {
        pc_timer_t *timer = &ide_drives[0]->timer;

        if (!timer->flags)
            return;
        timer->flags = 0;
        timer->callback(timer->priv);
    }
}

/* Read one sector's worth of PIO data and return the number it starts with. */
static uint32_t
read_sector(int words)
{
    uint16_t data[256];

    for (int i = 0; i < words; i++)
        data[i] = ide_readw(0x1f0, board);
    return data[0] | ((uint32_t) data[1] << 16);
}

static void
read_sectors_lba(uint32_t lba, uint8_t count)
{
    reg_out(6, 0xe0 | ((lba >> 24) & 0x0f));
    reg_out(2, count);
    reg_out(3, lba & 0xff);
    reg_out(4, (lba >> 8) & 0xff);
    reg_out(5, (lba >> 16) & 0xff);
    reg_out(7, WIN_READ);
    settle();
}

int
main(void)
{
    hdd[0].bus_type    = HDD_BUS_IDE;
    hdd[0].ide_channel = 0;
    hdd[0].spt         = SPT;
    hdd[0].hpc         = HPC;
    hdd[0].tracks      = TRACKS;

    ide_board_init(0, 14, 0x1f0, 0x3f6, 0, DEVICE_ISA);
    board = ide_boards[0];
    CHECK("disc attached", ide_drives[0]->type, IDE_HDD);

    /* A read of LBA 16 leaves its address in the task file. */
    read_sectors_lba(16, 1);
    CHECK("first read", read_sector(256), 16);

    /* HWiNFO32 reads only the first 128 words of IDENTIFY DEVICE ... */
    reg_out(7, WIN_IDENTIFY);
    settle();
    CHECK("IDENTIFY data ready", status() & DRQ_STAT, DRQ_STAT);
    (void) read_sector(128);
    CHECK("IDENTIFY left unfinished", status() & DRQ_STAT, DRQ_STAT);

    /* ... and Windows 95 then sets up and issues its next read, LBA 40. */
    read_sectors_lba(40, 1);
    CHECK("read after an unfinished IDENTIFY", read_sector(256), 40);
    CHECK("read after an unfinished IDENTIFY ends", status() & (BSY_STAT | DRQ_STAT), 0);

    /* A write into the task file during a transfer the host finishes does not
       move that transfer, nor a later command that sets up its own registers. */
    read_sectors_lba(50, 2);
    CHECK("two-sector read, first", read_sector(256), 50);
    settle();
    reg_out(3, 99);
    CHECK("two-sector read, second", read_sector(256), 51);
    read_sectors_lba(60, 1);
    CHECK("read after a finished transfer", read_sector(256), 60);

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("ide task-file tests passed\n");
    return failures ? 1 : 0;
}
