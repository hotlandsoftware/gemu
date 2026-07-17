#include "i2000.h"
#include "merced.h"
#include "input_menu.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint32_t  sac_cbnr, sac_ccsr;
    uint8_t   port61;
    uint8_t   pit2_polls;
    uint8_t   cmos_index;
    uint8_t   cmos[128];
    uint32_t  pci_cfg_addr;
    uint8_t   cmd649_cfg[256];
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

    MmioLogEnt mmio_log[MMIO_LOG_N];
    int        mmio_log_n;
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

static uint64_t io_port_read(Ia64I2000State *s, uint64_t port, unsigned size) {
    if (port == 0x73 && size == 1)
        return s->cmos[s->cmos_index & 0x7F];
    if (port == PCI_CFG_ADDR && size == 4) return s->pci_cfg_addr;
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4)
        return pci_cfg_read(s, (unsigned)(port - PCI_CFG_DATA), size);
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        switch (port - COM1_PORT) {
        case 0: return (s->uart_lcr & 0x80) ? s->uart_dll : 0x00;  /* RBR/DLL */
        case 1: return (s->uart_lcr & 0x80) ? s->uart_dlm : s->uart_ier;
        case 2: return 0x01;                        /* IIR: no int pending */
        case 3: return s->uart_lcr;
        case 4: return s->uart_mcr;
        case 5: return 0x60;                        /* LSR: THR empty+idle */
        case 6: return 0xB0;                        /* MSR: CTS|DSR|DCD */
        case 7: return s->uart_scr;
        }
    }
    if (port == 0x21 || port == 0xA1) return 0xFF;  /* PIC masks */
    if (port == 0x404)
        /* System status/GPIO word. The bootstrap reads this once: bits 24
         * and 19 both set means the BIOS recovery jumper is installed and
         * sends the whole boot into PspRecover. 0 = Normal position. */
        return 0;
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
    if (port == 0x72 && size == 1) {
        /* The i2000 IFB exposes its RTC/configuration RAM at 72h/73h. */
        s->cmos_index = (uint8_t)val & 0x7F;
        return;
    }
    if (port == 0x73 && size == 1) {
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
    if (port == 0x43) return;                       /* PIT mode control */
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
 *   offset = port + ((port & ~3) << 10)
 * The low two address bits select the byte lane. */
static bool sparse_io_port(uint64_t offset, uint64_t *port) {
    uint64_t lane = offset & 3;
    uint64_t p = (offset + (lane << 10)) / 1025;
    if (p > 0xFFFF || p + ((p & ~3ull) << 10) != offset)
        return false;
    *port = p;
    return true;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
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
    if (addr + size <= s->ram_size) {
        memcpy(s->ram + addr, &val, size);
        return;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t off = addr - I2000_FLASH_BASE;
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

static bool i2000_screendump(void *ud, const char *path) {
    Ia64I2000State *s = ud;
    panel_render(s);
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

/* monitor "x 0xADDR [count]": physical memory hexdump
 * monitor "trace N": stderr-trace the next N executed slots
 * monitor "history [N]": dump the most recently executed slots */
static void i2000_custom_cmd(Ia64I2000State *s) {
    const char *txt = gemu_monitor_command_text(s->monitor);
    uint64_t addr;
    int count = 64;
    uint64_t n;
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
    s->pci_cfg_addr = 0;
    s->chipset_bus = 0xFF;   /* 460GX power-on CBN default: top bus number */
    memset(s->chipset_cfg, 0, sizeof(s->chipset_cfg));
    memset(s->memcard_cfg, 0, sizeof(s->memcard_cfg));
    memset(s->cmd649_cfg, 0, sizeof(s->cmd649_cfg));
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
    s->halted = false;
    s->reset_requested = false;
    s->fw_shadow_enabled = false;
    s->post_code = 0;
    s->sac_cbnr = s->sac_ccsr = 0;
    s->port61 = s->pit2_polls = 0;
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

        if (!gemu_monitor_is_paused(s->monitor))
            i2000_run_slice(s);

        if (s->display) {
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));
            panel_render(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            gemu_sleep_ms(s->halted ? 30 : 1);
        } else {
            gemu_sleep_ms(s->halted ? 30 : 0);
        }
    }
    gemu_monitor_stop(s->monitor);
}
