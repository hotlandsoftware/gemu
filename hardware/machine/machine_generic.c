#include "generic.h"
#include "merced.h"
#include "vga_ibm.h"
#include "vgafont16.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 640
#define FB_H 400
#define INSTR_PER_FRAME 500000
#define HALT_TRACE_LINES 32
#define HALT_CALL_LINES 32

struct Ia64GenericState {
    GemuMonitor *monitor;
    GemuDisplay *display;
    Merced      *cpu;

    uint8_t  *ram;
    uint64_t  ram_size;
    uint8_t   rom[GENERIC_ROM_SIZE];
    char      rom_file[512];
    bool      rom_loaded;

    bool         halted;
    MercedStatus halt_status;

    VgaIbm    vga;
    uint32_t  fb[FB_W * FB_H];
};

/* ── Physical address space ─────────────────────────────────────────────── */

static bool vga_mem_window(Ia64GenericState *s, uint64_t addr, unsigned size,
                           uint32_t *voff) {
    uint32_t base, wsize;
    vga_ibm_aperture(&s->vga, &base, &wsize);
    if (addr < base || addr + size > (uint64_t)base + wsize)
        return false;
    *voff = (uint32_t)(addr - base);
    return true;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64GenericState *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)vga_ibm_mem_read(&s->vga, voff + i) << (i * 8);
        return v;
    }
    if (addr >= GENERIC_VGA_IO_BASE &&
        addr + size <= GENERIC_VGA_IO_BASE + GENERIC_VGA_IO_SIZE) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)vga_ibm_io_read(&s->vga,
                    (uint16_t)(0x3B0 + (addr - GENERIC_VGA_IO_BASE) + i)) << (i * 8);
        return v;
    }
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr >= GENERIC_ROM_BASE && addr + size <= GENERIC_ROM_BASE + GENERIC_ROM_SIZE) {
        uint64_t v = 0;
        memcpy(&v, s->rom + (addr - GENERIC_ROM_BASE), size);
        return v;
    }
    return ~0ull;
}

static void bus_write(void *ud, uint64_t addr, uint64_t val, unsigned size) {
    Ia64GenericState *s = ud;
    uint32_t voff;
    if (vga_mem_window(s, addr, size, &voff)) {
        for (unsigned i = 0; i < size; i++)
            vga_ibm_mem_write(&s->vga, voff + i, (uint8_t)(val >> (i * 8)));
        return;
    }
    if (addr >= GENERIC_VGA_IO_BASE &&
        addr + size <= GENERIC_VGA_IO_BASE + GENERIC_VGA_IO_SIZE) {
        for (unsigned i = 0; i < size; i++)
            vga_ibm_io_write(&s->vga,
                    (uint16_t)(0x3B0 + (addr - GENERIC_VGA_IO_BASE) + i),
                    (uint8_t)(val >> (i * 8)));
        return;
    }
    if (addr + size <= s->ram_size) {
        memcpy(s->ram + addr, &val, size);
        return;
    }
    /* ROM: read-only, like real (unprogrammed) flash - writes are dropped. */
}

/* Code and data share the same view - our own firmware, no legacy shadow
 * dance to model (see hardware/generic.h). */
static uint64_t bus_fetch(void *ud, uint64_t addr, unsigned size) {
    return bus_read(ud, addr, size);
}

static bool bus_fill(void *ud, uint64_t addr, uint8_t val, uint64_t len) {
    Ia64GenericState *s = ud;
    if (addr + len <= s->ram_size) {
        memset(s->ram + addr, val, (size_t)len);
        return true;
    }
    return false;
}

/* ── Firmware loading ────────────────────────────────────────────────────── */

bool ia64_generic_load_firmware(Ia64GenericState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: cannot open firmware '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (uint64_t)len > GENERIC_ROM_SIZE) {
        fprintf(stderr, "gemu: firmware '%s' is %ld bytes (expected at most %u)\n",
                path, len, GENERIC_ROM_SIZE);
        fclose(f);
        return false;
    }
    uint32_t off = GENERIC_ROM_SIZE - (uint32_t)len;
    memset(s->rom, 0xFF, off);
    size_t rd = fread(s->rom + off, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) {
        fprintf(stderr, "gemu: short read on firmware '%s'\n", path);
        return false;
    }
    snprintf(s->rom_file, sizeof(s->rom_file), "%s", path);
    s->rom_loaded = true;
    gemu_monitor_register_rom(s->monitor, (uint32_t)(GENERIC_ROM_BASE + off),
                              (uint32_t)len, path);
    return true;
}

/* ── Monitor / display glue ──────────────────────────────────────────────── */

static void generic_cpu_state(void *ud, char *buf, size_t buf_len) {
    Ia64GenericState *s = ud;
    merced_dump_state(s->cpu, buf, buf_len);
}

static void generic_render_frame(Ia64GenericState *s) {
    vga_ibm_render(&s->vga, s->fb, FB_W, FB_H, vgafont16);
}

static bool generic_screendump(void *ud, const char *path) {
    Ia64GenericState *s = ud;
    generic_render_frame(s);
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

static void generic_custom_cmd(Ia64GenericState *s) {
    const char *txt = gemu_monitor_command_text(s->monitor);
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
    gemu_monitor_unknown_command(s->monitor);
}

static void generic_report_halt(Ia64GenericState *s) {
    Merced *m = s->cpu;
    char buf[4096];
    fprintf(stderr, "\ngeneric: CPU halted after %" PRIu64 " instructions\n"
                    "generic: %s\n",
            m->ninsts, m->halt_msg);
    fprintf(stderr, "generic: recent instruction slots:\n");
    merced_dump_trace(m, HALT_TRACE_LINES, stderr);
    fprintf(stderr, "generic: recent calls/returns:\n");
    merced_dump_calls(m, HALT_CALL_LINES, stderr);
    merced_dump_state(m, buf, sizeof(buf));
    fputs(buf, stderr);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Ia64GenericState *ia64_generic_create(const GenericConfig *cfg) {
    Ia64GenericState *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    if (!s->ram) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_generic_destroy(s);
        return NULL;
    }
    memset(s->rom, 0xFF, sizeof(s->rom));
    vga_ibm_reset(&s->vga);

    MercedBus bus = {
        .ud = s, .read = bus_read, .fetch = bus_fetch, .write = bus_write,
        .fill = bus_fill
    };
    s->cpu = merced_create(&bus);
    if (!s->cpu) {
        ia64_generic_destroy(s);
        return NULL;
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, generic_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, generic_screendump, s);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = FB_W,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .ini_section = "generic",
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            ia64_generic_destroy(s);
            return NULL;
        }
    }
    return s;
}

void ia64_generic_destroy(Ia64GenericState *s) {
    if (!s)
        return;
    if (s->display) gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    if (s->cpu) merced_destroy(s->cpu);
    free(s->ram);
    free(s);
}

void ia64_generic_run(Ia64GenericState *s, const GenericConfig *cfg) {
    (void)cfg;
    gemu_monitor_start(s->monitor);

    bool running = true;
    while (running) {
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                running = false;
                break;
            case GEMU_MON_RESET:
                merced_reset(s->cpu);
                vga_ibm_reset(&s->vga);
                s->halted = false;
                printf("generic: processor reset, IP=0x%016" PRIX64 "\n",
                       s->cpu->ip);
                break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                s->halted = false;
                for (uint32_t i = 0; i < n && !s->halted; i++) {
                    if (gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
                        break;
                    MercedStatus st = merced_step(s->cpu);
                    if (st != MERCED_OK) {
                        s->halted = true;
                        s->halt_status = st;
                        generic_report_halt(s);
                    }
                }
                char buf[128];
                snprintf(buf, sizeof(buf), "IP=0x%016" PRIX64 " insts=%" PRIu64 "\n",
                         s->cpu->ip, s->cpu->ninsts);
                fputs(buf, stdout);
                break;
            }
            case GEMU_MON_CUSTOM:
                generic_custom_cmd(s);
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        if (!running)
            break;

        if (!gemu_monitor_is_paused(s->monitor) && !s->halted) {
            for (int i = 0; i < INSTR_PER_FRAME; i++) {
                if (gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
                    break;
                MercedStatus st = merced_step(s->cpu);
                if (st != MERCED_OK) {
                    s->halted = true;
                    s->halt_status = st;
                    generic_report_halt(s);
                    break;
                }
            }
        }

        if (s->display) {
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));
            generic_render_frame(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            gemu_sleep_ms(s->halted ? 30 : 1);
        } else {
            gemu_sleep_ms(s->halted ? 30 : 0);
        }
    }
    gemu_monitor_stop(s->monitor);
}
