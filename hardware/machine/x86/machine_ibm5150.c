#include "ibm5150.h"
#include "cgafont8x8.h"
#include "gemu/util.h"
#include <stdio.h>
#include <string.h>

#define IBM5150_BASIC_BASE 0xF6000u
#define IBM5150_BIOS_BASE  0xFE000u
#define IBM5150_CGA_BASE   0xB8000u
#define IBM5150_CGA_END    0xBC000u /* B8000-BBFFF, 16K */

#define IBM5150_INSTR_PER_FRAME  20000
#define IBM5150_PIT_TICKS_PER_FRAME 4000 /* not wall-clock accurate this pass -
                                           * see the run-loop comment below */

/* ── CPU <-> memory/IO bus ───────────────────────────────────────────────── */

static uint8_t bus_read8(uint32_t addr, void *ud) {
    Ibm5150State *s = ud;
    if (addr < s->ram_size) return s->ram[addr];
    if (addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END)
        return cga_mem_read(&s->cga, addr - IBM5150_CGA_BASE);
    if (s->has_basic && addr >= IBM5150_BASIC_BASE && addr < IBM5150_BASIC_BASE + IBM5150_BASIC_SIZE)
        return s->basic[addr - IBM5150_BASIC_BASE];
    if (addr >= IBM5150_BIOS_BASE && addr < IBM5150_BIOS_BASE + IBM5150_BIOS_SIZE)
        return s->bios[addr - IBM5150_BIOS_BASE];
    return 0xFF;
}
static void bus_write8(uint32_t addr, uint8_t val, void *ud) {
    Ibm5150State *s = ud;
    if (addr < s->ram_size) { s->ram[addr] = val; return; }
    if (addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END) { cga_mem_write(&s->cga, addr - IBM5150_CGA_BASE, val); return; }
    /* ROM regions: writes silently ignored, matching real hardware */
}

static void iolog(Ibm5150State *s, uint16_t port, bool is_write, uint8_t val) {
    for (int i = 0; i < s->iolog_n; i++) {
        if (s->iolog[i].port == port && s->iolog[i].is_write == is_write) {
            s->iolog[i].count++; s->iolog[i].val = val; return;
        }
    }
    if (s->iolog_n < IBM5150_IOLOG_N) {
        Ibm5150IoLogEnt *e = &s->iolog[s->iolog_n++];
        e->port = port; e->is_write = is_write; e->val = val; e->count = 1;
        if (is_write) fprintf(stderr, "ibm5150: unhandled write port=%#06x val=%#04x\n", port, val);
        else          fprintf(stderr, "ibm5150: unhandled read  port=%#06x\n", port);
    }
}

static uint8_t io_read8(uint16_t port, void *ud) {
    Ibm5150State *s = ud;
    if (port >= 0x20 && port <= 0x21) return i8259_io_read(&s->pic, port);
    if (port >= 0x40 && port <= 0x43) return i8253_io_read(&s->pit, port);
    if (port >= 0x60 && port <= 0x63) return i8255_io_read(&s->ppi, port);
    if (port >= 0x3D0 && port <= 0x3DF) return cga_io_read(&s->cga, port);
    iolog(s, port, false, 0);
    return 0xFF;
}
static void io_write8(uint16_t port, uint8_t val, void *ud) {
    Ibm5150State *s = ud;
    if (port >= 0x20 && port <= 0x21) { i8259_io_write(&s->pic, port, val); return; }
    if (port >= 0x40 && port <= 0x43) { i8253_io_write(&s->pit, port, val); return; }
    if (port >= 0x60 && port <= 0x63) { i8255_io_write(&s->ppi, port, val); return; }
    if (port >= 0x3D0 && port <= 0x3DF) { cga_io_write(&s->cga, port, val); return; }
    iolog(s, port, true, val);
}

static void on_unknown_opcode(X86Cpu *cpu, uint8_t opcode, void *ud) {
    Ibm5150State *s = ud;
    static bool seen[256];
    if (!seen[opcode]) {
        seen[opcode] = true;
        fprintf(stderr, "ibm5150: unimplemented opcode %#04x at %04x:%04x\n",
                opcode, cpu->cs, cpu->ip);
    }
    /* Stop cleanly rather than desync decode by guessing - keeps whatever
     * BIOS POST progress happened so far inspectable via the monitor. */
    s->halted_for_debug = true;
    cpu->halted = true;
}

/* ── Keyboard: XT scan code set 1, polled via gemu_display_is_key_held()
 * (the same full-keyboard pattern machine_pecom.c uses) since a PC
 * keyboard has far more keys than the 32-bit action-bitmask model fits.
 * Covers the keys BIOS/DOS-era interactive use actually needs; extending
 * this table is cheap. ────────────────────────────────────────────────── */

typedef struct { const char *name; uint8_t code; } XtKeyEnt;
static const XtKeyEnt XT_KEYMAP[] = {
    {"Escape",0x01},{"1",0x02},{"2",0x03},{"3",0x04},{"4",0x05},{"5",0x06},
    {"6",0x07},{"7",0x08},{"8",0x09},{"9",0x0A},{"0",0x0B},{"-",0x0C},{"=",0x0D},
    {"Backspace",0x0E},{"Tab",0x0F},
    {"Q",0x10},{"W",0x11},{"E",0x12},{"R",0x13},{"T",0x14},{"Y",0x15},{"U",0x16},
    {"I",0x17},{"O",0x18},{"P",0x19},{"[",0x1A},{"]",0x1B},{"Return",0x1C},
    {"Left Ctrl",0x1D},
    {"A",0x1E},{"S",0x1F},{"D",0x20},{"F",0x21},{"G",0x22},{"H",0x23},{"J",0x24},
    {"K",0x25},{"L",0x26},{";",0x27},{"'",0x28},{"`",0x29},
    {"Left Shift",0x2A},{"\\",0x2B},
    {"Z",0x2C},{"X",0x2D},{"C",0x2E},{"V",0x2F},{"B",0x30},{"N",0x31},{"M",0x32},
    {",",0x33},{".",0x34},{"/",0x35},{"Right Shift",0x36},
    {"Left Alt",0x38},{"Space",0x39},
    {"F1",0x3B},{"F2",0x3C},{"F3",0x3D},{"F4",0x3E},{"F5",0x3F},
    {"F6",0x40},{"F7",0x41},{"F8",0x42},{"F9",0x43},{"F10",0x44},
    {"Up",0x48},{"Left",0x4B},{"Right",0x4D},{"Down",0x50},
};
#define XT_KEYMAP_N ((int)(sizeof(XT_KEYMAP) / sizeof(XT_KEYMAP[0])))

static void scan_keyboard(Ibm5150State *s) {
    static bool prev[XT_KEYMAP_N];
    for (int i = 0; i < XT_KEYMAP_N; i++) {
        bool down = gemu_display_is_key_held(s->display, XT_KEYMAP[i].name);
        if (down != prev[i]) {
            prev[i] = down;
            uint8_t code = XT_KEYMAP[i].code;
            if (!down) code = (uint8_t)(code | 0x80);
            if (s->kbq_len < (int)(sizeof(s->kbq) / sizeof(s->kbq[0])))
                s->kbq[s->kbq_len++] = code;
        }
    }
    if (!s->ppi.kb_data_ready && s->kbq_len > 0) {
        i8255_kb_scancode(&s->ppi, s->kbq[0]);
        memmove(s->kbq, s->kbq + 1, (size_t)(s->kbq_len - 1) * sizeof(s->kbq[0]));
        s->kbq_len--;
        i8259_raise_irq(&s->pic, 1);
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Ibm5150State *ibm5150_create(const X86Config *cfg) {
    Ibm5150State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->ram_size = cfg->ram_size ? cfg->ram_size : IBM5150_RAM_MAX;
    if (s->ram_size > IBM5150_RAM_MAX) s->ram_size = IBM5150_RAM_MAX;
    s->ram = calloc(1, s->ram_size);
    if (!s->ram) { free(s); return NULL; }

    bool have_bios = false;
    for (int i = 0; i < cfg->n_roms; i++) {
        const X86RomLoad *r = &cfg->roms[i];
        FILE *f = fopen(r->path, "rb");
        if (!f) { fprintf(stderr, "gemu: cannot open '%s'\n", r->path); continue; }
        if (r->region && strcmp(r->region, "basic") == 0) {
            s->has_basic = fread(s->basic, 1, IBM5150_BASIC_SIZE, f) > 0;
        } else {
            have_bios = fread(s->bios, 1, IBM5150_BIOS_SIZE, f) > 0 || have_bios;
        }
        fclose(f);
    }
    if (!have_bios) {
        fprintf(stderr, "gemu: ibm5150 needs a BIOS ROM (-rom roms/ibm5150 to scan)\n");
        free(s->ram); free(s);
        return NULL;
    }

    s->cpu.type = cfg->cpu;
    s->cpu.mem_read8 = bus_read8;
    s->cpu.mem_write8 = bus_write8;
    s->cpu.io_read8 = io_read8;
    s->cpu.io_write8 = io_write8;
    s->cpu.bus_ud = s;
    s->cpu.on_unknown_opcode = on_unknown_opcode;
    x86_reset(&s->cpu);

    i8259_reset(&s->pic);
    i8253_reset(&s->pit);
    i8255_reset(&s->ppi);
    /* SW1 DIP switches: floppy present, no 8087, 64K motherboard bank
     * (bits 2-3, meaningless beyond the base board but BIOS still reads
     * it), 80x25 color CGA (bits 4-5 = 10), 1 floppy drive (bits 6-7 = 00). */
    s->ppi.sw1 = 0x2F;
    cga_reset(&s->cga);

    s->fb = calloc((size_t)640 * 200, sizeof(uint32_t));
    if (!s->fb) { free(s->ram); free(s); return NULL; }

    s->monitor = gemu_monitor_create();

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = 640, .fb_height = 200,
            .scale = cfg->display_scale > 0 ? cfg->display_scale : 1,
            .no_menu = true, /* full keyboard machine - no gamepad-style input menu */
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
    }

    return s;
}

void ibm5150_destroy(Ibm5150State *s) {
    if (!s) return;
    if (s->display) gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    free(s->fb);
    free(s->ram);
    free(s);
}

void ibm5150_run(Ibm5150State *s, const X86Config *cfg) {
    (void)cfg;
    printf("gemu-x86: IBM PC 5150\n"
           "  RAM   : %u KiB\n"
           "  Video : CGA (text mode)\n"
           "  BASIC : %s\n",
           s->ram_size >> 10, s->has_basic ? "Cassette BASIC present" : "(none loaded)");

    gemu_monitor_start(s->monitor);

    bool running = true;
    bool was_halted = false;
    while (running) {
        if (!gemu_monitor_is_paused(s->monitor)) {
            for (int i = 0; i < IBM5150_INSTR_PER_FRAME && !s->cpu.halted; i++)
                x86_step(&s->cpu);
        }
        if (s->cpu.halted && !was_halted && !s->halted_for_debug) {
            fprintf(stderr, "ibm5150: HLT at %04x:%04x (ninsts=%llu)\n",
                    s->cpu.cs, (uint16_t)(s->cpu.ip - 1), (unsigned long long)s->cpu.insn_count);
        }
        was_halted = s->cpu.halted;

        /* PIT/CGA are ticked by a fixed per-loop-iteration amount rather
         * than wall-clock time or actual cycles consumed - simplest thing
         * that still lets a HLT-based BIOS idle loop get woken by IRQ0.
         * Real-time pacing (see i2000's merced_set_external_itc) is a good
         * follow-up once timing accuracy actually matters. */
        if (i8253_tick(&s->pit, IBM5150_PIT_TICKS_PER_FRAME))
            i8259_raise_irq(&s->pic, 0);
        i8255_set_timer2_out(&s->ppi, i8253_ch2_out(&s->pit));
        cga_tick(&s->cga, IBM5150_PIT_TICKS_PER_FRAME);

        if (s->display) scan_keyboard(s);

        int vec = i8259_pending_vector(&s->pic);
        if (vec >= 0) x86_request_irq(&s->cpu, (uint8_t)vec);

        if (s->display) {
            gemu_display_poll(s->display);
            cga_render(&s->cga, s->fb, 640, 200, cgafont8x8);
            gemu_display_render(s->display, s->fb, 640, 200);
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor) || s->cpu.halted);
            if (gemu_display_should_quit(s->display)) running = false;
            if (gemu_display_reset_requested(s->display)) {
                x86_reset(&s->cpu);
                gemu_display_clear_flags(s->display);
            }
            gemu_sleep_ms(s->cpu.halted ? 30 : 1);
        }
        /* headless (-display none): deliberately no per-frame sleep here -
         * see machine_i2000.c's Sleep(0) history before adding one. */

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT: running = false; break;
            case GEMU_MON_RESET: x86_reset(&s->cpu); break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                for (uint32_t k = 0; k < n && !s->cpu.halted; k++) x86_step(&s->cpu);
                break;
            }
            default: break;
            }
        }
    }
    gemu_monitor_stop(s->monitor);
}
