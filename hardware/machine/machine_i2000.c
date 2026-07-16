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
#define MMIO_LOG_N 8

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
    MercedStatus halt_status;

    /* front panel */
    uint32_t  fb[FB_W * FB_H];
    char      console[CON_ROWS][COLS + 1];
    int       con_row, con_col;
    bool      con_dirty;

    /* devices */
    uint8_t   post_code;
    uint32_t  sac_cbnr, sac_ccsr;
    uint8_t   uart_ier, uart_lcr, uart_mcr, uart_scr, uart_dll, uart_dlm;

    MmioLogEnt mmio_log[MMIO_LOG_N];
    int        mmio_log_n;
};

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
    if (port == 0x61) return 0x00;                  /* port B */
    mmio_log(s, I2000_IO_BASE + port, 0, size, false);
    return ~0ull;
}

static void io_port_write(Ia64I2000State *s, uint64_t port, uint64_t val,
                          unsigned size) {
    if (port == POST_PORT) {
        s->post_code = (uint8_t)val;
        return;
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
    mmio_log(s, I2000_IO_BASE + port, val, size, true);
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t v = 0, off = addr - I2000_FLASH_BASE;
        if (off + size <= I2000_FLASH_SIZE)
            memcpy(&v, s->flash + off, size);
        return v;
    }
    if (addr - I2000_IO_BASE < I2000_IO_SIZE)
        return io_port_read(s, addr - I2000_IO_BASE, size);
    if (addr == I2000_SAC_CBNR && size == 4)
        return s->sac_cbnr;
    if (addr == I2000_SAC_CCSR && size == 4)
        return s->sac_ccsr;
    mmio_log(s, addr, 0, size, false);
    return ~0ull;
}

static void bus_write(void *ud, uint64_t addr, uint64_t val, unsigned size) {
    Ia64I2000State *s = ud;
    if (addr + size <= s->ram_size) {
        memcpy(s->ram + addr, &val, size);
        return;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE)
        return;                                     /* flash writes ignored */
    if (addr - I2000_IO_BASE < I2000_IO_SIZE) {
        io_port_write(s, addr - I2000_IO_BASE, val, size);
        return;
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
 * monitor "trace N": stderr-trace the next N executed slots */
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

    MercedBus bus = { .ud = s, .read = bus_read, .write = bus_write };
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
    merced_dump_trace(m, 32, stderr);
    merced_dump_state(m, buf, sizeof(buf));
    fputs(buf, stderr);
}

static void i2000_run_slice(Ia64I2000State *s) {
    if (s->halted)
        return;
    for (int i = 0; i < INSTR_PER_FRAME; i++) {
        MercedStatus st = merced_step(s->cpu);
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
    s->post_code = 0;
    s->sac_cbnr = s->sac_ccsr = 0;
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
