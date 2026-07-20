#include "i2000.h"
#include "merced.h"
#include "input_menu.h"
#include "vga_ibm.h"
#include "vgafont16.h"
#include "vgabios_rom.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/*
 * HP i2000 system model. See i2000.h for the memory map.
 *
 * The legacy I/O port window: on IA-64 the ISA/PCI port space is a
 * memory-mapped window whose base the firmware learns from the chipset.
 * 0x00000FFFFC000000 matches what HP's SAL reports on 460GX systems; every
 * unknown physical access is logged to the front panel, so if the firmware
 * uses a different window it will show up there and we can adjust.
 */
#define I2000_IO_BASE   0x00000FFFFC000000ull
#define I2000_IO_SIZE   0x0000000004000000ull   /* 64 MiB window */
#define COM1_PORT       0x3F8
#define POST_PORT       0x80
#define PCI_CFG_ADDR     0xCF8
#define PCI_CFG_DATA     0xCFC
#define RESET_CTRL_PORT  0xCF9
#define I2000_FW_SHADOW_BASE 0x03C00000ull
#define CMD649_PCI_DEV   3
#define CMD649_PCI_FUN   1

/* Early 460GX SAC scratch/control registers used by the SAL bootstrap. */
#define I2000_SAC_CBNR  0x0000FEB00CB0ull
#define I2000_SAC_CCSR  0x0000FEB00CC0ull

/* front panel framebuffer */
#define FB_W 640
#define FB_H 400
#define CELL_W 6
#define CELL_H 8
#define COLS (FB_W / CELL_W)     /* 106 */
#define ROWS (FB_H / CELL_H)     /* 50  */
#define CON_ROWS 30              /* serial console area at the bottom */

#define INSTR_PER_FRAME 500000
#define MMIO_LOG_N 24
#define HALT_TRACE_LINES 32
#define HALT_CALL_LINES  32

typedef struct {
    uint64_t addr;
    uint64_t val;
    uint32_t count;
    uint8_t  is_write;
    uint8_t  size;
} MmioLogEnt;

struct Ia64I2000State {
    GemuMonitor *monitor;
    GemuDisplay *display;
    Merced      *cpu;

    uint8_t  *ram;
    uint64_t  ram_size;
    uint8_t  *flash;
    char      flash_file[512];
    uint32_t  flash_image_size;
    bool      flash_loaded;
    bool      flash_read_status;
    bool      flash_read_id;
    uint8_t   flash_status;
    uint8_t   flash_cmd;
    uint64_t  flash_cmd_addr;

    bool      halted;
    bool      reset_requested;
    bool      fw_shadow_enabled;
    MercedStatus halt_status;

    /* front panel */
    uint32_t  fb[FB_W * FB_H];
    char      console[CON_ROWS][COLS + 1];
    int       con_row, con_col;
    bool      con_dirty;

    /* devices */
    uint8_t   post_code;
    bool      legacy_irq_routed;
    uint32_t  sac_cbnr, sac_ccsr;
    uint8_t   port61;
    uint8_t   pit2_polls;
    uint8_t   pic_master_mask, pic_slave_mask;
    uint8_t   pic_master_base, pic_slave_base;
    uint8_t   pic_master_icw, pic_slave_icw;
    uint16_t  pit0_reload, pit0_latch;
    uint8_t   pit0_write_phase;
    uint64_t  pit0_next_irq;
    uint8_t   kbc_command_byte;
    uint8_t   kbc_pending_write; /* 0=keyboard data, 1=command byte, 2=output port, 3=aux */
    uint8_t   kbc_out[8];
    uint8_t   kbc_out_pos, kbc_out_len;
    uint8_t   cmos_index;
    uint8_t   cmos[128];
    uint32_t  pci_cfg_addr;
    bool      ifb_smbus_cmd_read_once;
    uint8_t   ifb_cfg[256];
    uint8_t   ifb_usb_cfg[256];
    uint8_t   ifb_smbus_cfg[256];
    uint8_t   cmd649_cfg[256];
    uint8_t   acpi_io[64];
    FILE     *cdrom;
    char      cdrom_file[512];
    uint64_t  cdrom_size;
    uint8_t   atapi_error, atapi_features, atapi_count;
    uint8_t   atapi_lba_low, atapi_lba_mid, atapi_lba_high, atapi_device;
    uint8_t   atapi_status, atapi_packet[12];
    unsigned  atapi_packet_pos;
    uint8_t  *atapi_data;
    size_t    atapi_data_len, atapi_data_pos;
    uint8_t   chipset_bus;
    /* Real 460GX device map (SSDM 248704-001 Table 2-1, bus CBN):
     *   00h/01h SAC, 04h SDC, 05h/06h Memory Card A/B, 10h-17h Expander
     *   0-3 buses a/b (WXB/PXB/GXB - not yet modeled). Devices are
     *   multi-function; fn dimension added since SAC dev 1 fn 1 is
     *   accessed during boot (extended/diagnostic function, purpose
     *   still being traced). */
    uint8_t   chipset_cfg[32][8][256];
    uint8_t   memcard_cfg[2][8][256];
    uint8_t   uart_ier, uart_lcr, uart_mcr, uart_scr, uart_dll, uart_dlm;
    uint8_t   uart_rx[256], uart_rx_head, uart_rx_tail;

    VgaIbm    vga;
    /* The video BIOS option ROM shadow, kept separate from plain system RAM
     * (like `flash`) so a generic "clear all of RAM" firmware loop - which
     * legitimately treats the whole reported memory range as ordinary RAM -
     * can't wipe it out before IA-32 mode ever gets to use it. Real chipsets
     * exclude this range from the reported memory map for the same reason. */
    uint8_t   vga_rom_shadow[0x10000];

    MmioLogEnt mmio_log[MMIO_LOG_N];
    int        mmio_log_n;

    /* Wall-clock (not instruction-count) periodic autosave, so a long,
     * mostly-idle-waiting debug session always has a recent rollback point
     * without needing a human to remember to snapshot. Two rotating slots
     * rather than one: if the process is killed mid-write of the current
     * slot, the other one is still intact. */
    time_t     last_autosave;
    unsigned   autosave_slot;
};

static void mmio_log(Ia64I2000State *s, uint64_t addr, uint64_t val,
                     unsigned size, bool is_write);
static void i2000_reset(Ia64I2000State *s);

static uint64_t size_mask(unsigned size) {
    return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1;
}

static void atapi_set_data(Ia64I2000State *s, const void *data, size_t len) {
    free(s->atapi_data);
    s->atapi_data = NULL;
    s->atapi_data_len = s->atapi_data_pos = 0;
    if (len) {
        s->atapi_data = malloc(len);
        if (!s->atapi_data) {
            s->atapi_error = 0x04;
            s->atapi_status = 0x41;
            return;
        }
        memcpy(s->atapi_data, data, len);
        s->atapi_data_len = len;
    }
    s->atapi_count = 0x02; /* command/data phase, I/O to host */
    s->atapi_lba_mid = (uint8_t)len;
    s->atapi_lba_high = (uint8_t)(len >> 8);
    s->atapi_status = len ? 0x48 : 0x40; /* DRDY|DRQ / DRDY */
}

static void atapi_reply(Ia64I2000State *s) {
    const uint8_t *p = s->atapi_packet;
    uint8_t reply[64] = {0};
    uint32_t blocks = (uint32_t)(s->cdrom_size / 2048);
    switch (p[0]) {
    case 0x00: /* TEST UNIT READY */
    case 0x1B: /* START STOP UNIT */
    case 0x1E: /* PREVENT/ALLOW MEDIUM REMOVAL */
        atapi_set_data(s, NULL, 0);
        break;
    case 0x03: /* REQUEST SENSE */
        reply[0] = 0x70; reply[7] = 10;
        atapi_set_data(s, reply, p[4] < 18 ? p[4] : 18);
        break;
    case 0x12: { /* INQUIRY */
        reply[0] = 0x05; reply[1] = 0x80; reply[2] = 0x00;
        reply[3] = 0x21; reply[4] = 31;
        memcpy(reply + 8, "GEMU    ", 8);
        memcpy(reply + 16, "ATAPI CD-ROM    ", 16);
        memcpy(reply + 32, "1.0 ", 4);
        size_t n = p[4] < 36 ? p[4] : 36;
        atapi_set_data(s, reply, n);
        break;
    }
    case 0x25: /* READ CAPACITY */
        if (blocks) blocks--;
        reply[0] = blocks >> 24; reply[1] = blocks >> 16;
        reply[2] = blocks >> 8;  reply[3] = blocks;
        reply[6] = 0x08;
        atapi_set_data(s, reply, 8);
        break;
    case 0x43: { /* READ TOC: one data track, lead-out */
        reply[1] = 0x12; reply[2] = 1; reply[3] = 1;
        reply[5] = 0x14; reply[6] = 1;
        reply[13] = 0x14; reply[14] = 0xAA;
        reply[16] = blocks >> 24; reply[17] = blocks >> 16;
        reply[18] = blocks >> 8; reply[19] = blocks;
        size_t alloc = ((size_t)p[7] << 8) | p[8];
        atapi_set_data(s, reply, alloc < 20 ? alloc : 20);
        break;
    }
    case 0x28: /* READ(10) */
    case 0xA8: { /* READ(12) */
        uint32_t lba = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) |
                       ((uint32_t)p[4] << 8) | p[5];
        uint32_t count = p[0] == 0x28 ? ((uint32_t)p[7] << 8) | p[8] :
                         ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                         ((uint32_t)p[8] << 8) | p[9];
        size_t len = (size_t)count * 2048;
        uint8_t *buf = len ? malloc(len) : NULL;
        if ((len && !buf) || lba >= blocks || count > blocks - lba ||
            fseek(s->cdrom, (long)((uint64_t)lba * 2048), SEEK_SET) != 0 ||
            (len && fread(buf, 1, len, s->cdrom) != len)) {
            free(buf); s->atapi_error = 0x50; s->atapi_status = 0x41;
        } else {
            atapi_set_data(s, buf, len);
            free(buf);
        }
        break;
    }
    default:
        fprintf(stderr, "i2000: ATAPI unsupported packet command %02X\n", p[0]);
        s->atapi_error = 0x50;
        s->atapi_status = 0x41;
        break;
    }
}

/* Chipset-internal devices per Table 2-1: SAC (00h/01h), SDC (04h), and
 * Expander 0-3 buses a/b (10h-17h, where WXB/PXB/GXB actually live).
 * Memory Card A/B (05h/06h) are deliberately excluded - they're handled
 * by the separate memcard_cfg array below, and must be checked first by
 * callers or the (also dev-present) memcard reads/writes never fire.
 * Function 0 is always present for SAC/SDC/Expanders; other functions are
 * probed live (SAC dev 1 responds on fn 1 too) and default to an all-zero
 * backing store until we learn what firmware expects there. Making the
 * Expanders present (routed to a zeroed chipset_cfg entry instead of the
 * unhandled-access all-1s default) matters: firmware's memory-size setup
 * probes reg 0x98 on each of them and folds the result into a top-of-memory
 * computation, and an all-1s "no device" response there was read as a huge
 * bogus size, turning a bounded memclr loop into a multi-billion-iteration
 * one. */
static bool chipset_device_present(unsigned dev) {
    return dev == 0 || dev == 1 || dev == 4 || (dev >= 0x10 && dev <= 0x17);
}

static uint64_t pci_cfg_read(Ia64I2000State *s, unsigned lane, unsigned size) {
    uint32_t a = s->pci_cfg_addr;
    if (!(a & 0x80000000u) || lane + size > 4)
        return size_mask(size);
    unsigned bus = (a >> 16) & 0xFF;
    unsigned dev = (a >> 11) & 0x1F;
    unsigned fun = (a >> 8) & 7;
    unsigned reg = (a & 0xFC) + lane;
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == 0 &&
        reg + size <= sizeof(s->ifb_cfg)) {
        uint64_t v = 0;
        memcpy(&v, &s->ifb_cfg[reg], size);
        return v;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && (fun == 2 || fun == 3)) {
        uint8_t *cfg = fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
        uint64_t v = 0;
        memcpy(&v, &cfg[reg], size);
        if (fun == 3 && reg <= 4 && reg + size > 4) {
            /* Command register bit 0 (I/O space enable) doubles as a
             * software-driven busy pulse for the SMBus host controller's
             * retry loop: it writes 1, then polls this same bit forever
             * waiting for hardware to clear it, with no further writes in
             * the steady state - so a fixed "clear the Nth read" model
             * can't work; nothing ever arms a later read. The very first
             * ever read (confirming the enable write stuck) needs to see
             * the true set value; every read after that reports bit 0
             * clear unconditionally, so the poll succeeds immediately. */
            if (s->ifb_smbus_cmd_read_once)
                v &= ~(UINT64_C(1) << ((4 - reg) * 8));
            s->ifb_smbus_cmd_read_once = true;
        }
        return v;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        uint64_t v = 0;
        memcpy(&v, &s->cmd649_cfg[reg], size);
        return v;
    }

    /* On bus zero, device 10h is the SAC's special CBN programming
     * endpoint.  Firmware uses byte register 40h to select the bus on
     * which the rest of the 460GX components appear. */
    if (bus == 0 && dev == 0x10 && fun == 0 && reg == 0x40 && size == 1)
        return s->chipset_bus;
    if (bus == s->chipset_bus && chipset_device_present(dev) &&
        reg + size <= 256) {
        uint64_t v = 0;
        memcpy(&v, &s->chipset_cfg[dev][fun][reg], size);
        return v;
    }
    if (bus == s->chipset_bus && (dev == 5 || dev == 6) &&
        reg + size <= 256) {
        uint64_t v = 0;
        memcpy(&v, &s->memcard_cfg[dev - 5][fun][reg], size);
        return v;
    }

    uint64_t key = 0xC000000000000000ull |
                   ((uint64_t)bus << 24) | ((uint64_t)dev << 19) |
                   ((uint64_t)fun << 16) | reg;
    mmio_log(s, key, 0, size, false);
    return size_mask(size);
}

static void pci_cfg_write(Ia64I2000State *s, unsigned lane,
                          uint64_t val, unsigned size) {
    uint32_t a = s->pci_cfg_addr;
    if (!(a & 0x80000000u) || lane + size > 4)
        return;
    unsigned bus = (a >> 16) & 0xFF;
    unsigned dev = (a >> 11) & 0x1F;
    unsigned fun = (a >> 8) & 7;
    unsigned reg = (a & 0xFC) + lane;
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == 0 &&
        reg + size <= sizeof(s->ifb_cfg)) {
        /* Identity, class code and header type are read-only.  The rest of
         * the IFB LPC/FWH configuration space is mostly retained control
         * latches; firmware programs ACPI/GPIO bases and decode enables
         * here before entering EFI. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                s->ifb_cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && (fun == 2 || fun == 3)) {
        uint8_t *cfg = fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        /* Identity, revision/class and header type are read-only.  Command,
         * BAR and CMD timing registers are ordinary retained latches for the
         * empty-controller model. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                s->cmd649_cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == 0x10 && fun == 0 && reg == 0x40 && size == 1) {
        s->chipset_bus = (uint8_t)val;
        return;
    }
    if (bus == s->chipset_bus && chipset_device_present(dev) &&
        reg + size <= 256) {
        /* Device/vendor ID and class/revision are read-only on function 0
         * (real identity); other functions have no such fixed header yet,
         * so leave them fully writable until we know their layout. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (fun != 0 || (off >= 4 && !(off >= 8 && off < 12)))
                s->chipset_cfg[dev][fun][off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == s->chipset_bus && (dev == 5 || dev == 6) &&
        reg + size <= 256) {
        memcpy(&s->memcard_cfg[dev - 5][fun][reg], &val, size);
        return;
    }
    uint64_t key = 0xC000000000000000ull |
                   ((uint64_t)bus << 24) | ((uint64_t)dev << 19) |
                   ((uint64_t)fun << 16) | reg;
    mmio_log(s, key, val & size_mask(size), size, true);
}

/* ── Serial console ──────────────────────────────────────────────────────── */

static void con_newline(Ia64I2000State *s) {
    s->con_col = 0;
    if (++s->con_row >= CON_ROWS) {
        memmove(s->console[0], s->console[1], sizeof(s->console) - sizeof(s->console[0]));
        memset(s->console[CON_ROWS - 1], 0, sizeof(s->console[0]));
        s->con_row = CON_ROWS - 1;
    }
}

static void con_putc(Ia64I2000State *s, char c) {
    s->con_dirty = true;
    if (c == '\r') { s->con_col = 0; return; }
    if (c == '\n') { con_newline(s); return; }
    if (c < 32 || c > 126) c = '.';
    if (s->con_col >= COLS) con_newline(s);
    s->console[s->con_row][s->con_col++] = c;
    s->console[s->con_row][s->con_col] = 0;
}

/* ── Physical bus ────────────────────────────────────────────────────────── */

static void mmio_log(Ia64I2000State *s, uint64_t addr, uint64_t val,
                     unsigned size, bool is_write) {
    for (int i = 0; i < s->mmio_log_n; i++) {
        if (s->mmio_log[i].addr == addr && s->mmio_log[i].is_write == is_write) {
            s->mmio_log[i].count++;
            s->mmio_log[i].val = val;
            return;
        }
    }
    if (s->mmio_log_n < MMIO_LOG_N) {
        MmioLogEnt *e = &s->mmio_log[s->mmio_log_n++];
        e->addr = addr; e->val = val; e->count = 1;
        e->is_write = is_write; e->size = (uint8_t)size;
        fprintf(stderr, "i2000: unhandled %s pa=0x%012" PRIX64 " size=%u"
                        "%s0x%" PRIX64 "\n",
                is_write ? "write" : "read ", addr, size,
                is_write ? " val=" : " -> ", is_write ? val : ~0ull);
    } else {
        /* keep counting in the last slot so the panel shows activity */
        s->mmio_log[MMIO_LOG_N - 1].count++;
    }
}

static bool vga_port(uint64_t port) {
    return port == 0x3B4 || port == 0x3B5 || port == 0x3BA ||
           (port >= 0x3C0 && port <= 0x3CF) ||
           port == 0x3D4 || port == 0x3D5 || port == 0x3DA;
}

static uint8_t cmos_bcd(unsigned v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* The static cmos[] array (fixed at reset, never touched again) satisfies
 * anything that just wants a valid-looking byte back, but firmware
 * commonly calibrates or synchronizes against the RTC by reading a time
 * register, then re-reading it later and waiting for the VALUE to change -
 * a permanently frozen clock makes that an infinite wait no matter what
 * byte comes back. Refresh the standard time/date registers (0-9) from
 * real host time on every access so any such wait resolves within a real
 * second or so. Status register A's UIP (bit 7, update-in-progress) is
 * left at 0 always rather than pulsed, which is a fine approximation:
 * real hardware only asserts it for ~244us out of every second, so a
 * poll for "not updating" would see it clear almost immediately anyway. */
static void cmos_sync_live_clock(Ia64I2000State *s) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    s->cmos[0x00] = cmos_bcd((unsigned)tmv.tm_sec);
    s->cmos[0x02] = cmos_bcd((unsigned)tmv.tm_min);
    s->cmos[0x04] = cmos_bcd((unsigned)tmv.tm_hour);
    s->cmos[0x06] = (uint8_t)(tmv.tm_wday + 1);
    s->cmos[0x07] = cmos_bcd((unsigned)tmv.tm_mday);
    s->cmos[0x08] = cmos_bcd((unsigned)(tmv.tm_mon + 1));
    s->cmos[0x09] = cmos_bcd((unsigned)(tmv.tm_year % 100));
}

static uint64_t io_port_read(Ia64I2000State *s, uint64_t port, unsigned size) {
    static unsigned debug_reads;
    if (size == 1 && vga_port(port))
        return vga_ibm_io_read(&s->vga, (uint16_t)port);
    if (port >= 0x1000 && port + size <= 0x1008) {
        /* Firmware busy-polls a status byte here waiting for bit 0 to
         * clear (the CMD649 SMBus function's reg 20h BAR turned out to be
         * an unrelated 64-bit *memory* BAR at the same numeric value -
         * 0x1004 has its I/O-space bit clear - so this isn't actually
         * that device's I/O space; what real hardware is mapped at this
         * port isn't established). No real device backs it, so read as
         * idle/complete unconditionally rather than falling through to
         * the generic all-ones "unmapped" response, which left bit 0
         * permanently set and the poll spinning forever. */
        return 0;
    }
    if (port == 0x60 && size == 1) {               /* 8042 data */
        if (s->kbc_out_pos >= s->kbc_out_len)
            return 0;
        uint8_t v = s->kbc_out[s->kbc_out_pos++];
        if (s->kbc_out_pos == s->kbc_out_len)
            s->kbc_out_pos = s->kbc_out_len = 0;
        return v;
    }
    if (port == 0x64 && size == 1) {               /* 8042 status */
        /* Commands are consumed synchronously, so IBF (bit 1) is clear.
         * OBF reflects queued controller/keyboard response bytes. */
        return (s->kbc_out_len ? 0x01 : 0) | 0x04; /* system flag */
    }
    if ((port == 0x71 || port == 0x73) && size == 1)
    {
        /* 72h/73h is the i2000 IFB's own RTC/CFGRAM alias; 70h/71h is the
         * universal, standard AT-compatible CMOS index/data pair that
         * every x86-compatible BIOS (including the real-mode setup code
         * this firmware runs before entering native SAL/EFI) expects to
         * work regardless of platform, so both need to reach the same
         * underlying state. */
        if ((s->cmos_index & 0x7F) < 0x0A)
            cmos_sync_live_clock(s);
        uint8_t v = s->cmos[s->cmos_index & 0x7F];
        if ((s->cmos_index & 0x7F) == 0x0A)
            /* Status Register A bit 7 is UIP (update in progress). We don't
             * emulate the real ~244us-per-second update window, and nothing
             * else ever clears whatever got stored here (firmware itself
             * can write this register, and did - a stale 1 bit is exactly
             * what left an earlier boot spinning forever waiting for "not
             * busy"). Force it clear on every read rather than trust
             * whatever's retained; the other status-A bits (divider/rate
             * select) are harmless to read back as stored. */
            v &= ~0x80u;
        if (getenv("MERCED_DEBUG") && debug_reads++ < 128)
            fprintf(stderr, "i2000: CMOS[%02X] -> %02X\n",
                    s->cmos_index & 0x7F, v);
        return v;
    }
    if (port == PCI_CFG_ADDR && size == 4) return s->pci_cfg_addr;
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4)
        return pci_cfg_read(s, (unsigned)(port - PCI_CFG_DATA), size);
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        if (getenv("MERCED_DEBUG") && debug_reads++ < 128)
            fprintf(stderr, "i2000: COM1 read reg %u\n",
                    (unsigned)(port - COM1_PORT));
        switch (port - COM1_PORT) {
        case 0:
            if (s->uart_lcr & 0x80) return s->uart_dll;
            if (s->uart_rx_head != s->uart_rx_tail) {
                uint8_t v = s->uart_rx[s->uart_rx_head++];
                return v;
            }
            return 0;
        case 1: return (s->uart_lcr & 0x80) ? s->uart_dlm : s->uart_ier;
        case 2: return ((s->uart_ier & 1) && s->uart_rx_head != s->uart_rx_tail)
                     ? 0x04 : 0x01;                /* RX data / no interrupt */
        case 3: return s->uart_lcr;
        case 4: return s->uart_mcr;
        case 5: return 0x60 | (s->uart_rx_head != s->uart_rx_tail ? 1 : 0);
        case 6: return 0xB0;                        /* MSR: CTS|DSR|DCD */
        case 7: return s->uart_scr;
        }
    }
    if (port == 0x21) return s->pic_master_mask;
    if (port == 0xA1) return s->pic_slave_mask;
    if (port == 0x404)
        /* System status/GPIO word. The bootstrap reads this once: bits 24
         * and 19 both set means the BIOS recovery jumper is installed and
         * sends the whole boot into PspRecover. 0 = Normal position. */
        return 0;
    if (port >= 0x400 && port + size <= 0x440) {
        uint64_t v = 0;
        memcpy(&v, &s->acpi_io[port - 0x400], size);
        return v;
    }
    if (port == 0x61) {                            /* PIT channel-2 gate/output */
        if (s->pit2_polls < 2) s->pit2_polls++;
        return s->port61 | (s->pit2_polls >= 2 ? 0x20 : 0);
    }
    if (s->cdrom && ((port >= 0x170 && port <= 0x177) || port == 0x376)) {
        unsigned reg = port == 0x376 ? 7 : (unsigned)(port - 0x170);
        if (reg == 0) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++) {
                if (s->atapi_data_pos < s->atapi_data_len)
                    v |= (uint64_t)s->atapi_data[s->atapi_data_pos++] << (i * 8);
            }
            if (s->atapi_data_pos >= s->atapi_data_len && s->atapi_status & 0x08) {
                s->atapi_status = 0x40;
                s->atapi_count = 0x03; /* command complete */
            }
            return v;
        }
        switch (reg) {
        case 1: return s->atapi_error;
        case 2: return s->atapi_count;
        case 3: return s->atapi_lba_low;
        case 4: return s->atapi_lba_mid;
        case 5: return s->atapi_lba_high;
        case 6: return s->atapi_device;
        case 7: return s->atapi_status;
        }
    }
    /* CMD-649 primary/secondary channels, with no ATA devices attached.
     * A floating ATA bus reports status zero; returning the generic unmapped
     * value 0xFF makes firmware believe BSY is asserted forever. */
    if (size == 1 && (port == 0x1F7 || port == 0x177 ||
                      port == 0x3F6 || port == 0x376))
        return 0x00;
    if (size == 1 && ((port >= 0x1F0 && port <= 0x1F6) ||
                      (port >= 0x170 && port <= 0x176)))
        return 0x00;
    mmio_log(s, I2000_IO_BASE + port, 0, size, false);
    return ~0ull;
}

static void io_port_write(Ia64I2000State *s, uint64_t port, uint64_t val,
                          unsigned size) {
    static unsigned debug_writes;
    if (size == 1 && vga_port(port)) {
        vga_ibm_io_write(&s->vga, (uint16_t)port, (uint8_t)val);
        return;
    }
    if (port >= 0x1000 && port + size <= 0x1008)
        return; /* no real device behind it - see io_port_read */
    if (port >= 0x400 && port + size <= 0x440) {
        memcpy(&s->acpi_io[port - 0x400], &val, size);
        return;
    }
    if (port == 0x21 && size == 1) {
        if (s->pic_master_icw) {
            if (s->pic_master_icw == 1)
                s->pic_master_base = (uint8_t)val & 0xF8;
            if (++s->pic_master_icw > 3)
                s->pic_master_icw = 0;
            return;
        }
        s->pic_master_mask = (uint8_t)val;
        if (!(s->pic_master_mask & 1) && s->pit0_reload &&
            !s->pit0_next_irq)
            s->pit0_next_irq = s->cpu->ninsts + 100000;
        return;
    }
    if (port == 0xA1 && size == 1) {
        if (s->pic_slave_icw) {
            if (s->pic_slave_icw == 1)
                s->pic_slave_base = (uint8_t)val & 0xF8;
            if (++s->pic_slave_icw > 3)
                s->pic_slave_icw = 0;
            return;
        }
        s->pic_slave_mask = (uint8_t)val;
        return;
    }
    if (port == 0x20 || port == 0xA0) {
        if ((val & 0x10) != 0) {
            if (port == 0x20)
                s->pic_master_icw = 1;
            else
                s->pic_slave_icw = 1;
        }
        /* Other commands are OCWs (including EOI). */
        return;
    }
    if (port == 0x40 && size == 1) {
        if (!s->pit0_write_phase) {
            s->pit0_latch = (uint8_t)val;
            s->pit0_write_phase = 1;
        } else {
            s->pit0_reload = s->pit0_latch | ((uint16_t)(uint8_t)val << 8);
            s->pit0_write_phase = 0;
            s->pit0_next_irq = s->cpu->ninsts + 100000;
        }
        return;
    }
    if (port == 0x64 && size == 1) {               /* 8042 command */
        uint8_t cmd = (uint8_t)val;
        s->kbc_out_pos = s->kbc_out_len = 0;
        switch (cmd) {
        case 0x20: /* read command byte */
            s->kbc_out[0] = s->kbc_command_byte;
            s->kbc_out_len = 1;
            break;
        case 0x60: s->kbc_pending_write = 1; break;
        case 0xAA: /* controller self-test */
            s->kbc_out[0] = 0x55; s->kbc_out_len = 1;
            break;
        case 0xAB: /* keyboard interface test */
            s->kbc_out[0] = 0x00; s->kbc_out_len = 1;
            break;
        case 0xA9: /* auxiliary interface test: no PS/2 mouse attached */
            s->kbc_out[0] = 0x01; s->kbc_out_len = 1;
            break;
        case 0xAD: s->kbc_command_byte |= 0x10; break;
        case 0xAE: s->kbc_command_byte &= (uint8_t)~0x10; break;
        case 0xA7: s->kbc_command_byte |= 0x20; break;
        case 0xA8: s->kbc_command_byte &= (uint8_t)~0x20; break;
        case 0xD0: /* read output port: reset deasserted, A20 enabled */
            s->kbc_out[0] = 0x03; s->kbc_out_len = 1;
            break;
        case 0xD1: s->kbc_pending_write = 2; break;
        case 0xD4: s->kbc_pending_write = 3; break;
        default: break;
        }
        return;
    }
    if (port == 0x60 && size == 1) {               /* 8042/keyboard data */
        uint8_t data = (uint8_t)val;
        s->kbc_out_pos = s->kbc_out_len = 0;
        if (s->kbc_pending_write == 1) {
            s->kbc_command_byte = data;
        } else if (s->kbc_pending_write == 3) {
            /* No auxiliary PS/2 device is attached.  Consume the routed
             * byte but leave OBF clear so the firmware's mouse probe times
             * out and does not instantiate a phantom mouse driver. */
        } else if (s->kbc_pending_write == 0) {
            /* An empty PS/2 keyboard still acknowledges commands.  Reset
             * additionally reports a successful BAT result. */
            s->kbc_out[0] = 0xFA;
            s->kbc_out_len = 1;
            if (data == 0xFF) {
                s->kbc_out[1] = 0xAA;
                s->kbc_out_len = 2;
            }
        }
        s->kbc_pending_write = 0;
        return;
    }
    if ((port == 0x70 || port == 0x72) && size == 1) {
        /* The i2000 IFB exposes its RTC/configuration RAM at 72h/73h, but
         * 70h/71h (standard AT CMOS index/data) must alias the same state -
         * see the read-side comment. Bit 7 of 70h is conventionally the
         * NMI-mask bit on real hardware; we don't model NMI, so it's
         * harmlessly folded into the index like the rest of the byte. */
        s->cmos_index = (uint8_t)val & 0x7F;
        return;
    }
    if ((port == 0x71 || port == 0x73) && size == 1) {
        if (getenv("MERCED_DEBUG") && debug_writes++ < 128)
            fprintf(stderr, "i2000: CMOS[%02X] <- %02X\n",
                    s->cmos_index & 0x7F, (unsigned)(uint8_t)val);
        s->cmos[s->cmos_index & 0x7F] = (uint8_t)val;
        return;
    }
    if (port == RESET_CTRL_PORT && size == 1) {
        /* Intel reset-control convention: bit 1 selects a hard reset and
         * bit 2 triggers it.  SAL writes 02h followed by 06h, then waits in
         * a dead loop for the platform reset to arrive. */
        if (val & 0x04)
            s->reset_requested = true;
        return;
    }
    if (port == PCI_CFG_ADDR && size == 4) {
        s->pci_cfg_addr = (uint32_t)val;
        return;
    }
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4) {
        pci_cfg_write(s, (unsigned)(port - PCI_CFG_DATA), val, size);
        return;
    }
    if (port == POST_PORT) {
        s->post_code = (uint8_t)val;
        return;
    }
    if (port == 0x42) {                            /* PIT channel-2 count */
        s->pit2_polls = 0;
        return;
    }
    if (port == 0x43) {
        if (((uint8_t)val >> 6) == 0)
            s->pit0_write_phase = 0;
        return;
    }
    if (port == 0x61) {
        s->port61 = (uint8_t)val & 0x0F;
        return;
    }
    if (s->cdrom && ((port >= 0x170 && port <= 0x177) || port == 0x376)) {
        unsigned reg = port == 0x376 ? 8 : (unsigned)(port - 0x170);
        if (reg == 0) {
            for (unsigned i = 0; i < size && s->atapi_packet_pos < 12; i++)
                s->atapi_packet[s->atapi_packet_pos++] = (uint8_t)(val >> (i * 8));
            if (s->atapi_packet_pos == 12)
                atapi_reply(s);
            return;
        }
        switch (reg) {
        case 1: s->atapi_features = (uint8_t)val; return;
        case 2: s->atapi_count = (uint8_t)val; return;
        case 3: s->atapi_lba_low = (uint8_t)val; return;
        case 4: s->atapi_lba_mid = (uint8_t)val; return;
        case 5: s->atapi_lba_high = (uint8_t)val; return;
        case 6: s->atapi_device = (uint8_t)val; return;
        case 7:
            s->atapi_error = 0;
            if ((uint8_t)val == 0xA0) { /* PACKET */
                s->atapi_packet_pos = 0;
                memset(s->atapi_packet, 0, sizeof(s->atapi_packet));
                s->atapi_count = 0x01;
                s->atapi_status = 0x48;
            } else if ((uint8_t)val == 0xA1) { /* IDENTIFY PACKET DEVICE */
                uint8_t id[512] = {0};
                id[0] = 0xC0; id[1] = 0x85;       /* removable ATAPI CD-ROM */
                id[98] = 0x00; id[99] = 0x02;     /* LBA supported */
                char model[40];
                memset(model, ' ', sizeof(model));
                memcpy(model, "GEMU ATAPI CD-ROM", 17);
                for (unsigned i = 0; i < 40; i += 2) {
                    id[54 + i] = model[i + 1]; id[55 + i] = model[i];
                }
                atapi_set_data(s, id, sizeof(id));
            } else if ((uint8_t)val == 0x08) { /* DEVICE RESET */
                s->atapi_status = 0x40;
                s->atapi_count = 1; s->atapi_lba_mid = 0x14; s->atapi_lba_high = 0xEB;
            } else {
                s->atapi_error = 0x04; s->atapi_status = 0x41;
            }
            return;
        case 8: /* device control / software reset */
            if (val & 4) s->atapi_status = 0x80;
            else { s->atapi_status = 0x40; s->atapi_count = 1;
                   s->atapi_lba_mid = 0x14; s->atapi_lba_high = 0xEB; }
            return;
        }
    }
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        if (getenv("MERCED_DEBUG") && debug_writes++ < 128)
            fprintf(stderr, "i2000: COM1 write reg %u <- %02X\n",
                    (unsigned)(port - COM1_PORT), (unsigned)(uint8_t)val);
        switch (port - COM1_PORT) {
        case 0:
            if (s->uart_lcr & 0x80) { s->uart_dll = (uint8_t)val; return; }
            con_putc(s, (char)val);
            fputc((int)val, stdout);
            fflush(stdout);
            return;
        case 1:
            if (s->uart_lcr & 0x80) s->uart_dlm = (uint8_t)val;
            else s->uart_ier = (uint8_t)val;
            return;
        case 3: s->uart_lcr = (uint8_t)val; return;
        case 4: s->uart_mcr = (uint8_t)val; return;
        case 7: s->uart_scr = (uint8_t)val; return;
        default: return;                            /* FCR etc. */
        }
    }
    if (size == 1 && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6 ||
                      (port >= 0x170 && port <= 0x177) || port == 0x376))
        return;                                     /* empty CMD-649 channels */
    mmio_log(s, I2000_IO_BASE + port, val, size, true);
}

/* Merced uses the architected sparse legacy-I/O encoding:
 *   offset = ((port & ~3) << 10) | (port & 0xFFF)
 * a bitfield insert, not an addition - the low two address bits select the
 * byte lane, and bits 2-11 of port are carried in offset's low 12 bits
 * unchanged while offset's bits 12 and up are just (port & ~3) shifted.
 * Treating this as addition (offset = port + ((port & ~3) << 10)) happens
 * to agree with the true insert for every port below 0x1000, since there's
 * no bit overlap to lose to the OR there - which is exactly why ports like
 * 0xCFC (PCI CONFIG_DATA) and 0x3F8 (COM1) always worked. It silently
 * breaks for port >= 0x1000: the insert clips the deposited port value to
 * its low 12 bits, discarding bit 12 and up entirely, while the shifted
 * term alone already carries those bits - addition instead double-counts
 * them and produces a different offset than hardware does. A real device
 * BAR'd above 0x1000 (seen: the CMD649 SMBus function's I/O BAR at 0x1000)
 * would decode to "not a valid port" under the old formula, so its host
 * controller's status register always fell through to the generic
 * unmapped-I/O response (all ones), and firmware's "wait for not busy"
 * poll on it spun forever. */
static bool sparse_io_port(uint64_t offset, uint64_t *port) {
    uint64_t p = (offset & 0xFFF) | ((offset >> 10) & ~0xFFFull);
    if (p > 0xFFFF)
        return false;
    *port = p;
    return true;
}

static bool vga_mem_window(Ia64I2000State *s, uint64_t addr, unsigned size,
                          uint32_t *voff) {
    uint32_t base, wsize;
    vga_ibm_aperture(&s->vga, &base, &wsize);
    if (addr < base || addr + size > (uint64_t)base + wsize)
        return false;
    *voff = (uint32_t)(addr - base);
    return true;
}

#define VGA_ROM_BASE 0xC0000ull

static bool vga_rom_window(uint64_t addr, unsigned size, uint32_t *voff) {
    if (addr < VGA_ROM_BASE || addr + size > VGA_ROM_BASE + 0x10000ull)
        return false;
    *voff = (uint32_t)(addr - VGA_ROM_BASE);
    return true;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)vga_ibm_mem_read(&s->vga, voff + i) << (i * 8);
        return v;
    }
    if (vga_rom_window(addr, size, &voff)) {
        uint64_t v = 0;
        memcpy(&v, s->vga_rom_shadow + voff, size);
        return v;
    }
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        if (s->flash_read_status) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++)
                v |= (uint64_t)s->flash_status << (i * 8);
            return v;
        }
        if (s->flash_read_id) {
            uint64_t v = 0;
            uint64_t off = addr - I2000_FLASH_BASE;
            for (unsigned i = 0; i < size; i++) {
                /* Intel manufacturer, firmware-recognized 1 MiB device. */
                uint8_t id = ((off + i) & 1) ? 0xAC : 0x89;
                v |= (uint64_t)id << (i * 8);
            }
            return v;
        }
        /* Reads through the top-of-4GiB window always see the ROM (the
         * recovery-image scan depends on it); only writes divert into the
         * RAM shadow, where the firmware reads them back through the low
         * alias. */
        uint64_t v = 0, off = addr - I2000_FLASH_BASE;
        if (off + size <= I2000_FLASH_SIZE)
            memcpy(&v, s->flash + off, size);
        return v;
    }
    if (addr - I2000_IO_BASE < I2000_IO_SIZE) {
        uint64_t port;
        if (sparse_io_port(addr - I2000_IO_BASE, &port))
            return io_port_read(s, port, size);
    }
    if (addr == I2000_SAC_CBNR && size == 4)
        return s->sac_cbnr;
    if (addr == I2000_SAC_CCSR && size == 4)
        return s->sac_ccsr;
    mmio_log(s, addr, 0, size, false);
    return ~0ull;
}

static uint64_t bus_fetch(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    /* Code fetches in the shadow window come from the flash ROM: on real
     * hardware the firmware executes through the top-of-4GiB ROM alias
     * while its data lives in the RAM shadow at the same offsets. Serving
     * fetches from the (mutable) RAM copy lets the SAL data-area clear
     * wipe the very code performing it. */
    /* Top-of-4GiB fetches execute the ROM itself even once the window is
     * RAM-shadowed for data; low shadow fetches read RAM (the firmware
     * patches handler code there at runtime). */
    uint64_t off = addr - I2000_FLASH_BASE;
    if (off < I2000_FLASH_SIZE) {
        uint64_t v = 0;
        if (off + size <= I2000_FLASH_SIZE)
            memcpy(&v, s->flash + off, size);
        return v;
    }
    return bus_read(ud, addr, size);
}

static void bus_write(void *ud, uint64_t addr, uint64_t val, unsigned size) {
    Ia64I2000State *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        for (unsigned i = 0; i < size; i++)
            vga_ibm_mem_write(&s->vga, voff + i, (uint8_t)(val >> (i * 8)));
        return;
    }
    /* Read-only, like a real (locked) option ROM shadow: this is what keeps
     * a generic "clear all of system RAM" firmware loop from wiping the
     * video BIOS out before it's ever used, since real hardware wouldn't
     * report this range as regular RAM in the first place. */
    if (vga_rom_window(addr, size, &voff))
        return;
    if (addr + size <= s->ram_size) {
        memcpy(s->ram + addr, &val, size);
        return;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t off = addr - I2000_FLASH_BASE;
        /* BIOS 1.30 probes the Intel flash device with the standard
         * clear-status/read-status/read-array command sequence.  Without
        * command-state handling the status read returns an array byte and
        * platform initialization reports EFI_OUT_OF_RESOURCES. */
        if (size == 1) {
            if (s->flash_cmd == 0x40 || s->flash_cmd == 0x10) {
                /* Intel byte-program operation.  Keep NVRAM updates in the
                 * in-memory image; the user's ROM file remains untouched. */
                s->flash[off] &= (uint8_t)val;
                s->flash_status = 0x80;
                s->flash_read_status = true;
                s->flash_read_id = false;
                s->flash_cmd = 0;
                return;
            }
            if (s->flash_cmd == 0x20) {
                if ((uint8_t)val == 0xD0) {
                    uint64_t block = s->flash_cmd_addr & ~0xFFFFull;
                    if (block < I2000_FLASH_SIZE)
                        memset(s->flash + block, 0xFF, 0x10000);
                    s->flash_status = 0x80;
                } else {
                    s->flash_status = 0xB0; /* ready + erase error */
                }
                s->flash_read_status = true;
                s->flash_read_id = false;
                s->flash_cmd = 0;
                return;
            }
            switch ((uint8_t)val) {
            case 0x50:                         /* clear status register */
                s->flash_status = 0x80;        /* ready, no errors */
                return;
            case 0x70:                         /* read status register */
                s->flash_read_status = true;
                s->flash_read_id = false;
                return;
            case 0x90:                         /* read identifier codes */
                s->flash_read_status = false;
                s->flash_read_id = true;
                return;
            case 0x10: case 0x40:              /* byte program setup */
                s->flash_cmd = (uint8_t)val;
                s->flash_cmd_addr = off;
                return;
            case 0x20:                         /* block erase setup */
                s->flash_cmd = 0x20;
                s->flash_cmd_addr = off;
                return;
            case 0xFF:                         /* read array */
                s->flash_read_status = false;
                s->flash_read_id = false;
                s->flash_cmd = 0;
                return;
            }
        }
        /* Shadowed: writes through the alias land in the RAM copy.
         * Unshadowed: flash programming cycles are ignored for now. */
        if (s->fw_shadow_enabled && off + size <= I2000_FLASH_SIZE &&
            I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
            memcpy(s->ram + I2000_FW_SHADOW_BASE + off, &val, size);
        return;
    }
    if (addr - I2000_IO_BASE < I2000_IO_SIZE) {
        uint64_t port;
        if (sparse_io_port(addr - I2000_IO_BASE, &port)) {
            io_port_write(s, port, val, size);
            return;
        }
    }
    if (addr == I2000_SAC_CBNR && size == 4) {
        s->sac_cbnr = (uint32_t)val;
        return;
    }
    if (addr == I2000_SAC_CCSR && size == 4) {
        s->sac_ccsr = (uint32_t)val;
        return;
    }
    mmio_log(s, addr, val, size, true);
}

uint8_t ia64_i2000_phys_read8(Ia64I2000State *s, uint64_t addr) {
    return (uint8_t)bus_read(s, addr & MERCED_PHYS_MASK, 1);
}
void ia64_i2000_phys_write8(Ia64I2000State *s, uint64_t addr, uint8_t val) {
    bus_write(s, addr & MERCED_PHYS_MASK, val, 1);
}

static bool i2000_bus_fill(void *ud, uint64_t addr, uint8_t val, uint64_t len) {
    Ia64I2000State *s = ud;
    if (addr > s->ram_size || len > s->ram_size - addr)
        return false;
    memset(s->ram + addr, val, (size_t)len);
    return true;
}

/* ── Firmware ────────────────────────────────────────────────────────────── */

bool ia64_i2000_load_firmware(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: cannot open firmware '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (uint64_t)len > I2000_FLASH_SIZE) {
        fprintf(stderr, "gemu: firmware '%s' is %ld bytes (expected at most %u)\n",
                path, len, I2000_FLASH_SIZE);
        fclose(f);
        return false;
    }
    uint32_t off = I2000_FLASH_SIZE - (uint32_t)len;
    memset(s->flash, 0xFF, off);
    size_t rd = fread(s->flash + off, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) {
        fprintf(stderr, "gemu: short read on firmware '%s'\n", path);
        return false;
    }
    snprintf(s->flash_file, sizeof(s->flash_file), "%s", path);
    s->flash_image_size = (uint32_t)len;
    s->flash_loaded = true;
    gemu_monitor_register_rom(s->monitor, (uint32_t)(I2000_FLASH_BASE + off),
                              (uint32_t)len, path);
    return true;
}

/* ── Front panel ─────────────────────────────────────────────────────────── */

#define C_BG     0xFF101418
#define C_TITLE  0xFFFFB000
#define C_TEXT   0xFFB0B8C0
#define C_DIM    0xFF586068
#define C_GOOD   0xFF40C040
#define C_BAD    0xFFE05050
#define C_CON    0xFF30D030

static void panel_text(Ia64I2000State *s, int col, int row,
                       uint32_t color, const char *str) {
    for (; *str && col < COLS; str++, col++) {
        char c = *str;
        if (c < 32 || c > 122) c = '?';
        const uint8_t *g = gemu_font_glyph(c);
        int px = col * CELL_W, py = row * CELL_H;
        for (int x = 0; x < CELL_W; x++)
            for (int y = 0; y < 8; y++)
                if (g[x] & (1 << y))
                    s->fb[(py + y) * FB_W + (px + x)] = color;
    }
}

static void panel_render(Ia64I2000State *s) {
    char line[COLS + 1];
    Merced *m = s->cpu;

    for (int i = 0; i < FB_W * FB_H; i++)
        s->fb[i] = C_BG;

    panel_text(s, 0, 0, C_TITLE,
               "HP i2000 - Intel Itanium (Merced) - GEMU front panel");
    snprintf(line, sizeof(line), "POST %02X", s->post_code);
    panel_text(s, COLS - 8, 0, C_TITLE, line);

    snprintf(line, sizeof(line),
             "IP %016" PRIX64 ".%u  insts %-12" PRIu64 " faults %" PRIu64,
             m->ip & ~0xFull, (unsigned)(m->ip & 0xF), m->ninsts, m->nfaults);
    panel_text(s, 0, 2, C_TEXT, line);

    snprintf(line, sizeof(line),
             "PSR %016" PRIX64 "  ic=%u i=%u it=%u dt=%u bn=%u  CFM sof=%u sol=%u",
             m->psr,
             (unsigned)((m->psr >> 13) & 1), (unsigned)((m->psr >> 14) & 1),
             (unsigned)((m->psr >> 36) & 1), (unsigned)((m->psr >> 17) & 1),
             (unsigned)((m->psr >> 44) & 1),
             (unsigned)(m->cfm & 0x7F), (unsigned)((m->cfm >> 7) & 0x7F));
    panel_text(s, 0, 3, C_TEXT, line);

    if (s->halted) {
        panel_text(s, 0, 5, C_BAD, "HALTED:");
        panel_text(s, 8, 5, C_BAD, m->halt_msg);
    } else {
        panel_text(s, 0, 5, C_GOOD, "RUNNING");
    }

    panel_text(s, 0, 7, C_DIM, "unhandled MMIO (addr / last value / count):");
    for (int i = 0; i < s->mmio_log_n; i++) {
        MmioLogEnt *e = &s->mmio_log[i];
        snprintf(line, sizeof(line), "%c 0x%012" PRIX64 " %u  0x%-16" PRIX64 " x%u",
                 e->is_write ? 'W' : 'R', e->addr, e->size, e->val, e->count);
        panel_text(s, 2, 8 + i, C_DIM, line);
    }

    int con_top = ROWS - CON_ROWS - 1;
    panel_text(s, 0, con_top, C_DIM,
               "--- COM1 ------------------------------------------------------------------------------------------------");
    for (int r = 0; r < CON_ROWS; r++)
        panel_text(s, 0, con_top + 1 + r, C_CON, s->console[r]);
}

/* ── Monitor callbacks ───────────────────────────────────────────────────── */

static void i2000_cpu_state(void *ud, char *buf, size_t buf_len) {
    Ia64I2000State *s = ud;
    merced_dump_state(s->cpu, buf, buf_len);
}

static void i2000_render_frame(Ia64I2000State *s) {
    vga_ibm_render(&s->vga, s->fb, FB_W, FB_H, vgafont16);
}

static bool i2000_screendump(void *ud, const char *path) {
    Ia64I2000State *s = ud;
    i2000_render_frame(s);
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

/* Snapshot format: full-machine save/restore so a slow, deterministic boot
 * only ever has to run once. Everything in Ia64I2000State and Merced is
 * plain data (fixed-size arrays and scalars) except a handful of pointers
 * that are only meaningful within one process's lifetime - live handles
 * (monitor/display), the CPU's back-reference and bus hookup, and buffers
 * whose CONTENTS need saving but whose addresses obviously can't be
 * (ram, flash, atapi_data). The save/restore strategy is: snapshot those
 * buffers' contents separately, then bulk-copy the rest of each struct in
 * one shot with the pointer fields blanked out (save) or preserved from the
 * live, already-correctly-allocated instance (load) - far less fragile
 * than hand-listing every scalar field, and it stays correct automatically
 * as fields get added. */
#define I2000_SNAPSHOT_MAGIC 0x32304B32554D4547ull /* "GEMU2K02" */
#define I2000_SNAPSHOT_VERSION 1u

static bool i2000_save_snapshot(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    uint64_t magic = I2000_SNAPSHOT_MAGIC;
    uint32_t version = I2000_SNAPSHOT_VERSION;
    ok &= fwrite(&magic, sizeof(magic), 1, f) == 1;
    ok &= fwrite(&version, sizeof(version), 1, f) == 1;
    ok &= fwrite(&s->ram_size, sizeof(s->ram_size), 1, f) == 1;
    ok &= fwrite(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fwrite(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;
    uint64_t atapi_len = (uint64_t)s->atapi_data_len;
    ok &= fwrite(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    if (atapi_len)
        ok &= fwrite(s->atapi_data, 1, atapi_len, f) == atapi_len;

    Ia64I2000State snap = *s;
    snap.monitor = NULL;
    snap.display = NULL;
    snap.cpu = NULL;
    snap.ram = NULL;
    snap.flash = NULL;
    snap.cdrom = NULL;
    snap.atapi_data = NULL;
    ok &= fwrite(&snap, sizeof(snap), 1, f) == 1;

    Merced cpu_snap = *s->cpu;
    memset(&cpu_snap.bus, 0, sizeof(cpu_snap.bus));
    ok &= fwrite(&cpu_snap, sizeof(cpu_snap), 1, f) == 1;

    fclose(f);
    return ok;
}

static bool i2000_load_snapshot(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = true;
    uint64_t magic = 0;
    uint32_t version = 0;
    uint64_t ram_size = 0;
    ok &= fread(&magic, sizeof(magic), 1, f) == 1;
    ok &= fread(&version, sizeof(version), 1, f) == 1;
    ok &= fread(&ram_size, sizeof(ram_size), 1, f) == 1;
    if (!ok || magic != I2000_SNAPSHOT_MAGIC ||
        version != I2000_SNAPSHOT_VERSION || ram_size != s->ram_size) {
        fclose(f);
        return false;
    }
    ok &= fread(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fread(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;

    uint64_t atapi_len = 0;
    ok &= fread(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    free(s->atapi_data);
    s->atapi_data = atapi_len ? malloc((size_t)atapi_len) : NULL;
    if (atapi_len) {
        if (!s->atapi_data) { fclose(f); return false; }
        ok &= fread(s->atapi_data, 1, atapi_len, f) == atapi_len;
    }

    GemuMonitor *monitor = s->monitor;
    GemuDisplay *display = s->display;
    Merced *cpu = s->cpu;
    uint8_t *ram = s->ram;
    uint8_t *flash = s->flash;
    uint8_t *atapi_data = s->atapi_data;
    if (s->cdrom) fclose(s->cdrom);
    Ia64I2000State loaded;
    ok &= fread(&loaded, sizeof(loaded), 1, f) == 1;
    if (ok) {
        *s = loaded;
        s->monitor = monitor;
        s->display = display;
        s->cpu = cpu;
        s->ram = ram;
        s->flash = flash;
        s->atapi_data = atapi_data;
        s->cdrom = s->cdrom_file[0] ? fopen(s->cdrom_file, "rb") : NULL;
        /* Restart the autosave clock from now, not from whatever wall time
         * the snapshot itself was taken at. */
        s->last_autosave = time(NULL);
        /* A snapshot taken right at (or after) a halt is exactly the point
         * of loading it back in - to keep going, typically to retest a
         * fix for whatever caused that halt. Don't leave it stuck. */
        s->halted = false;
    }

    MercedBus bus = s->cpu->bus;
    Merced loaded_cpu;
    ok &= fread(&loaded_cpu, sizeof(loaded_cpu), 1, f) == 1;
    if (ok) {
        *s->cpu = loaded_cpu;
        s->cpu->bus = bus;
    }

    fclose(f);
    return ok;
}

#define I2000_AUTOSAVE_DIR "/home/admin/jemu/snapshots"
#define I2000_AUTOSAVE_PERIOD_SEC 300

/* Called once per run_slice from the main loop. Wall-clock gated (not
 * instruction-count gated) since the whole point is a rollback point every
 * few minutes of real debugging time, regardless of how fast or slow any
 * particular stretch of firmware executes. Silent no-op until the first
 * period has elapsed, so a quick one-off run doesn't pay for a snapshot it
 * will never use. */
static void i2000_autosave_tick(Ia64I2000State *s) {
    time_t now = time(NULL);
    if (s->last_autosave == 0) {
        s->last_autosave = now;
        return;
    }
    if (now - s->last_autosave < I2000_AUTOSAVE_PERIOD_SEC)
        return;
    s->last_autosave = now;
    char path[256];
    snprintf(path, sizeof(path), "%s/autosave_%u.vmstate",
             I2000_AUTOSAVE_DIR, s->autosave_slot);
    s->autosave_slot ^= 1u;
    if (i2000_save_snapshot(s, path))
        printf("i2000: autosaved to %s (ninsts=%" PRIu64 ")\n",
               path, s->cpu->ninsts);
    else
        fprintf(stderr, "i2000: autosave to %s FAILED\n", path);
}

/* monitor "x 0xADDR [count]": physical memory hexdump
 * monitor "trace N": stderr-trace the next N executed slots
 * monitor "history [N]": dump the most recently executed slots
 * monitor "panel FILE": dump the CPU/MMIO debug front panel (the live
 * display and default screendump now show real VGA output instead)
 * monitor "savevm FILE": snapshot the full machine state to FILE
 * monitor "loadvm FILE": restore full machine state from FILE (run this
 * right after startup to skip re-running a slow, already-verified boot
 * prefix) */
static void i2000_custom_cmd(Ia64I2000State *s) {
    const char *txt = gemu_monitor_command_text(s->monitor);
    uint64_t addr;
    int count = 64;
    uint64_t n;
    char panel_path[256];
    char vm_path[256];
    if (txt && sscanf(txt, "savevm %255s", vm_path) == 1) {
        if (i2000_save_snapshot(s, vm_path))
            printf("saved snapshot to %s (ninsts=%" PRIu64 ")\n",
                   vm_path, s->cpu->ninsts);
        else
            printf("failed to save snapshot to %s\n", vm_path);
        return;
    }
    if (txt && sscanf(txt, "loadvm %255s", vm_path) == 1) {
        if (i2000_load_snapshot(s, vm_path))
            printf("loaded snapshot from %s (ninsts=%" PRIu64 ")\n",
                   vm_path, s->cpu->ninsts);
        else
            printf("failed to load snapshot from %s\n", vm_path);
        return;
    }
    if (txt && sscanf(txt, "panel %255s", panel_path) == 1) {
        panel_render(s);
        if (gemu_screendump_argb(panel_path, s->fb, FB_W, FB_H))
            printf("wrote debug panel to %s\n", panel_path);
        else
            printf("cannot write %s\n", panel_path);
        return;
    }
    if (txt && sscanf(txt, "trace %" SCNu64, &n) == 1) {
        s->cpu->trace_n = n;
        printf("tracing next %" PRIu64 " slots to stderr\n", n);
        return;
    }
    if (txt && strncmp(txt, "history", 7) == 0) {
        unsigned count = 128;
        (void)sscanf(txt + 7, "%u", &count);
        if (count > MERCED_TRACE_HISTORY) count = MERCED_TRACE_HISTORY;
        merced_dump_trace(s->cpu, count, stderr);
        return;
    }
    if (txt && strncmp(txt, "calls", 5) == 0) {
        merced_dump_calls(s->cpu, MERCED_CALL_HISTORY, stderr);
        return;
    }
    {
        uint64_t daddr, dlen;
        char path[256];
        if (txt && sscanf(txt, "dump %" SCNx64 " %" SCNx64 " %255s",
                          &daddr, &dlen, path) == 3) {
            FILE *f = fopen(path, "wb");
            if (!f) { printf("cannot open %s\n", path); return; }
            for (uint64_t i = 0; i < dlen; i++) {
                uint8_t b = (uint8_t)bus_read(s, daddr + i, 1);
                fwrite(&b, 1, 1, f);
            }
            fclose(f);
            printf("dumped 0x%" PRIX64 " bytes from 0x%" PRIX64 " to %s\n",
                   dlen, daddr, path);
            return;
        }
    }
    if (txt && sscanf(txt, "x %" SCNx64 " %d", &addr, &count) >= 1) {
        if (count > 1024) count = 1024;
        for (int i = 0; i < count; i += 16) {
            printf("%012" PRIX64 ": ", addr + (uint64_t)i);
            for (int j = 0; j < 16 && i + j < count; j++)
                printf("%02X ", (unsigned)bus_read(s, addr + (uint64_t)(i + j), 1));
            printf("\n");
        }
        return;
    }
    gemu_monitor_unknown_command(s->monitor);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

static void chipset_cfg_reset(Ia64I2000State *s) {
    s->flash_read_status = false;
    s->flash_read_id = false;
    s->flash_status = 0x80;
    s->flash_cmd = 0;
    s->flash_cmd_addr = 0;
    s->pci_cfg_addr = 0;
    s->ifb_smbus_cmd_read_once = false;
    s->chipset_bus = 0xFF;   /* 460GX power-on CBN default: top bus number */
    memset(s->chipset_cfg, 0, sizeof(s->chipset_cfg));
    memset(s->memcard_cfg, 0, sizeof(s->memcard_cfg));
    memset(s->ifb_cfg, 0, sizeof(s->ifb_cfg));
    memset(s->ifb_usb_cfg, 0, sizeof(s->ifb_usb_cfg));
    memset(s->ifb_smbus_cfg, 0, sizeof(s->ifb_smbus_cfg));
    memset(s->cmd649_cfg, 0, sizeof(s->cmd649_cfg));
    memset(s->acpi_io, 0, sizeof(s->acpi_io));
    s->uart_rx_head = s->uart_rx_tail = 0;
    memset(s->cmos, 0, sizeof(s->cmos));
    s->cmos_index = 0;
    s->cmos[0x0A] = 0x26;                         /* divider, 32.768 kHz */
    s->cmos[0x0B] = 0x02;                         /* 24-hour BCD mode */
    s->cmos[0x0D] = 0x80;                         /* CMOS power valid */
    /* Extended byte 3 bit 3: "previous boot completed" flag. The bootstrap
     * takes the PspRecover path (and wants wpgbios.bin from recovery
     * media) whenever it is clear, i.e. on CMOS loss. */
    s->cmos[0x03] = 0x08;
    s->atapi_error = s->atapi_features = 0;
    s->atapi_count = 1;
    s->atapi_lba_low = 1;
    s->atapi_lba_mid = 0x14;
    s->atapi_lba_high = 0xEB;
    s->atapi_device = 0xA0;
    s->atapi_status = s->cdrom ? 0x40 : 0;
    s->atapi_packet_pos = 0;
    free(s->atapi_data);
    s->atapi_data = NULL;
    s->atapi_data_len = s->atapi_data_pos = 0;

    /* Intel 460GX IFB PCI-to-LPC/FWH bridge at 00:03.0.  BIOS 1.30 writes
     * its device-specific configuration registers very early; treating
     * the function as absent made every read return all ones.  Reset values
     * are from SSDM 248704-001, chapter 11. */
    s->ifb_cfg[0x00] = 0x86; s->ifb_cfg[0x01] = 0x80; /* Intel */
    s->ifb_cfg[0x02] = 0x00; s->ifb_cfg[0x03] = 0x76; /* IFB 7600 */
    s->ifb_cfg[0x04] = 0x07; s->ifb_cfg[0x05] = 0x00;
    s->ifb_cfg[0x06] = 0x80; s->ifb_cfg[0x07] = 0x02;
    s->ifb_cfg[0x09] = 0x00; s->ifb_cfg[0x0A] = 0x01;
    s->ifb_cfg[0x0B] = 0x06; s->ifb_cfg[0x0E] = 0x80;
    s->ifb_cfg[0x4E] = 0xC1; s->ifb_cfg[0x4F] = 0x07;
    s->ifb_cfg[0x60] = s->ifb_cfg[0x61] = 0x80;
    s->ifb_cfg[0x62] = s->ifb_cfg[0x63] = 0x80;
    s->ifb_cfg[0x64] = 0x10;
    s->ifb_cfg[0x69] = 0x02;
    s->ifb_cfg[0x84] = 0x00; s->ifb_cfg[0x85] = 0x05;

    /* The remaining documented IFB functions are present even though the
     * UHCI and SMBus engines themselves are currently empty.  Advertising
     * their real identities lets EFI enumerate and then cleanly disable
     * them instead of treating configuration reads as master aborts. */
    s->ifb_usb_cfg[0x00] = 0x86; s->ifb_usb_cfg[0x01] = 0x80;
    s->ifb_usb_cfg[0x02] = 0x02; s->ifb_usb_cfg[0x03] = 0x76;
    s->ifb_usb_cfg[0x06] = 0x80; s->ifb_usb_cfg[0x07] = 0x02;
    s->ifb_usb_cfg[0x09] = 0x00; s->ifb_usb_cfg[0x0A] = 0x03;
    s->ifb_usb_cfg[0x0B] = 0x0C; s->ifb_usb_cfg[0x0E] = 0x00;
    s->ifb_usb_cfg[0x3D] = 0x04; s->ifb_usb_cfg[0x60] = 0x10;

    s->ifb_smbus_cfg[0x00] = 0x86; s->ifb_smbus_cfg[0x01] = 0x80;
    s->ifb_smbus_cfg[0x02] = 0x03; s->ifb_smbus_cfg[0x03] = 0x76;
    s->ifb_smbus_cfg[0x06] = 0x80; s->ifb_smbus_cfg[0x07] = 0x02;
    s->ifb_smbus_cfg[0x09] = 0x00; s->ifb_smbus_cfg[0x0A] = 0x05;
    s->ifb_smbus_cfg[0x0B] = 0x0C; s->ifb_smbus_cfg[0x0E] = 0x00;
    s->ifb_smbus_cfg[0x3D] = 0x02;

    /* Integrated CMD Technology PCI-649 Ultra ATA/100 controller at the
     * i2000 IFB's fixed 00:03.1 function. */
    s->cmd649_cfg[0x00] = 0x95; s->cmd649_cfg[0x01] = 0x10; /* 1095 */
    s->cmd649_cfg[0x02] = 0x49; s->cmd649_cfg[0x03] = 0x06; /* 0649 */
    s->cmd649_cfg[0x08] = 0x02;                    /* revision */
    s->cmd649_cfg[0x09] = 0x8A;                    /* native/legacy IDE */
    s->cmd649_cfg[0x0A] = 0x01;                    /* IDE subclass */
    s->cmd649_cfg[0x0B] = 0x01;                    /* mass storage */
    s->cmd649_cfg[0x0E] = 0x00;                    /* normal header */
    s->cmd649_cfg[0x3D] = 0x01;                    /* INTA */
    /* Device numbers and roles per SSDM 248704-001 Table 2-1 (Device
     * Mapping on Bus CBN): 00h/01h = SAC (82461GX), 04h = SDC (82462GX,
     * not GXB as an earlier version of this table assumed - GXB has no
     * fixed device number, it lives on an Expander bus per WXB/PXB/GXB
     * board population), 05h/06h = Memory Card A/B (handled via
     * memcard_cfg). Real vendor/device IDs aren't published in the
     * datasheet or SSDM; these placeholders just need to be internally
     * consistent and non-zero (0xFFFF reads as "no device"). */
    static const struct { uint8_t dev; uint16_t did; } ids[] = {
        { 0, 0x84E0 }, { 1, 0x84E0 },   /* SAC */
        { 4, 0x84E2 },                  /* SDC */
        { 0x10, 0x84E4 }, { 0x11, 0x84E4 }, { 0x12, 0x84E4 }, { 0x13, 0x84E4 },
        { 0x14, 0x84E4 }, { 0x15, 0x84E4 }, { 0x16, 0x84E4 }, { 0x17, 0x84E4 },
                                         /* Expander 0-3 buses a/b */
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint8_t *c = s->chipset_cfg[ids[i].dev][0];
        c[0] = 0x86; c[1] = 0x80;
        c[2] = (uint8_t)ids[i].did; c[3] = (uint8_t)(ids[i].did >> 8);
        c[0x0B] = 0x06;
    }
    /* SAC dev0/fn0 byte 60h: a capability-bit count that gates a firmware
     * loop walking the 32-bit feature mask at reg 70h and registering a
     * handler for each clear bit. Left at zero (the memset default), that
     * loop never runs even once, so a later, unconditional dispatch call
     * (hardcoded to request feature type 6) finds an empty handler list,
     * falls through to a NULL-derived default pointer, and eventually
     * crashes dereferencing stale low memory. Real hardware's value isn't
     * published anywhere we have; 20h matches the mask's full width so the
     * loop at least examines every bit reg 70h can report. */
    s->chipset_cfg[0][0][0x60] = 0x20;
    /* Memory Card A/B (dev 5/6) live in memcard_cfg, not chipset_cfg - give
     * their function 0 a real PCI identity too, or firmware sees an all-zero
     * vendor ID at CBN:05.0/06.0 and concludes the card is absent before it
     * ever reads the processor descriptor at CBN:05.2 below. */
    static const struct { uint8_t card; uint16_t did; } mcids[] = {
        { 0, 0x84E3 }, { 1, 0x84E3 },   /* Memory Card A/B */
    };
    for (unsigned i = 0; i < sizeof(mcids) / sizeof(mcids[0]); i++) {
        uint8_t *c = s->memcard_cfg[mcids[i].card][0];
        c[0] = 0x86; c[1] = 0x80;
        c[2] = (uint8_t)mcids[i].did; c[3] = (uint8_t)(mcids[i].did >> 8);
        c[0x0B] = 0x06;
    }

    /* The SDV firmware enumerates processors through the 460GX system-bus
     * configuration mechanism.  Its address format is PCI-like: processor
     * zero is CBN:05.2, and byte 02h contains the presence/type code 4.
     * Bytes 03h-05h form the family/model/revision signature used to group
     * compatible processors.  Advertise the one Merced CPU implemented by
     * this machine; leaving this function all zero makes SAL conclude that
     * no processor exists and deliberately enter its timer-calibrated park
     * loop at FFFE2020.
     *
     * This must stay consistent with cpuid[3]'s family/model/revision in
     * merced_reset() (cpu/merced.c) - firmware cross-checks the two, and a
     * mismatch also lands in the FFFE2020 park loop as if no (recognized)
     * processor were present. */
    s->memcard_cfg[0][2][0x02] = 4;  /* processor present */
    s->memcard_cfg[0][2][0x03] = 7;  /* Itanium family */
    s->memcard_cfg[0][2][0x04] = 0;  /* Merced model */
    s->memcard_cfg[0][2][0x05] = 0;  /* revision - must match cpuid[3] */
}

Ia64I2000State *ia64_i2000_create(const Ia64Config *cfg) {
    Ia64I2000State *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    mkdir(I2000_AUTOSAVE_DIR, 0755); /* ignore EEXIST/already-there */

    s->pic_master_mask = 0xFF;
    s->pic_slave_mask = 0xFF;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    s->flash = malloc(I2000_FLASH_SIZE);
    if (!s->ram || !s->flash) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_i2000_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xFF, I2000_FLASH_SIZE);
    vga_ibm_reset(&s->vga);
    memset(s->vga_rom_shadow, 0xFF, sizeof(s->vga_rom_shadow));
    memcpy(s->vga_rom_shadow, vgabios_rom, vgabios_rom_len);
    if (cfg->cdrom_path) {
        s->cdrom = fopen(cfg->cdrom_path, "rb");
        if (!s->cdrom || fseek(s->cdrom, 0, SEEK_END) != 0) {
            fprintf(stderr, "gemu: cannot open CD-ROM image '%s'\n", cfg->cdrom_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        long end = ftell(s->cdrom);
        if (end <= 0 || (end % 2048) != 0 || fseek(s->cdrom, 0, SEEK_SET) != 0) {
            fprintf(stderr, "gemu: CD-ROM image '%s' is not a sector-aligned ISO\n",
                    cfg->cdrom_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        s->cdrom_size = (uint64_t)end;
        snprintf(s->cdrom_file, sizeof(s->cdrom_file), "%s", cfg->cdrom_path);
    }
    chipset_cfg_reset(s);

    MercedBus bus = {
        .ud = s, .read = bus_read, .fetch = bus_fetch, .write = bus_write,
        .fill = i2000_bus_fill
    };
    s->cpu = merced_create(&bus);
    if (!s->cpu) {
        ia64_i2000_destroy(s);
        return NULL;
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, i2000_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, i2000_screendump, s);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = FB_W,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .ini_section = "i2000",
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            ia64_i2000_destroy(s);
            return NULL;
        }
    }
    return s;
}

void ia64_i2000_destroy(Ia64I2000State *s) {
    if (!s)
        return;
    gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    merced_destroy(s->cpu);
    free(s->atapi_data);
    if (s->cdrom) fclose(s->cdrom);
    free(s->flash);
    free(s->ram);
    free(s);
}

/* ── Execution ───────────────────────────────────────────────────────────── */

static void i2000_poll_interrupts(Ia64I2000State *s) {
    if (s->legacy_irq_routed && !(s->pic_master_mask & 1) && s->pit0_next_irq &&
        s->cpu->ninsts >= s->pit0_next_irq) {
        merced_raise_external(s->cpu, s->pic_master_base);
        s->pit0_next_irq = s->cpu->ninsts + 100000;
    }
}

static void i2000_report_halt(Ia64I2000State *s) {
    Merced *m = s->cpu;
    char buf[4096];
    fprintf(stderr, "\ni2000: CPU halted after %" PRIu64 " instructions\n"
                    "i2000: %s\n",
            m->ninsts, m->halt_msg);
    uint64_t bpa = m->halt_ip & ~0xFull & MERCED_PHYS_MASK;
    fprintf(stderr, "i2000: bundle @ 0x%012" PRIX64 ":", bpa);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %02X", (unsigned)bus_read(s, bpa + (uint64_t)i, 1));
    fprintf(stderr, "\n");
    fprintf(stderr, "i2000: recent instruction slots:\n");
    merced_dump_trace(m, HALT_TRACE_LINES, stderr);
    fprintf(stderr, "i2000: recent calls/returns:\n");
    merced_dump_calls(m, HALT_CALL_LINES, stderr);
    fprintf(stderr, "i2000: translation registers:\n");
    for (unsigned i = 0; i < MERCED_N_TR; i++) {
        const MercedTlbEntry *it = &m->itr[i];
        const MercedTlbEntry *dt = &m->dtr[i];
        if (it->valid)
            fprintf(stderr, "  itr[%u] rid=%06X va=%016" PRIX64
                            "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                    i, it->rid, it->va_start, it->va_end, it->pfn_base, it->ps);
        if (dt->valid)
            fprintf(stderr, "  dtr[%u] rid=%06X va=%016" PRIX64
                            "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                    i, dt->rid, dt->va_start, dt->va_end, dt->pfn_base, dt->ps);
    }
    merced_dump_state(m, buf, sizeof(buf));
    fputs(buf, stderr);
}

static void i2000_run_slice(Ia64I2000State *s) {
    if (s->halted)
        return;
    for (int i = 0; i < INSTR_PER_FRAME; i++) {
        if (gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
            return;
        i2000_poll_interrupts(s);
        MercedStatus st = merced_step(s->cpu);
        if (s->reset_requested) {
            fprintf(stderr, "i2000: firmware requested a platform reset\n");
            /* CF9 resets the processor while the 460GX retains the sticky
             * configuration state SAL just programmed.  In particular,
             * SAC CBNR bit 0 selects the second bootstrap phase, with the
             * firmware range shadowed in RAM: the flash image is copied to
             * 0x03C00000 and the top-of-4GiB window becomes RAM-backed for
             * reads AND writes (the firmware's data segment lives up there
             * - its physical-mode stores via the 0xFFFDxxxx alias must
             * stick, or the SAL descriptors stay empty). */
            s->fw_shadow_enabled = true;
            if (I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
                memcpy(s->ram + I2000_FW_SHADOW_BASE, s->flash,
                       I2000_FLASH_SIZE);
            merced_reset(s->cpu);
            s->halted = false;
            s->reset_requested = false;
            s->mmio_log_n = 0;
            memset(s->mmio_log, 0, sizeof(s->mmio_log));
            printf("i2000: processor reset, IP=0x%016" PRIX64 "\n",
                   s->cpu->ip);
            return;
        }
        if (st != MERCED_OK) {
            s->halted = true;
            s->halt_status = st;
            i2000_report_halt(s);
            break;
        }
    }
}

static void i2000_reset(Ia64I2000State *s) {
    merced_reset(s->cpu);
    vga_ibm_reset(&s->vga);
    /* Standard VGA option ROM, shadowed at its conventional address so any
     * legacy option-ROM scan (0x55 0xAA signature check) finds it, the same
     * way a real add-in VGA card's ROM would appear at boot. */
    if (0xC0000ull + vgabios_rom_len <= s->ram_size)
        memcpy(s->ram + 0xC0000, vgabios_rom, vgabios_rom_len);
    s->halted = false;
    s->reset_requested = false;
    s->fw_shadow_enabled = false;
    s->post_code = 0;
    s->legacy_irq_routed = false;
    s->sac_cbnr = s->sac_ccsr = 0;
    s->port61 = s->pit2_polls = 0;
    s->pic_master_mask = s->pic_slave_mask = 0xFF;
    s->pic_master_base = 0x08;
    s->pic_slave_base = 0x70;
    s->pic_master_icw = s->pic_slave_icw = 0;
    s->pit0_reload = s->pit0_latch = 0;
    s->pit0_write_phase = 0;
    s->pit0_next_irq = 0;
    chipset_cfg_reset(s);
    s->mmio_log_n = 0;
    memset(s->mmio_log, 0, sizeof(s->mmio_log));
    memset(s->console, 0, sizeof(s->console));
    s->con_row = s->con_col = 0;
    printf("i2000: reset, IP=0x%016" PRIX64 "\n", s->cpu->ip);
}

void ia64_i2000_run(Ia64I2000State *s, const Ia64Config *cfg) {
    printf("gemu-ia64: HP i2000, Intel Itanium (Merced), 460GX chipset\n"
           "  RAM   : %" PRIu64 " MiB\n"
           "  Flash : %s (mapped at 0x%08" PRIX64 "-0xFFFFFFFF)\n"
           "  Reset : IP=0x00000000%08" PRIX64 " (PALE_RESET)\n",
           s->ram_size >> 20,
           s->flash_loaded ? s->flash_file : "(none)",
           (uint64_t)I2000_FLASH_BASE, (uint64_t)IA64_RESET_VECTOR);
    if (s->cdrom)
        printf("  CD-ROM: %s (%" PRIu64 " MiB)\n", s->cdrom_file,
               s->cdrom_size >> 20);

    gemu_monitor_start(s->monitor);

    bool running = true;
    while (running) {
        if (s->display) {
            gemu_display_poll(s->display);
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0) {
                uint8_t next = (uint8_t)(s->uart_rx_tail + 1);
                if (next != s->uart_rx_head && cp <= 0x7F) {
                    s->uart_rx[s->uart_rx_tail] = (uint8_t)cp;
                    s->uart_rx_tail = next;
                }
            }
            if (gemu_display_should_quit(s->display)) {
                if (cfg->no_shutdown) gemu_display_clear_flags(s->display);
                else running = false;
            }
            if (gemu_display_reset_requested(s->display)) {
                i2000_reset(s);
                gemu_display_clear_flags(s->display);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { running = false; }
                break;
            case GEMU_MON_RESET:
                i2000_reset(s);
                break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                s->halted = false;
                for (uint32_t k = 0; k < n && !s->halted; k++) {
                    if (gemu_monitor_check_exec(s->monitor,
                                                (uint32_t)s->cpu->ip))
                        break;
                    i2000_poll_interrupts(s);
                    MercedStatus st = merced_step(s->cpu);
                    if (st != MERCED_OK) {
                        s->halted = true;
                        s->halt_status = st;
                        i2000_report_halt(s);
                    }
                }
                char buf[512];
                snprintf(buf, sizeof(buf), "IP=0x%016" PRIX64 " insts=%" PRIu64 "\n",
                         s->cpu->ip, s->cpu->ninsts);
                fputs(buf, stdout);
                break;
            }
            case GEMU_MON_CUSTOM:
                i2000_custom_cmd(s);
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        if (!running)
            break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            i2000_run_slice(s);
            if (!s->halted)
                i2000_autosave_tick(s);
        }

        if (s->display) {
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));
            i2000_render_frame(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            gemu_sleep_ms(s->halted ? 30 : 1);
        } else {
            gemu_sleep_ms(s->halted ? 30 : 0);
        }
    }
    gemu_monitor_stop(s->monitor);
}
