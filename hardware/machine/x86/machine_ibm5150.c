#include "ibm5150.h"
#include "controller/i8237_dev.h"
#include "controller/i8255_dev.h"
#include "controller/i8259_dev.h"
#include "serial/ins8250_dev.h"
#include "storage/ibm_hdc_dev.h"
#include "storage/upd765_dev.h"
#include "timer/i8253_dev.h"
#include "pc_video_dev.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include "audio/pc_speaker.h"
#include "cgafont8x8.h"
#include "mdafont8x14.h"
#include "gemu/util.h"
#include "gemu/screendump.h"
#include "gemu/sha256.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define IBM5150_BASIC_BASE 0xF6000u
#define IBM5150_BIOS_BASE  0xFE000u
#define IBM5150_CGA_BASE   0xB8000u
#define IBM5150_CGA_END    0xBC000u /* B8000-BBFFF, 16K */
#define IBM5150_MDA_BASE   0xB0000u
#define IBM5150_MDA_END    0xB8000u /* 4K, mirrored through B7FFF */

#define PIT_HZ 1193182u

/* The CGA's 640x200 picture with doubled scanlines (see cga_render()). */
#define IBM5150_CGA_FB_W 640
#define IBM5150_CGA_FB_H 400

/* ── Display adapters (-vga cga | mda, plus -device for the other) ────────
 *
 * The CGA and MDA decode different memory and ports, so a PC can have one of
 * each, each driving its own monitor (the BIOS starts on the one the DIP
 * switches name; DOS's MODE MONO / MODE CO80 switch between them). */

/* VRAM windows: MDA B0000-B7FFF (4K mirrored), CGA B8000-BBFFF, Hercules
 * B0000-B7FFF and, when configured for it, B8000-BFFFF.  A Hercules set to
 * its second page beside a CGA has both cards answering at B8000: the
 * writes reach both, and the CGA's reads are what comes back. */
static bool herc_mem(const Ibm5150State *s, uint32_t addr) {
    return s->has_herc && addr >= IBM5150_MDA_BASE &&
           hercules_mem_decoded(s->herc, addr - IBM5150_MDA_BASE);
}

static bool video_mem_read(const Ibm5150State *s, uint32_t addr, uint8_t *val) {
    if (herc_mem(s, addr) && !(s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END)) {
        *val = hercules_mem_read(s->herc, addr - IBM5150_MDA_BASE);
        return true;
    }
    if (s->has_mda && addr >= IBM5150_MDA_BASE && addr < IBM5150_MDA_END) {
        *val = mda_mem_read(s->mda, addr - IBM5150_MDA_BASE);
        return true;
    }
    if (s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END) {
        *val = cga_mem_read(s->cga, addr - IBM5150_CGA_BASE);
        return true;
    }
    return false;
}

static bool video_mem_write(Ibm5150State *s, uint32_t addr, uint8_t val) {
    if (herc_mem(s, addr)) {
        hercules_mem_write(s->herc, addr - IBM5150_MDA_BASE, val);
        if (!(s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END)) return true;
    }
    if (s->has_mda && addr >= IBM5150_MDA_BASE && addr < IBM5150_MDA_END) {
        mda_mem_write(s->mda, addr - IBM5150_MDA_BASE, val);
        return true;
    }
    if (s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END) {
        cga_mem_write(s->cga, addr - IBM5150_CGA_BASE, val);
        return true;
    }
    return false;
}

/* Both cards run; the primary's frames pace the machine. */
static bool video_tick(Ibm5150State *s, uint32_t clocks) {
    bool mda_frame = s->has_mda && gemu_mda_tick(s->dev_mda, clocks);
    bool herc_frame = s->has_herc && gemu_hercules_tick(s->dev_herc, clocks, IBM5150_CPU_HZ);
    bool cga_frame = s->has_cga && gemu_cga_tick(s->dev_cga, clocks);
    return s->vga == X86_VGA_MDA ? mda_frame : s->vga == X86_VGA_HERCULES ? herc_frame : cga_frame;
}

/* The screen on the monitor the window shows (View menu). */
static GemuScreen *shown_screen(const Ibm5150State *s) {
    int head = s->display ? gemu_display_head(s->display) : 0;
    return s->head_screen[head < s->n_heads ? head : 0];
}

/* Every monitor that's on screen: the main window's, and any detached into
 * a window of its own (View menu). */
static void video_present(Ibm5150State *s) {
    for (int i = 0; i < s->n_heads; i++) {
        if (!gemu_display_head_shown(s->display, i)) continue;
        const GemuBitmap *b = gemu_screen_render(s->head_screen[i]);
        gemu_display_render_head(s->display, i, b->pix, b->width, b->height);
    }
}

/* Keyboard timing, in CPU clocks: the keyboard answers a reset with 0xAA a
 * few milliseconds after its clock line is released.  Scan codes are spaced
 * 8 ms apart - a fast typist's pace - so the BIOS's 15-key buffer keeps up
 * even with input injected by sendkey. */
#define KB_RESET_HOLD   (IBM5150_CPU_HZ / 1000 * 5)   /* clock low >= 5 ms resets */
#define KB_RESET_REPLY  (IBM5150_CPU_HZ / 1000 * 2)
#define KB_BYTE_GAP     (IBM5150_CPU_HZ / 1000 * 8)

/* ── CPU <-> memory/IO bus ───────────────────────────────────────────────── */

static uint8_t bus_read8(uint32_t addr, void *ud) {
    Ibm5150State *s = ud;
    if (addr < s->ram_size) return s->ram[addr];
    uint8_t v;
    if (video_mem_read(s, addr, &v)) return v;
    if (s->has_basic && addr >= IBM5150_BASIC_BASE && addr < IBM5150_BASIC_BASE + IBM5150_BASIC_SIZE)
        return s->basic[addr - IBM5150_BASIC_BASE];
    if (addr >= IBM5150_BIOS_BASE && addr < IBM5150_BIOS_BASE + IBM5150_BIOS_SIZE)
        return s->bios[addr - IBM5150_BIOS_BASE];
    if (s->has_hdc && addr >= IBM5150_HDC_ROM_BASE && addr < IBM5150_HDC_ROM_BASE + s->hdc_rom_mapped)
        return s->hdc_rom[addr - IBM5150_HDC_ROM_BASE];
    return 0xFF;
}
static void bus_write8(uint32_t addr, uint8_t val, void *ud) {
    Ibm5150State *s = ud;
    if (addr < s->ram_size) { s->ram[addr] = val; return; }
    if (video_mem_write(s, addr, val)) return;
    /* ROM regions: writes silently ignored, matching real hardware */
}

/* Monitor memory view (20-bit physical): RAM, video RAM and the ROMs are
 * plain arrays; unmapped space returns false instead of the bus's 0xFF. */
static bool ibm5150_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    const Ibm5150State *s = ud;
    if (addr < s->ram_size) { *val = s->ram[addr]; return true; }
    if (addr <= 0xFFFFF && video_mem_read(s, (uint32_t)addr, val))
        return true;
    if (s->has_basic && addr >= IBM5150_BASIC_BASE &&
        addr < IBM5150_BASIC_BASE + IBM5150_BASIC_SIZE) {
        *val = s->basic[addr - IBM5150_BASIC_BASE];
        return true;
    }
    if (addr >= IBM5150_BIOS_BASE && addr < IBM5150_BIOS_BASE + IBM5150_BIOS_SIZE) {
        *val = s->bios[addr - IBM5150_BIOS_BASE];
        return true;
    }
    if (s->has_hdc && addr >= IBM5150_HDC_ROM_BASE && addr < IBM5150_HDC_ROM_BASE + s->hdc_rom_mapped) {
        *val = s->hdc_rom[addr - IBM5150_HDC_ROM_BASE];
        return true;
    }
    return false;
}

static bool ibm5150_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Ibm5150State *s = ud;
    if (addr < s->ram_size) { s->ram[addr] = val; return true; }
    if (addr <= 0xFFFFF && video_mem_write(s, (uint32_t)addr, val))
        return true;
    return false;   /* ROM / unmapped */
}

static void ibm5150_mon_cpu_state(void *ud, char *buf, size_t len) {
    const Ibm5150State *s = ud;
    x86_format_state(&s->cpu, buf, len);
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

/* ── Speaker and sound ───────────────────────────────────────────────────── *
 *
 * The speaker is a device (board/speaker, a pc-speaker): PIT channel 2's
 * output and PPI port B bit 1 drive it, and its stream goes through the
 * machine's mixer to the audio output - in emulated time, edge by edge. */

#define IBM5150_AUDIO_HZ 48000
/* bytes of float samples to keep queued: 50 ms */
#define IBM5150_AUDIO_QUEUE_MAX (IBM5150_AUDIO_HZ / 20 * sizeof(float))

static void audio_sink(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */

static bool kb_enabled(const Ibm5150State *s) {
    return !(s->ppi->port_b & 0x80) && (s->ppi->port_b & 0x40);
}

static void kb_update_irq(Ibm5150State *s) {
    gemu_line_set(s->kb_irq, s->kb_full && !(s->ppi->port_b & 0x80));
}

/* Shift the next byte into the PPI if the keyboard may send one. */
static void kb_service(Ibm5150State *s) {
    if (s->kb_full || !kb_enabled(s) || s->cycles < s->kb_next_at) return;
    if (s->kb_reset_at) {
        if (s->cycles < s->kb_reset_at) return;
        s->kb_reset_at = 0;
        s->ppi->kb_data = 0xAA;
    } else if (s->kbq_len > 0) {
        s->ppi->kb_data = s->kbq[0];
        memmove(s->kbq, s->kbq + 1, (size_t)(s->kbq_len - 1));
        s->kbq_len--;
    } else {
        return;
    }
    s->kb_full = true;
    s->kb_next_at = s->cycles + KB_BYTE_GAP;
    kb_update_irq(s);
}

static void kb_port_b_changed(Ibm5150State *s, uint8_t old, uint8_t val) {
    if ((old & 0x40) && !(val & 0x40))           /* clock pulled low */
        s->kb_clock_low_at = s->cycles;
    if (!(old & 0x40) && (val & 0x40) &&         /* clock released after a reset hold */
        s->cycles - s->kb_clock_low_at >= KB_RESET_HOLD) {
        s->kbq_len = 0;
        s->kb_reset_at = s->cycles + KB_RESET_REPLY;
    }
    if (val & 0x80) {                            /* clear: empty the shift register */
        s->kb_full = false;
        s->ppi->kb_data = 0;
    }
    kb_update_irq(s);
}

/* ── I/O ports ───────────────────────────────────────────────────────────── *
 *
 * The PC decodes A0-A9 only: the "io" space mirrors 0-3FFh across all 64K.
 * ibm5150_io_map() lists what this configuration has there, in priority
 * order (the first range to claim a port for reads, or for writes, gets
 * it); io_build() maps each range at a priority that keeps that order.
 * The chips' ranges use their devices' handlers; the display adapters,
 * the NMI mask and the empty slots are the machine's.  Ports nobody claims
 * read $FF (the bus floats high) and are logged once (iolog). */

#define MS(ud) ((Ibm5150State *)(ud))
static void    nmi_w(void *ud, uint64_t o, uint8_t v)  { (void)o; MS(ud)->nmi_mask = v; }
/* Standard adapters this machine doesn't have: POST probes them all, so
 * the accesses are expected and not worth logging; the bus floats high. */
static uint8_t empty_r(void *ud, uint64_t o)           { (void)ud; (void)o; return 0xFF; }
static void    empty_w(void *ud, uint64_t o, uint8_t v) { (void)ud; (void)o; (void)v; }
static bool    empty_peek(void *ud, uint64_t o, uint8_t *v) { (void)ud; (void)o; *v = 0xFF; return true; }

static const GemuHandlers nmi_io   = { .w8 = nmi_w };
static const GemuHandlers empty_io = { .r8 = empty_r, .w8 = empty_w, .peek8 = empty_peek };

/* What follows a write on the board, stacked (shared) after the chip's own
 * handler so it sees the chip already updated: the keyboard clock/clear
 * after port B.  (PB0's gate for PIT channel 2 and PB1's speaker data are
 * lines.) */
static void port_b_written(void *ud, uint64_t o, uint8_t v) {
    Ibm5150State *s = ud;
    (void)o;
    uint8_t old = s->port_b;
    s->port_b = v;
    kb_port_b_changed(s, old, v);
}
static const GemuHandlers ppib_after = { .w8 = port_b_written };

static void io_unmapped_read(void *ud, uint64_t addr, unsigned n) {
    (void)n;
    iolog(ud, (uint16_t)(addr & 0x3FF), false, 0);
}
static void io_unmapped_write(void *ud, uint64_t addr, unsigned n, uint64_t v) {
    (void)n;
    iolog(ud, (uint16_t)(addr & 0x3FF), true, (uint8_t)v);
}

typedef struct {
    uint16_t            lo, hi;
    const GemuHandlers *h;
    void               *ud;
    const char         *name;
} IoRange;

#define IO_MAX_RANGES 32

/* This configuration's ports, highest priority first. */
static int ibm5150_io_map(const Ibm5150State *s, IoRange *r) {
    int n = 0;
    void *m = (void *)s;
#define RANGE(l, h, hd, u, nm) (r[n++] = (IoRange){ (l), (h), (hd), (u), (nm) })
    RANGE(0x000, 0x00F, &gemu_i8237_io,      s->dev_dma, "8237 DMA");
    RANGE(0x020, 0x021, &gemu_i8259_io,      s->dev_pic, "8259 PIC");
    RANGE(0x040, 0x043, &gemu_i8253_io,      s->dev_pit, "8253 PIT");
    RANGE(0x060, 0x063, &gemu_i8255_io,      s->dev_ppi, "8255 PPI");
    RANGE(0x080, 0x083, &gemu_i8237_page_io, s->dev_dma, "DMA page registers");
    RANGE(0x0A0, 0x0AF, &nmi_io,             m,          "NMI mask");
    /* MDA 3B0-3BB (3BC- is its printer port); Hercules adds 3BF */
    if (s->has_mda)    RANGE(0x3B0, 0x3BB, &gemu_mda_io,          s->dev_mda,  "MDA");
    if (s->has_herc) { RANGE(0x3B0, 0x3BB, &gemu_hercules_io,     s->dev_herc, "Hercules");
                       RANGE(0x3BF, 0x3BF, &gemu_hercules_cfg_io, s->dev_herc, "Hercules configuration"); }
    if (s->has_cga)    RANGE(0x3D0, 0x3DF, &gemu_cga_io,          s->dev_cga,  "CGA");
    if (s->has_fdc)    RANGE(0x3F0, 0x3F7, &gemu_upd765_io,  s->dev_fdc, "diskette adapter (765)");
    if (s->has_hdc)    RANGE(0x320, 0x323, &gemu_ibm_hdc_io, s->dev_hdc, "fixed disk adapter");
    if (s->has_com[0]) RANGE(0x3F8, 0x3FF, &gemu_ins8250_io, s->dev_com[0], "COM1 (8250)");
    if (s->has_com[1]) RANGE(0x2F8, 0x2FF, &gemu_ins8250_io, s->dev_com[1], "COM2 (8250)");
    RANGE(0x200, 0x20F, &empty_io, m, "game port (empty)");
    RANGE(0x210, 0x217, &empty_io, m, "expansion unit (empty)");
    RANGE(0x320, 0x32F, &empty_io, m, "fixed disk adapters (empty)");
    RANGE(0x278, 0x27F, &empty_io, m, "LPT2 (empty)");
    RANGE(0x2F8, 0x2FF, &empty_io, m, "COM2 (empty)");
    RANGE(0x378, 0x37F, &empty_io, m, "LPT1 (empty)");
    RANGE(0x3B0, 0x3BF, &empty_io, m, "MDA and its printer port (empty)");
    RANGE(0x3D0, 0x3DF, &empty_io, m, "CGA (empty)");
    RANGE(0x3F0, 0x3F7, &empty_io, m, "diskette adapter (empty)");
    RANGE(0x3F8, 0x3FF, &empty_io, m, "COM1 (empty)");
#undef RANGE
    return n;
}

static bool reads(const GemuHandlers *h)  { return h->r8 != NULL; }
static bool writes(const GemuHandlers *h) { return h->w8 != NULL; }

/* Map every range: earlier ranges get higher priorities, so they win. */
static bool io_build(Ibm5150State *s) {
    IoRange r[IO_MAX_RANGES];
    int n = ibm5150_io_map(s, r);
    for (int i = 0; i < n; i++) {
        GemuMapping *mp = gemu_space_map(s->io, r[i].lo, (uint64_t)(r[i].hi - r[i].lo + 1),
                                         r[i].h, r[i].ud, n - i, 0);
        if (!mp) return false;
        gemu_mapping_set_name(mp, r[i].name);
        /* the board's reaction, at the chip's priority, after it */
        if (r[i].h == &gemu_i8255_io) {
            GemuMapping *after = gemu_space_map(s->io, 0x061, 1, &ppib_after, s, n - i,
                                                GEMU_MAP_SHARED);
            if (!after) return false;
            gemu_mapping_set_name(after, "keyboard (after port B)");
        }
    }
    gemu_space_set_unmapped(s->io, 0xFF, io_unmapped_read, io_unmapped_write, s);
    return gemu_space_mirror(s->io, 0, 0x10000, 0x3FF);
}

static uint8_t io_read8(uint16_t port, void *ud) {
    return gemu_space_read8(((Ibm5150State *)ud)->io, port);
}

static void io_write8(uint16_t port, uint8_t val, void *ud) {
    gemu_space_write8(((Ibm5150State *)ud)->io, port, val);
}

/* The range that gets port p's reads (or writes): the first that has it. */
static int io_owner(const IoRange *r, int n, unsigned p, bool write) {
    for (int i = 0; i < n; i++)
        if (p >= r[i].lo && p <= r[i].hi && (write ? writes(r[i].h) : reads(r[i].h)))
            return i;
    return -1;
}

/* ioports: the port map, as the CPU sees it now - each range's ports that
 * it actually owns (earlier ranges shadow later ones), in port order. */
static GemuMonCmd ibm5150_cmd_ioports(GemuMonitor *mon, const char *args, void *ud) {
    const Ibm5150State *s = ud;
    (void)args;
    IoRange r[IO_MAX_RANGES];
    int n = ibm5150_io_map(s, r);
    struct { uint16_t lo, hi; int range; } span[0x400];
    int n_span = 0;
    for (int i = 0; i < n; i++) {
        int start = -1;
        for (unsigned p = r[i].lo; p <= (unsigned)r[i].hi + 1u; p++) {
            bool owned = p <= r[i].hi &&
                         ((reads(r[i].h) && io_owner(r, n, p, false) == i) ||
                          (writes(r[i].h) && io_owner(r, n, p, true) == i));
            if (owned && start < 0) start = (int)p;
            if (!owned && start >= 0) {
                span[n_span++] = (typeof(span[0])){ (uint16_t)start, (uint16_t)(p - 1u), i };
                start = -1;
            }
        }
    }
    for (int i = 1; i < n_span; i++)          /* insertion sort by port */
        for (int j = i; j > 0 && span[j - 1].lo > span[j].lo; j--) {
            typeof(span[0]) t = span[j]; span[j] = span[j - 1]; span[j - 1] = t;
        }
    for (int i = 0; i < n_span; i++) {
        const IoRange *q = &r[span[i].range];
        gemu_monitor_printf(mon, "  %03X-%03X  %s%s\n", span[i].lo, span[i].hi, q->name,
                            !reads(q->h) ? " (write only)" : "");
    }
    gemu_monitor_printf(mon, "  (anything else reads FF and is logged as unhandled)\n");
    return GEMU_MON_NONE;
}

/* ── Adapters' wiring ─────────────────────────────────────────────────────── *
 *
 * The diskette adapter is IRQ 6 / DMA 2, the fixed disk adapter IRQ 5 (or 2)
 * / DMA 3, COM1 IRQ 4 and COM2 IRQ 3: lines and DMA channels set up in
 * build_board().  Here only which adapters there are. */

static void attach_com(Ibm5150State *s, int n) {
    s->has_com[n] = true;
}

/* -device msmouse: the mouse and the serial adapter it plugs into (once
 * the adapter's device exists: see build_board()). */
static void attach_mouse(Ibm5150State *s, int com) {
    msmouse_init(&s->mouse, IBM5150_CPU_HZ);
    Ins8250 *u = s->com[com - 1];
    u->dev = &s->mouse;
    u->dev_lines = msmouse_lines;
    u->dev_rx = msmouse_rx;
    s->has_mouse = true;
}

/* QEMU's mouse_move DX DY [DZ] and mouse_button STATE (1 left, 2 middle,
 * 4 right). */
static GemuMonCmd ibm5150_cmd_mouse_move(GemuMonitor *mon, const char *args, void *ud) {
    Ibm5150State *s = ud;
    int dx, dy;
    if (!s->has_mouse) { gemu_monitor_printf(mon, "mouse_move: no mouse (-device msmouse)\n"); return GEMU_MON_NONE; }
    if (sscanf(args, "%d %d", &dx, &dy) != 2) {
        gemu_monitor_printf(mon, "usage: mouse_move dx dy\n");
        return GEMU_MON_NONE;
    }
    msmouse_input(&s->mouse, dx, dy, s->mouse_buttons & 1, s->mouse_buttons & 4);
    return GEMU_MON_NONE;
}

static GemuMonCmd ibm5150_cmd_mouse_button(GemuMonitor *mon, const char *args, void *ud) {
    Ibm5150State *s = ud;
    if (!s->has_mouse) { gemu_monitor_printf(mon, "mouse_button: no mouse (-device msmouse)\n"); return GEMU_MON_NONE; }
    s->mouse_buttons = (uint8_t)atoi(args);
    msmouse_input(&s->mouse, 0, 0, s->mouse_buttons & 1, s->mouse_buttons & 4);
    return GEMU_MON_NONE;
}

/* ── Interrupts and timers ───────────────────────────────────────────────── */

static bool cpu_intr_pending(void *ud) {
    Ibm5150State *s = ud;
    return i8259_intr(s->pic);
}

static uint8_t cpu_intr_ack(void *ud) {
    Ibm5150State *s = ud;
    return gemu_i8259_ack(s->dev_pic);
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
     * progress happened so far inspectable via the monitor. */
    s->halted_for_debug = true;
    cpu->halted = true;
}

/* Advance the rest of the machine by the clocks the CPU just used.
 * Returns true when the display adapter started a new frame. */
static bool advance(Ibm5150State *s, unsigned clocks) {
    s->cycles += clocks;
    s->pit_phase += clocks;
    if (s->pit_phase >= 4) {
        i8253_tick(s->pit, s->pit_phase / 4);
        s->pit_phase %= 4;
    }
    kb_service(s);
    if (s->has_fdc) upd765_tick(s->fdc, clocks);
    if (s->has_hdc) ibm_hdc_tick(s->hdc, clocks);
    if (s->has_mouse) msmouse_tick(&s->mouse, clocks);
    for (int i = 0; i < 2; i++)
        if (s->has_com[i]) ins8250_tick(s->com[i], clocks);
    return video_tick(s, clocks);
}

/* ── The 8088 on the scheduler ────────────────────────────────────────────── *
 *
 * The CPU is the machine's executor: the scheduler gives it a budget of
 * CPU clocks, it runs instructions and clocks the board's chips after each
 * one (advance()), as the machine always has.  A run also ends at the
 * instruction where the display adapter starts a frame (the front end then
 * shows it), at an exec breakpoint, or when a monitor "step N" has run its
 * N instructions - each by stopping the machine, which leaves its time
 * exactly where the CPU got. */

static uint64_t cpu_run(void *ud, uint64_t budget) {
    Ibm5150State *s = ud;
    (void)budget;                      /* gemu_executor_budget() can shrink mid-run */
    s->run_done = 0;
    while (s->run_done < gemu_executor_budget(s->cpu_exec)) {
        if (s->steps_left) {           /* stepping: no breakpoints, frames don't stop it */
            unsigned c = x86_step(&s->cpu);
            s->run_done += c;
            advance(s, c);
            if (--s->steps_left == 0) gemu_machine_stop(s->machine);
            continue;
        }
        if (s->check_bp && !s->cpu.halted &&
            gemu_monitor_check_exec(s->monitor, x86_linear(s->cpu.cs, s->cpu.ip))) {
            gemu_machine_stop(s->machine);
            break;
        }
        unsigned c = x86_step(&s->cpu);
        s->run_done += c;
        if (advance(s, c)) gemu_machine_stop(s->machine);   /* a new frame */
    }
    return s->run_done;
}

static uint64_t cpu_elapsed(void *ud) { return ((Ibm5150State *)ud)->run_done; }

static const GemuExecutorOps cpu_ops = { cpu_run, cpu_elapsed };

/* Run to the next frame (or breakpoint). */
static void run_frame(Ibm5150State *s) {
    s->check_bp = gemu_monitor_has_exec_breakpoints(s->monitor);
    gemu_machine_run_until(s->machine, gemu_machine_time(s->machine) + GEMU_SEC(1));
    gemu_mixer_run(s->machine);
}

/* Exactly n instructions. */
static void run_steps(Ibm5150State *s, uint64_t n) {
    s->steps_left = n;
    while (s->steps_left) {   /* mixing as it goes, as frames would (wavcapture) */
        gemu_machine_run_until(s->machine, gemu_machine_time(s->machine) + GEMU_MSEC(20));
        gemu_mixer_run(s->machine);
    }
}

/* ── Keyboard: XT scan code set 1, polled via gemu_display_is_key_held()
 * (the same full-keyboard pattern machine_pecom.c uses) since a PC
 * keyboard has far more keys than the 32-bit action-bitmask model fits. ── */

typedef struct { const char *name; uint8_t code; } XtKeyEnt;
static const XtKeyEnt XT_KEYMAP[] = {
    {"Escape",0x01},{"1",0x02},{"2",0x03},{"3",0x04},{"4",0x05},{"5",0x06},
    {"6",0x07},{"7",0x08},{"8",0x09},{"9",0x0A},{"0",0x0B},{"-",0x0C},{"=",0x0D},
    {"Backspace",0x0E},{"Tab",0x0F},
    {"Q",0x10},{"W",0x11},{"E",0x12},{"R",0x13},{"T",0x14},{"Y",0x15},{"U",0x16},
    {"I",0x17},{"O",0x18},{"P",0x19},{"[",0x1A},{"]",0x1B},{"Return",0x1C},
    {"Left Ctrl",0x1D},{"Right Ctrl",0x1D},
    {"A",0x1E},{"S",0x1F},{"D",0x20},{"F",0x21},{"G",0x22},{"H",0x23},{"J",0x24},
    {"K",0x25},{"L",0x26},{";",0x27},{"'",0x28},{"`",0x29},
    {"Left Shift",0x2A},{"\\",0x2B},
    {"Z",0x2C},{"X",0x2D},{"C",0x2E},{"V",0x2F},{"B",0x30},{"N",0x31},{"M",0x32},
    {",",0x33},{".",0x34},{"/",0x35},{"Right Shift",0x36},{"Keypad *",0x37},
    {"Left Alt",0x38},{"Right Alt",0x38},{"Space",0x39},{"CapsLock",0x3A},
    {"F1",0x3B},{"F2",0x3C},{"F3",0x3D},{"F4",0x3E},{"F5",0x3F},
    {"F6",0x40},{"F7",0x41},{"F8",0x42},{"F9",0x43},{"F10",0x44},
    {"Numlock",0x45},{"ScrollLock",0x46},
    {"Home",0x47},{"Up",0x48},{"PageUp",0x49},{"Keypad -",0x4A},
    {"Left",0x4B},{"Keypad 5",0x4C},{"Right",0x4D},{"Keypad +",0x4E},
    {"End",0x4F},{"Down",0x50},{"PageDown",0x51},{"Insert",0x52},{"Delete",0x53},
};
#define XT_KEYMAP_N ((int)(sizeof(XT_KEYMAP) / sizeof(XT_KEYMAP[0])))

static void scan_keyboard(Ibm5150State *s) {
    bool now[128] = {0};
    for (int i = 0; i < XT_KEYMAP_N; i++)
        if (gemu_display_is_key_held(s->display, XT_KEYMAP[i].name))
            now[XT_KEYMAP[i].code] = true;
    /* Keys that changed together in one frame (common with VNC, which
     * delivers Shift and the letter back to back) are sent in the order a
     * typist produces them: releases before presses, and modifiers inside
     * ordinary keys - otherwise Shift+P can arrive as "p", or the key after
     * a shifted one gets shifted too. */
    static const uint8_t modifiers[] = { 0x1D, 0x2A, 0x36, 0x38 };
    for (int pass = 0; pass < 4; pass++) {
        bool want_mod  = pass == 1 || pass == 2;   /* 0: key up, 1: mod up, */
        bool want_down = pass >= 2;                /* 2: mod down, 3: key down */
        for (int code = 1; code < 128; code++) {
            if (now[code] == s->key_down[code]) continue;
            bool mod = memchr(modifiers, code, sizeof(modifiers)) != NULL;
            if (mod != want_mod || now[code] != want_down)
                continue;
            s->key_down[code] = now[code];
            if (s->kbq_len < (int)sizeof(s->kbq))
                s->kbq[s->kbq_len++] = (uint8_t)(now[code] ? code : (code | 0x80));
        }
    }
}

/* ── Monitor commands ────────────────────────────────────────────────────── */

static bool ibm5150_screendump(void *ud, const char *path) {
    Ibm5150State *s = ud;
    const GemuBitmap *b = gemu_screen_render(shown_screen(s));
    return gemu_screendump_argb(path, b->pix, b->width, b->height);
}

static void kb_queue(Ibm5150State *s, uint8_t code) {
    if (s->kbq_len < (int)sizeof(s->kbq)) s->kbq[s->kbq_len++] = code;
}

/* Named keys for sendkey, using QEMU's names where it has them. */
static const XtKeyEnt SENDKEY_NAMES[] = {
    {"esc",0x01},{"backspace",0x0E},{"tab",0x0F},{"ret",0x1C},{"enter",0x1C},
    {"ctrl",0x1D},{"shift",0x2A},{"shift_r",0x36},{"alt",0x38},{"spc",0x39},
    {"space",0x39},{"caps_lock",0x3A},{"num_lock",0x45},{"scroll_lock",0x46},
    {"f1",0x3B},{"f2",0x3C},{"f3",0x3D},{"f4",0x3E},{"f5",0x3F},{"f6",0x40},
    {"f7",0x41},{"f8",0x42},{"f9",0x43},{"f10",0x44},{"home",0x47},{"up",0x48},
    {"pgup",0x49},{"left",0x4B},{"right",0x4D},{"end",0x4F},{"down",0x50},
    {"pgdn",0x51},{"insert",0x52},{"delete",0x53},{"minus",0x0C},{"equal",0x0D},
};

/* US layout: scan code for an ASCII character, and whether it needs shift. */
static bool ascii_to_scan(char ch, uint8_t *code, bool *shift) {
    static const char unshifted[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0"
                                    "asdfghjkl;'`\0\\" "zxcvbnm,./";
    static const char shifted[]   = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0"
                                    "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";
    if (ch == ' ') { *code = 0x39; *shift = false; return true; }
    for (unsigned i = 0; i < sizeof(unshifted) - 1; i++) {
        if (unshifted[i] && unshifted[i] == ch) { *code = (uint8_t)i; *shift = false; return true; }
        if (shifted[i]   && shifted[i]   == ch) { *code = (uint8_t)i; *shift = true;  return true; }
    }
    return false;
}

static bool sendkey_name(const char *name, uint8_t *code) {
    for (size_t i = 0; i < sizeof(SENDKEY_NAMES) / sizeof(SENDKEY_NAMES[0]); i++)
        if (strcasecmp(name, SENDKEY_NAMES[i].name) == 0) { *code = SENDKEY_NAMES[i].code; return true; }
    bool shift;
    return name[0] && !name[1] && ascii_to_scan((char)tolower((unsigned char)name[0]), code, &shift);
}

/* "ctrl-alt-delete": press each key in order, release in reverse.  Returns
 * false (queueing nothing) unless every part is a key name. */
static bool sendkey_combo(Ibm5150State *s, const char *tok) {
    uint8_t codes[8];
    int n = 0;
    for (const char *p = tok; *p; ) {
        const char *dash = strchr(p, '-');
        size_t len = dash ? (size_t)(dash - p) : strlen(p);
        char part[32];
        if (n == 8 || len == 0 || len >= sizeof(part)) return false;
        memcpy(part, p, len);
        part[len] = '\0';
        if (!sendkey_name(part, &codes[n])) return false;
        n++;
        p += len + (dash ? 1 : 0);
    }
    if (n == 0) return false;
    for (int i = 0; i < n; i++) kb_queue(s, codes[i]);
    for (int i = n - 1; i >= 0; i--) kb_queue(s, (uint8_t)(codes[i] | 0x80));
    return true;
}

static GemuMonCmd ibm5150_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    Ibm5150State *s = ud;
    if (!*args) {
        gemu_monitor_printf(mon, "usage: sendkey <keys|text>...  e.g. sendkey ctrl-alt-delete, "
                                 "sendkey PRINT spc 2+2 ret\n");
        return GEMU_MON_NONE;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", args);
    for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (strlen(tok) > 1 && sendkey_combo(s, tok))   /* named key or combo */
            continue;
        for (const char *p = tok; *p; p++) {       /* literal text */
            uint8_t code; bool shift;
            if (!ascii_to_scan(*p, &code, &shift)) {
                gemu_monitor_printf(mon, "sendkey: can't type '%c'\n", *p);
                return GEMU_MON_NONE;
            }
            if (shift) kb_queue(s, 0x2A);
            kb_queue(s, code);
            kb_queue(s, (uint8_t)(code | 0x80));
            if (shift) kb_queue(s, 0xAA);
        }
    }
    return GEMU_MON_NONE;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* DIP switches for the configured RAM.  SW1 bits 2-3 give the planar
 * (motherboard) RAM in 16K steps up to 64K; SW2 counts 32K expansion
 * blocks above that, which the 19OCT81 BIOS reads as a 4-bit count - so
 * 64K + 15 x 32K = 544K is the most it can use.  Bits 4-5 give the
 * display: 80x25 color (CGA) or monochrome (MDA).  Diskette drives (bit 0, count in bits 6-7) are
 * added by attach_floppies(); without them POST boots Cassette BASIC. */
static uint32_t configure_switches(Ibm5150State *s, uint32_t ram) {
    uint32_t kb = ram >> 10;
    uint32_t planar = kb >= 64 ? 64 : (kb < 16 ? 16 : kb / 16 * 16);
    uint32_t blocks = kb > 64 ? (kb - 64) / 32 : 0;
    if (blocks > 15) blocks = 15;
    s->sw1 = (uint8_t)(((planar / 16 - 1) << 2) |
                       (x86_vga_mono(s->vga) ? 0x30 : 0x20));   /* mono / 80x25 color */
    s->sw2 = (uint8_t)blocks;
    return (planar + blocks * 32) << 10;
}

/* -M ibm5150,OPTS: DIP switch settings that override the ones worked out
 * from the configuration.  -1 = not given. */
typedef struct {
    int floppy;      /* SW1-1: diskette drives installed (0/1) */
    int floppies;    /* SW1-7/8: how many (1-4) */
    int display;     /* SW1-5/6: 0 none, 1 CGA 40x25, 2 CGA 80x25, 3 monochrome */
    long ram_kb;     /* SW1-3/4 + SW2: memory the switches claim */
} Switches;

static bool parse_switches(const char *opts, Switches *sw) {
    *sw = (Switches){ -1, -1, -1, -1 };
    if (!opts) return true;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", opts);
    for (char *o = strtok(buf, ","); o; o = strtok(NULL, ",")) {
        char *v = strchr(o, '=');
        if (v) *v++ = '\0';
        if (!v) {
            fprintf(stderr, "gemu: -M ibm5150,%s: expected KEY=VALUE\n", o);
            return false;
        }
        if (strcmp(o, "floppy") == 0 && (!strcmp(v, "on") || !strcmp(v, "off"))) {
            sw->floppy = strcmp(v, "on") == 0;
        } else if (strcmp(o, "floppies") == 0 && v[0] >= '1' && v[0] <= '4' && !v[1]) {
            sw->floppies = v[0] - '0';
        } else if (strcmp(o, "display") == 0) {
            static const char *const names[] = { "none", "cga40", "cga80", "mono" };
            for (int i = 0; i < 4; i++) if (strcmp(v, names[i]) == 0) sw->display = i;
            if (sw->display < 0) {
                fprintf(stderr, "gemu: -M ibm5150: display=none|cga40|cga80|mono\n");
                return false;
            }
        } else if (strcmp(o, "ram-switch") == 0) {
            char *end;
            sw->ram_kb = strtol(v, &end, 10);
            if (*end == 'K' || *end == 'k') end++;
            if (*end || sw->ram_kb < 16 || sw->ram_kb > 544) {
                fprintf(stderr, "gemu: -M ibm5150: ram-switch=16K-544K (what the switches can say)\n");
                return false;
            }
        } else if (strcmp(o, "fpu") == 0) {
            fprintf(stderr, "gemu: -M ibm5150: fpu= isn't available yet: GEMU doesn't emulate "
                            "the 8087, and software trusting the switch would crash\n");
            return false;
        } else {
            fprintf(stderr, "gemu: -M ibm5150: unknown switch '%s=%s' (floppy=on|off, "
                            "floppies=1-4, display=none|cga40|cga80|mono, ram-switch=SIZE)\n", o, v);
            return false;
        }
    }
    return true;
}

/* Floppy drives come from -drive if=floppy / -fda / -fdb, with as many
 * drives as the highest unit used (the 5150 supports four).  Without any,
 * drive A: is there and empty, as QEMU's PC has an empty floppy0, so a
 * diskette can be inserted later; -nodefaults leaves the diskette adapter
 * out, and POST goes straight to Cassette BASIC.  floppies=N adds empty
 * drives up to N. */
static void attach_floppies(Ibm5150State *s, int want) {
    int count = 0;
    for (int u = 0; u < 4; u++) {
        s->floppy[u] = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, u);
        /* floppies=N: the drives it counts are there, empty if not given */
        if (!s->floppy[u] && u < want) {
            char opts[32];
            snprintf(opts, sizeof(opts), "if=floppy,index=%d", u);
            if (gemu_drive_add(opts)) s->floppy[u] = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, u);
        }
        if (s->floppy[u]) count = u + 1;
    }
    if (!count && (s->floppy[0] = gemu_drive_add_default(GEMU_DRIVE_IF_FLOPPY, 0)))
        count = 1;
    if (!count) return;
    s->has_fdc = true;
    s->sw1 = (uint8_t)(s->sw1 | 0x01 | ((count - 1) << 6));
}

/* BIOSes from before 10/27/82 don't look for adapter ROMs, so the fixed
 * disk adapter's never runs under them. */
static void warn_old_bios(const Ibm5150State *s) {
    static const char *const no_rom_scan[] = {
        "a259306c47b43e14ef6378d7bdeff67331a2519d363bc36b3f354bc91a6874c8",  /* 24APR81 */
        "0932d30f91df12761e05bb0229508b4255329990e76264fdc8f1cdc3bf07c0e4",  /* 19OCT81 */
        "8aa2c24a4d342b77624cba34f4881ffee125e941900c45a186cf1c8af249cd1a",  /* 16AUG82 */
    };
    GemuSha256Ctx ctx;
    uint8_t digest[GEMU_SHA256_DIGEST_LEN];
    char hex[65];
    gemu_sha256_init(&ctx);
    gemu_sha256_update(&ctx, s->bios, IBM5150_BIOS_SIZE);
    gemu_sha256_final(&ctx, digest);
    gemu_sha256_hex(digest, hex);
    for (size_t i = 0; i < sizeof(no_rom_scan) / sizeof(no_rom_scan[0]); i++)
        if (strcmp(hex, no_rom_scan[i]) == 0)
            fprintf(stderr, "gemu: warning: this BIOS predates adapter ROM support, so the fixed "
                            "disk adapter's ROM won't run (it needs the 10/27/82 BIOS)\n");
}

/* The adapter's ROM holds the drive types its jumpers choose between: a
 * table of 16-byte entries (cylinders, heads, ...) that it points INT 41h
 * at with MOV WORD [0104h], table.  Returns the table's offset, or -1. */
static int hdc_drive_table(const Ibm5150State *s) {
    for (uint32_t i = 0; i + 6 <= s->hdc_rom_size; i++) {
        const uint8_t *p = &s->hdc_rom[i];
        if (p[0] == 0xC7 && p[1] == 0x06 && p[2] == 0x04 && p[3] == 0x01) {
            uint32_t t = p[4] | (uint32_t)p[5] << 8;
            if (t + 4 * 16 <= s->hdc_rom_size) return (int)t;
        }
    }
    return -1;
}

static uint64_t hdc_type_bytes(const uint8_t *e) {
    return (uint64_t)(e[0] | e[1] << 8) * e[2] * IBM_HDC_SPT * IBM_HDC_SECTOR;
}

/* -device ibm-fixed-disk[,romfile=FILE][,interleave=N][,irq=5|2][,rom=on|off]:
 * the card, its ROM
 * (romfile=, or the one the ROM scan found: 5000059 before 59X7291) and
 * the drives from -hda/-hdb, each one's type jumpered to the table entry
 * its image is the size of. */
static bool attach_fixed_disk(Ibm5150State *s, const X86Config *cfg, const char *scanned_rom) {
    const char *romfile = scanned_rom;
    unsigned interleave = 6;   /* how IBM formatted these drives */
    int want_type[2] = { -1, -1 };
    bool rom_on = true;
    s->hdc_irq = 5;
    char opts[512];
    snprintf(opts, sizeof(opts), "%s", cfg->hdc_opts ? cfg->hdc_opts : "");
    for (char *o = strtok(opts, ","); o; o = strtok(NULL, ",")) {
        if (strncmp(o, "romfile=", 8) == 0) romfile = o + 8;
        else if ((strncmp(o, "type0=", 6) == 0 || strncmp(o, "type1=", 6) == 0) &&
                 o[6] >= '0' && o[6] <= '3' && !o[7]) {
            want_type[o[4] - '0'] = o[6] - '0';
        } else if (strncmp(o, "interleave=", 11) == 0) {
            interleave = (unsigned)atoi(o + 11);
            if (interleave < 1 || interleave >= IBM_HDC_SPT) {
                fprintf(stderr, "gemu: ibm-fixed-disk: interleave must be 1-16\n");
                return false;
            }
        } else if (strcmp(o, "irq=5") == 0 || strcmp(o, "irq=2") == 0) {
            s->hdc_irq = (uint8_t)(o[4] - '0');
        } else if (strcmp(o, "rom=on") == 0 || strcmp(o, "rom=off") == 0) {
            rom_on = strcmp(o, "rom=on") == 0;
        } else {
            fprintf(stderr, "gemu: ibm-fixed-disk: unknown option '%s' (romfile=, type0=0-3, "
                            "type1=0-3, interleave=, irq=5|2, rom=on|off)\n", o);
            return false;
        }
    }
    if (!romfile) {
        fprintf(stderr, "gemu: ibm-fixed-disk needs its BIOS ROM (5000059 or 59X7291): "
                        "-rom roms/ibm5150 to scan, or romfile=FILE\n");
        return false;
    }
    FILE *f = fopen(romfile, "rb");
    if (!f) { fprintf(stderr, "gemu: ibm-fixed-disk: cannot open '%s'\n", romfile); return false; }
    s->hdc_rom_size = (uint32_t)fread(s->hdc_rom, 1, IBM5150_HDC_ROM_MAX, f);
    fclose(f);
    int table = hdc_drive_table(s);
    if (s->hdc_rom_size < 3 || s->hdc_rom[0] != 0x55 || s->hdc_rom[1] != 0xAA || table < 0) {
        fprintf(stderr, "gemu: ibm-fixed-disk: '%s' isn't a fixed disk adapter ROM\n", romfile);
        return false;
    }

    IbmHdc *h = s->hdc;
    for (int u = 0; u < 2; u++) {
        GemuDrive *d = gemu_drive_get(GEMU_DRIVE_IF_IDE, 0, u);
        if (!d) continue;
        if (gemu_drive_media(d) != GEMU_DRIVE_MEDIA_DISK) {
            fprintf(stderr, "gemu: ibm-fixed-disk: %s is a CD-ROM; the adapter takes hard disks\n",
                    gemu_drive_name(d));
            return false;
        }
        uint64_t size = gemu_drive_size(d);
        /* the first type the size fits, unless typeN= picked one (59X7291's
         * 612x4 and 306x8 drives are the same size) */
        int type = -1;
        for (int t = 0; t < 4 && type < 0; t++)
            if (hdc_type_bytes(&s->hdc_rom[table + 16 * t]) == size &&
                (want_type[u] < 0 || want_type[u] == t)) type = t;
        if (type < 0) {
            fprintf(stderr, "gemu: ibm-fixed-disk: %s is %llu bytes, which isn't %s.  "
                            "The ROM's drive types (17 sectors of 512 bytes per track):\n",
                    gemu_drive_name(d), (unsigned long long)size,
                    want_type[u] < 0 ? "a drive this ROM knows" : "the drive type chosen");
            for (int t = 0; t < 4; t++) {
                const uint8_t *e = &s->hdc_rom[table + 16 * t];
                fprintf(stderr, "  type %d: %4u cylinders, %u heads: %llu bytes\n", t,
                        (unsigned)(e[0] | e[1] << 8), e[2], (unsigned long long)hdc_type_bytes(e));
            }
            fprintf(stderr, "Create an empty one with e.g. truncate -s %llu hd.img\n",
                    (unsigned long long)hdc_type_bytes(&s->hdc_rom[table + 16 * 0]));
            return false;
        }
        const uint8_t *e = &s->hdc_rom[table + 16 * type];
        h->drive[u] = d;
        h->geom[u] = (IbmHdcGeometry){ (uint16_t)(e[0] | e[1] << 8), e[2] };
        h->interleave[u] = (uint8_t)interleave;
        h->jumpers |= (uint8_t)(type << (u == 0 ? 2 : 0));
    }
    /* rom=off: the ROM jumper out.  The drive types still come from the
     * ROM's table - they're what the drive jumpers select. */
    s->hdc_rom_mapped = rom_on ? s->hdc_rom_size : 0;
    h->clock_hz = IBM5150_CPU_HZ;
    warn_old_bios(s);
    return true;
}

/* ── The board as core devices ───────────────────────────────────────────── *
 *
 *   board            the motherboard: CPU clock bookkeeping, the NMI mask
 *     cpu            the 8088 (its executor runs the machine)
 *     dma pic pit ppi   the chips (i8237, i8259, i8253, i8255-pc)
 *     kbd            the keyboard interface: the scan code queue, IRQ 1
 *     speaker        the speaker (PIT 2 AND PB1), a sound stream
 *   isa              the expansion slots
 *     fdc hdc com1 com2 the adapters this configuration has
 *     cga mda hercules  the display adapters, each with its screen
 *
 * Wired as the 5150 is: PIT 0 -> IRQ 0, PIT 1 -> DMA refresh, PIT 2 -> PPI
 * PC5, PPI PB0 -> PIT gate 2; keyboard IRQ 1, COM2 3, COM1 4, fixed disk 5
 * (or 2), diskette 6; DMA 2 diskette, 3 fixed disk.  Device resets run in
 * that order: the chips, then the adapters, as the machine always did. */

static Ibm5150State *machine_of(GemuDevice *d) { return *(Ibm5150State **)d->priv; }

/* The 8088: its registers, and its executor. */
static bool cpu_init(GemuDevice *d) {
    Ibm5150State *s = machine_of(d);
    X86Cpu *c = &s->cpu;
    if (!(s->cpu_exec = gemu_executor_new(d, IBM5150_CPU_HZ, &cpu_ops, s))) return false;
    GEMU_STATE_VAR(d, "ax", c->ax.x);
    GEMU_STATE_VAR(d, "bx", c->bx.x);
    GEMU_STATE_VAR(d, "cx", c->cx.x);
    GEMU_STATE_VAR(d, "dx", c->dx.x);
    GEMU_STATE_VAR(d, "sp", c->sp);
    GEMU_STATE_VAR(d, "bp", c->bp);
    GEMU_STATE_VAR(d, "si", c->si);
    GEMU_STATE_VAR(d, "di", c->di);
    GEMU_STATE_VAR(d, "cs", c->cs);
    GEMU_STATE_VAR(d, "ds", c->ds);
    GEMU_STATE_VAR(d, "es", c->es);
    GEMU_STATE_VAR(d, "ss", c->ss);
    GEMU_STATE_VAR(d, "ip", c->ip);
    GEMU_STATE_VAR(d, "flags", c->flags);
    GEMU_STATE_VAR(d, "halted", c->halted);
    GEMU_STATE_VAR(d, "irq_pending", c->irq_pending);
    GEMU_STATE_VAR(d, "irq_vector", c->irq_vector);
    GEMU_STATE_VAR(d, "int_inhibit", c->int_inhibit);
    GEMU_STATE_VAR(d, "insn_count", c->insn_count);
    return true;
}
static void cpu_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    x86_reset(&machine_of(d)->cpu);
}
static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    x86_format_state(&machine_of(d)->cpu, buf, len);
}
static const GemuDeviceType cpu_type = {
    .name = "i8088", .desc = "Intel 8088 CPU", .size = sizeof(Ibm5150State *),
    .init = cpu_init, .reset = cpu_reset, .describe = cpu_describe,
};

/* The board's state: its clocks, the RAM, the NMI mask; and the serial
 * mouse's (it isn't a device of its own: it hangs off a COM port's lines). */
static bool board_init(GemuDevice *d) {
    Ibm5150State *s = machine_of(d);
    GEMU_STATE_VAR(d, "cycles", s->cycles);
    GEMU_STATE_VAR(d, "pit_phase", s->pit_phase);
    GEMU_STATE_VAR(d, "nmi_mask", s->nmi_mask);
    gemu_state_add(d, "ram", s->ram, 1, s->ram_size);
    if (s->has_mouse) {
        MsMouse *mo = &s->mouse;
        GEMU_STATE_VAR(d, "mouse.powered", mo->powered);
        GEMU_STATE_VAR(d, "mouse.start_wait", mo->start_wait);
        GEMU_STATE_ARRAY(d, "mouse.q", mo->q);
        GEMU_STATE_VAR(d, "mouse.qn", mo->qn);
        GEMU_STATE_VAR(d, "mouse.qpos", mo->qpos);
        GEMU_STATE_VAR(d, "mouse.dx", mo->dx);
        GEMU_STATE_VAR(d, "mouse.dy", mo->dy);
        GEMU_STATE_VAR(d, "mouse.left", mo->left);
        GEMU_STATE_VAR(d, "mouse.right", mo->right);
        GEMU_STATE_VAR(d, "mouse.sent_left", mo->sent_left);
        GEMU_STATE_VAR(d, "mouse.sent_right", mo->sent_right);
    }
    return true;
}
/* A machine parked on an unimplemented opcode runs again from a good state. */
static void board_post_load(GemuDevice *d) {
    machine_of(d)->halted_for_debug = false;
}
static void board_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Ibm5150State *s = machine_of(d);
    s->nmi_mask = 0;
    s->pit_phase = 0;
}
static void board_describe(GemuDevice *d, char *buf, size_t len) {
    Ibm5150State *s = machine_of(d);
    GemuTime t = gemu_machine_time(s->machine);
    snprintf(buf, len, "%llu CPU clocks since power-on; machine time %llu.%06llu s\n"
             "NMI mask %02X; %d unhandled port%s logged",
             (unsigned long long)s->cycles, (unsigned long long)(t / GEMU_PS_PER_SEC),
             (unsigned long long)(t % GEMU_PS_PER_SEC / 1000000u), s->nmi_mask,
             s->iolog_n, s->iolog_n == 1 ? "" : "s");
}
static const GemuDeviceType board_type = {
    .name = "ibm5150-board", .desc = "IBM 5150 system board", .size = sizeof(Ibm5150State *),
    .init = board_init, .reset = board_reset, .post_load = board_post_load,
    .describe = board_describe,
};

static bool kbd_init(GemuDevice *d) {
    Ibm5150State *s = machine_of(d);
    if (!(s->kb_irq = gemu_line_out(d, "irq"))) return false;
    GEMU_STATE_ARRAY(d, "queue", s->kbq);
    GEMU_STATE_VAR(d, "queued", s->kbq_len);
    GEMU_STATE_VAR(d, "full", s->kb_full);
    GEMU_STATE_VAR(d, "clock_low_at", s->kb_clock_low_at);
    GEMU_STATE_VAR(d, "reset_at", s->kb_reset_at);
    GEMU_STATE_VAR(d, "next_at", s->kb_next_at);
    GEMU_STATE_VAR(d, "port_b", s->port_b);
    return true;
}
/* After the PPI's reset (it's later in the tree): port B is 0 again. */
static void kbd_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Ibm5150State *s = machine_of(d);
    s->kbq_len = 0;
    s->kb_full = false;
    s->kb_reset_at = s->kb_next_at = 0;
    s->port_b = s->ppi->port_b;
    kb_update_irq(s);
}
static const GemuDeviceType kbd_type = {
    .name = "ibm5150-keyboard", .desc = "IBM 5150 keyboard interface (scan codes into the PPI, IRQ 1)",
    .size = sizeof(Ibm5150State *), .init = kbd_init, .reset = kbd_reset,
};

static const GemuDeviceType isa_type = {
    .name = "isa", .desc = "ISA expansion slots", .stateless = true,
};

/* DMA sees the CPU's memory bus. */
static uint8_t bus_r(void *ud, uint64_t a)            { return bus_read8((uint32_t)a, ud); }
static void    bus_w(void *ud, uint64_t a, uint8_t v) { bus_write8((uint32_t)a, v, ud); }
static bool    bus_peek(void *ud, uint64_t a, uint8_t *v) { return ibm5150_mon_read(ud, a, v); }
static const GemuHandlers bus_io = { .r8 = bus_r, .w8 = bus_w, .peek8 = bus_peek };

static GemuDevice *add_dev(Ibm5150State *s, const char *type, const char *name,
                           GemuDevice *parent, const char *opts) {
    GemuDevice *d = gemu_device_add_type(s->machine, type, name, parent, opts);
    if (!d) fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(s->machine));
    return d;
}

static GemuDevice *add_own(Ibm5150State *s, const GemuDeviceType *t, const char *name,
                           GemuDevice *parent) {
    GemuDevice *d = gemu_device_add(s->machine, t, name, parent, NULL);
    if (!d) fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(s->machine));
    else if (t->size) *(Ibm5150State **)d->priv = s;
    return d;
}

/* The devices and spaces for this configuration, up to the point where the
 * adapters can be given their drives (then wire_board()). */
static bool build_board(Ibm5150State *s) {
    GemuMachine *m = s->machine = gemu_machine_new("ibm5150");
    if (!m) return false;
    s->io = gemu_space_new(m, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    s->bus = gemu_space_new(m, "program", 20, 8, GEMU_LITTLE_ENDIAN);
    if (!s->io || !s->bus || !gemu_space_map(s->bus, 0, 0x100000, &bus_io, s, 0, 0))
        return false;

    char sw[48], clock[32];
    snprintf(sw, sizeof(sw), "sw1=%u,sw2=%u", s->sw1, s->sw2);
    snprintf(clock, sizeof(clock), "clock=%u", IBM5150_CPU_HZ);
    GemuDevice *board = add_own(s, &board_type, "board", NULL);
    if (!board ||
        !(s->dev_cpu = add_own(s, &cpu_type, "cpu", board)) ||
        !(s->dev_dma = add_dev(s, "i8237", "dma", board, "space=program")) ||
        !(s->dev_pic = add_dev(s, "i8259", "pic", board, NULL)) ||
        !(s->dev_pit = add_dev(s, "i8253", "pit", board, NULL)) ||
        !(s->dev_ppi = add_dev(s, "i8255-pc", "ppi", board, sw)) ||
        !(s->dev_kbd = add_own(s, &kbd_type, "kbd", board)) ||
        !(s->dev_speaker = add_dev(s, "pc-speaker", "speaker", board, NULL)))
        return false;
    gemu_device_set_clock(board, IBM5150_CPU_HZ);
    gemu_device_set_clock(s->dev_pit, PIT_HZ);
    GemuDevice *isa = add_own(s, &isa_type, "isa", NULL);
    if (!isa) return false;
    gemu_device_set_clock(isa, IBM5150_CPU_HZ);   /* the bus runs at the CPU's clock */
    if (s->has_fdc && !(s->dev_fdc = add_dev(s, "upd765", "fdc", isa, NULL))) return false;
    if (s->has_hdc && !(s->dev_hdc = add_dev(s, "ibm-fixed-disk", "hdc", isa, NULL))) return false;
    for (int i = 0; i < 2; i++)
        if (s->has_com[i] &&
            !(s->dev_com[i] = add_dev(s, "ins8250", i == 0 ? "com1" : "com2", isa, clock)))
            return false;
    /* the display adapters, each with the monitor it drives (a screen) */
    for (int i = 0; i < s->n_heads; i++) {
        X86VgaType t = s->head_card[i];
        const char *type = t == X86_VGA_CGA ? "cga" : t == X86_VGA_HERCULES ? "hercules" : "mda";
        GemuDevice **slot = t == X86_VGA_CGA ? &s->dev_cga : t == X86_VGA_HERCULES ? &s->dev_herc
                                                                                 : &s->dev_mda;
        if (!(*slot = add_dev(s, type, type, isa, t == X86_VGA_CGA ? NULL : s->head_opts[i])))
            return false;
    }
    /* other cards in the slots (-device adlib, ...) */
    for (int i = 0; i < s->n_isa_cards; i++) {
        char type[64];
        const char *opts = NULL;
        if (!gemu_device_spec_split(s->isa_cards[i], type, sizeof type, &opts) ||
            !add_dev(s, type, type, isa, opts))
            return false;
    }
    if (!gemu_machine_init(m)) {
        fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(m));
        return false;
    }
    s->dma = gemu_i8237(s->dev_dma);
    s->pic = gemu_i8259(s->dev_pic);
    s->pit = gemu_i8253(s->dev_pit);
    s->ppi = gemu_i8255(s->dev_ppi);
    if (s->has_fdc) {
        s->fdc = gemu_upd765(s->dev_fdc);
        for (int u = 0; u < 4; u++) s->fdc->drive[u] = s->floppy[u];
    }
    if (s->has_hdc) s->hdc = gemu_ibm_hdc(s->dev_hdc);
    for (int i = 0; i < 2; i++)
        if (s->has_com[i]) s->com[i] = gemu_ins8250(s->dev_com[i]);
    if (s->dev_cga)  s->cga = gemu_cga(s->dev_cga);
    if (s->dev_mda)  s->mda = gemu_mda(s->dev_mda);
    if (s->dev_herc) s->herc = gemu_hercules(s->dev_herc);
    for (int i = 0; i < s->n_heads; i++)
        s->head_screen[i] = gemu_pc_video_screen(s->head_card[i] == X86_VGA_CGA ? s->dev_cga
                                                 : s->head_card[i] == X86_VGA_HERCULES ? s->dev_herc
                                                                                      : s->dev_mda);
    return true;
}

/* Ports, lines and DMA channels; then the power-on reset. */
static bool wire_board(Ibm5150State *s) {
    GemuMachine *m = s->machine;
    if (!io_build(s)) return false;
    char hdc_irq[32];
    snprintf(hdc_irq, sizeof(hdc_irq), "board/pic:ir%u", s->hdc_irq);
    struct { const char *out, *in; bool want; } w[] = {
        { "board/pit:out0", "board/pic:ir0",   true },
        { "board/pit:out1", "board/dma:dreq0", true },
        { "board/pit:out2", "board/ppi:pc5",   true },
        { "board/ppi:pb0",  "board/pit:gate2", true },
        { "board/pit:out2", "board/speaker:timer", true },
        { "board/ppi:pb1",  "board/speaker:data",  true },
        { "board/kbd:irq",  "board/pic:ir1",   true },
        { "isa/com2:int",   "board/pic:ir3",   s->has_com[1] },
        { "isa/com1:int",   "board/pic:ir4",   s->has_com[0] },
        { "isa/hdc:int",    hdc_irq,           s->has_hdc },
        { "isa/fdc:int",    "board/pic:ir6",   s->has_fdc },
    };
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++)
        if (w[i].want && !gemu_lines_connect(m, w[i].out, w[i].in, 0)) {
            fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(m));
            return false;
        }
    /* a card's IRQ lines irq2..irq7 (the XT slot's) to the PIC */
    for (GemuDevice *d = gemu_device_find(m, "isa")->first_child; d; d = d->next_sibling)
        for (int n = 2; n < 8; n++) {
            char out[16], from[GEMU_DEVICE_PATH_MAX + 16], to[32];
            snprintf(out, sizeof out, "irq%d", n);
            if (!gemu_line_find_out(d, out)) continue;
            snprintf(from, sizeof from, "%s:%s", d->path, out);
            snprintf(to, sizeof to, "board/pic:ir%d", n);
            if (!gemu_lines_connect(m, from, to, 0)) {
                fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(m));
                return false;
            }
        }
    if (s->has_fdc) gemu_upd765_set_dma(s->dev_fdc, s->dev_dma, 2);
    if (s->has_hdc) gemu_ibm_hdc_set_dma(s->dev_hdc, s->dev_dma, 3);
    if (!gemu_machine_start(m)) {
        fprintf(stderr, "gemu: ibm5150: %s\n", gemu_machine_error(m));
        return false;
    }
    return true;
}

static void ibm5150_reset(Ibm5150State *s) {
    gemu_machine_reset(s->machine, GEMU_RESET_HARD);   /* the CPU, chips and adapters */
    s->halted_for_debug = false;
}

Ibm5150State *ibm5150_create(const X86Config *cfg) {
    Ibm5150State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    uint32_t ram = cfg->ram_size ? cfg->ram_size : IBM5150_RAM_MAX;
    if (ram > IBM5150_RAM_MAX) ram = IBM5150_RAM_MAX;
    s->vga = cfg->vga;
    s->n_heads = 1;
    s->head_card[0] = cfg->vga;
    if (cfg->has_vga2 && cfg->vga2 != cfg->vga) {
        s->head_card[1] = cfg->vga2;
        s->n_heads = 2;
    }
    for (int i = 0; i < s->n_heads; i++) {
        if (s->head_card[i] == X86_VGA_CGA) { s->has_cga = true; continue; }
        bool herc = s->head_card[i] == X86_VGA_HERCULES;
        if (herc) s->has_herc = true;
        else      s->has_mda = true;
        s->head_opts[i] = i == 0 ? cfg->vga_opts : cfg->vga2_opts;   /* checked by the device */
    }
    Switches sw;
    if (!parse_switches(cfg->machine_opts, &sw)) { free(s); return NULL; }
    s->ram_size = configure_switches(s, ram);
    if (sw.ram_kb > 0) {
        /* switches claiming other than what's fitted: keep the RAM, set
         * the switches from ram-switch= (more than fitted fails POST) */
        uint32_t fitted = s->ram_size;
        configure_switches(s, (uint32_t)sw.ram_kb << 10);
        s->ram_size = fitted;
    }
    s->ram = calloc(1, s->ram_size);
    if (!s->ram) { free(s); return NULL; }

    bool have_bios = false;
    const char *hdc_rom = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        const X86RomLoad *r = &cfg->roms[i];
        if (r->region && strncmp(r->region, "fixed-disk-", 11) == 0) {
            /* the first adapter (5000059) wins over its successor */
            if (!hdc_rom || strcmp(r->region, "fixed-disk-5000059") == 0) hdc_rom = r->path;
            continue;
        }
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
    s->cpu.intr_pending = cpu_intr_pending;
    s->cpu.intr_ack = cpu_intr_ack;
    s->cpu.bus_ud = s;
    s->cpu.on_unknown_opcode = on_unknown_opcode;


    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = ibm5150_mon_read, .write = ibm5150_mon_write, .ud = s,
        .addr_bits = 20, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, ibm5150_mon_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, ibm5150_screendump, s);
    gemu_monitor_bind(s->monitor, "sendkey", ibm5150_cmd_sendkey, s);
    if (cfg->mouse_com) {
        attach_com(s, cfg->mouse_com - 1);
        s->has_mouse = true;
        gemu_monitor_bind(s->monitor, "mouse_move", ibm5150_cmd_mouse_move, s);
        gemu_monitor_bind(s->monitor, "mouse_button", ibm5150_cmd_mouse_button, s);
    }
    attach_floppies(s, sw.floppies);
    if (sw.floppy >= 0)   s->sw1 = (uint8_t)((s->sw1 & ~0x01) | sw.floppy);
    if (sw.floppies > 0)  s->sw1 = (uint8_t)((s->sw1 & ~0xC0) | (sw.floppies - 1) << 6);
    if (sw.display >= 0)  s->sw1 = (uint8_t)((s->sw1 & ~0x30) | sw.display << 4);
    if (cfg->machine_opts) {
        static const char *const disp[] = { "none", "CGA 40x25", "CGA 80x25", "monochrome" };
        printf("gemu: DIP switches: diskettes %s, %d drive(s), display %s, %u K\n",
               s->sw1 & 1 ? "on" : "off", ((s->sw1 >> 6) & 3) + 1,
               disp[(s->sw1 >> 4) & 3],
               (((s->sw1 >> 2) & 3) + 1) * 16 + (s->sw2 & 0x1F) * 32);
    }
    for (int u = 0; u < 4; u++)
        if (s->floppy[u]) gemu_drive_attach_monitor(s->floppy[u], s->monitor);
    if (cfg->hdc == X86_HDC_NONE &&
        (gemu_drive_get(GEMU_DRIVE_IF_IDE, 0, 0) || gemu_drive_get(GEMU_DRIVE_IF_IDE, 0, 1))) {
        fprintf(stderr, "gemu: the 5150 needs a hard disk adapter for -hda/-hdb: "
                        "add -device ibm-fixed-disk\n");
        ibm5150_destroy(s);
        return NULL;
    }
    s->has_hdc = cfg->hdc == X86_HDC_IBM;
    s->n_isa_cards = cfg->n_isa_cards;
    for (int i = 0; i < cfg->n_isa_cards; i++) s->isa_cards[i] = cfg->isa_cards[i];
    s->hdc_irq = 5;
    /* the device tree, then what needs its devices: drives, the mouse, wiring */
    if (!build_board(s) || (s->has_hdc && !attach_fixed_disk(s, cfg, hdc_rom))) {
        ibm5150_destroy(s);
        return NULL;
    }
    if (s->has_mouse) attach_mouse(s, cfg->mouse_com);
    if (!wire_board(s)) {
        ibm5150_destroy(s);
        return NULL;
    }
    for (int u = 0; u < 2; u++)
        if (s->hdc && s->hdc->drive[u]) gemu_drive_attach_monitor(s->hdc->drive[u], s->monitor);
    gemu_monitor_bind(s->monitor, "ioports", ibm5150_cmd_ioports, s);
    gemu_monitor_attach_machine(s->monitor, s->machine, NULL);

    if (gemu_display_wanted(cfg->display_type)) {
        /* each monitor shows its card's screen, at the screen's size and shape
         * (the MDA's 720x350 filled a 4:3 monitor; CGA's 640x400 is drawn
         * with doubled scanlines and already has its shape) */
        const GemuBitmap *b0 = gemu_screen_render(s->head_screen[0]);
        int aw0, ah0;
        gemu_screen_get_aspect(s->head_screen[0], &aw0, &ah0);
        GemuDisplayConfig dc = {
            .fb_width = b0->width, .fb_height = b0->height,
            .monitor = s->monitor,
            .aspect_w = aw0, .aspect_h = ah0,
            .scale = cfg->display_scale > 0 ? cfg->display_scale : 1, /* 640x400 is big enough at 1x */
            .no_menu = true, /* full keyboard machine - no gamepad-style input menu */
            .capture_pointer = s->has_mouse,
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (s->n_heads > 1) {
            GemuDisplayHead heads[2];
            for (int i = 0; i < 2; i++) {
                bool mda = x86_vga_mono(s->head_card[i]);
                static char names[2][32];
                snprintf(names[i], sizeof(names[i]), "Monitor %d (%s)", i + 1,
                         s->head_card[i] == X86_VGA_HERCULES ? "Hercules" : mda ? "MDA" : "CGA");
                const GemuBitmap *b = gemu_screen_render(s->head_screen[i]);
                int aw, ah;
                gemu_screen_get_aspect(s->head_screen[i], &aw, &ah);
                heads[i] = (GemuDisplayHead){
                    .name = names[i], .width = b->width, .height = b->height,
                    .aspect_w = aw, .aspect_h = ah,
                };
            }
            gemu_display_set_heads(s->display, heads, 2);
        }
        s->audio = gemu_audio_open("ibm5150", IBM5150_AUDIO_HZ);
    }
    /* the mixer runs headless too: wavcapture records without a sound card */
    gemu_mixer_set_output(s->machine, IBM5150_AUDIO_HZ, 1, s->audio ? audio_sink : NULL, s->audio);

    ibm5150_reset(s);
    return s;
}

void ibm5150_destroy(Ibm5150State *s) {
    if (!s) return;
    if (s->audio) gemu_audio_close(s->audio);
    if (s->display) gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    gemu_machine_free(s->machine);
    free(s->ram);
    free(s);
}

static const char *card_name(X86VgaType t) {
    return t == X86_VGA_MDA ? "MDA (monochrome)" : t == X86_VGA_HERCULES ? "Hercules (monochrome)" : "CGA";
}

static const char *video_desc(const Ibm5150State *s) {
    static char buf[128];
    if (s->n_heads > 1)
        snprintf(buf, sizeof(buf), "%s + %s, View menu switches monitors",
                 card_name(s->head_card[0]), card_name(s->head_card[1]));
    else
        snprintf(buf, sizeof(buf), "%s", card_name(s->vga));
    return buf;
}

void ibm5150_run(Ibm5150State *s, const X86Config *cfg) {
    printf("gemu-x86: IBM PC 5150\n"
           "  RAM   : %u KiB\n"
           "  Video : %s\n"
           "  BASIC : %s\n",
           s->ram_size >> 10,
           video_desc(s),
           s->has_basic ? "Cassette BASIC present" : "(none loaded)");
    for (int u = 0; u < 4; u++)
        if (s->floppy[u])
            printf("  %s: %s\n", gemu_drive_name(s->floppy[u]),
                   gemu_drive_inserted(s->floppy[u]) ? gemu_drive_filename(s->floppy[u])
                                                     : "(empty)");
    if (s->has_mouse)
        printf("  Mouse : Microsoft serial mouse on COM%d (click the window to capture, "
               "Ctrl+Alt+G to release)\n", s->has_com[0] ? 1 : 2);
    if (s->has_hdc) {
        printf("  Fixed disk adapter: IBM, %s, IRQ %u, DMA 3\n",
               s->hdc_rom_mapped ? "ROM at C8000h" : "ROM disabled", s->hdc_irq);
        for (int u = 0; u < 2; u++)
            if (s->hdc->drive[u])
                printf("  %s: %s (%u cylinders, %u heads, drive type %u)\n",
                       gemu_drive_name(s->hdc->drive[u]), gemu_drive_filename(s->hdc->drive[u]),
                       s->hdc->geom[u].cyls, s->hdc->geom[u].heads,
                       (s->hdc->jumpers >> (u == 0 ? 2 : 0)) & 3u);
    }

    gemu_monitor_start(s->monitor);

    bool running = true;
    bool reported_halt = false;
    while (running) {
        /* Run until the display adapter starts its next frame (1/60 s of PC
         * time on the CGA, 1/50 s on the MDA). */
        bool idle = gemu_monitor_is_paused(s->monitor) || s->halted_for_debug;
        if (!idle) run_frame(s);

        /* HLT with interrupts off never resumes: POST does this on failure. */
        bool dead = s->cpu.halted && !(s->cpu.flags & X86_FLAG_IF);
        if (dead && !reported_halt && !s->halted_for_debug)
            fprintf(stderr, "ibm5150: CPU halted with interrupts disabled at %04x:%04x\n",
                    s->cpu.cs, (uint16_t)(s->cpu.ip - 1));
        reported_halt = dead;

        if (s->display) {
            gemu_display_poll(s->display);
            scan_keyboard(s);
            if (s->has_mouse) {
                GemuPointerState p = gemu_display_get_pointer(s->display);
                /* a click shorter than a frame still counts: pressed says
                 * the button went down since the last poll */
                bool left = p.button || p.pressed, right = p.right_button || p.right_pressed;
                if (p.rel_x || p.rel_y || left || right || s->mouse.left || s->mouse.right)
                    msmouse_input(&s->mouse, p.rel_x, p.rel_y, left, right);
            }
            video_present(s);
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor) || dead);
            if (gemu_display_should_quit(s->display)) running = false;
            if (gemu_display_reset_requested(s->display)) {
                ibm5150_reset(s);
                gemu_display_clear_flags(s->display);
            }
            /* Real time.  With sound, the audio clock: keep ~3 frames of it
             * queued (the sound card never runs dry, and nothing drifts);
             * without, the scheduler paces the machine to the host clock. */
            if (idle) {
                gemu_sleep_ms(16);
                gemu_machine_pace_reset(s->machine);
            } else if (s->audio) {
                while (gemu_audio_queued(s->audio) > IBM5150_AUDIO_QUEUE_MAX)
                    gemu_sleep_ms(1);
            } else {
                gemu_machine_pace(s->machine, 0);
            }
        }
        /* headless (-display none): deliberately unthrottled while running -
         * see machine_i2000.c's Sleep(0) history before adding a sleep. */
        else if (idle || dead)
            gemu_sleep_ms(1);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else running = false;
                break;
            case GEMU_MON_RESET: ibm5150_reset(s); break;
            case GEMU_MON_STEP:
                run_steps(s, gemu_monitor_step_count(s->monitor));
                break;
            default: break;
            }
        }
    }
    gemu_monitor_stop(s->monitor);
}
