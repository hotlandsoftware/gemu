#include "ibm5150.h"
#include "cgafont8x8.h"
#include "mdafont8x14.h"
#include "gemu/util.h"
#include "gemu/screendump.h"
#include <ctype.h>
#include <SDL2/SDL.h>
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

/* VRAM windows: MDA B0000-B7FFF (4K mirrored), CGA B8000-BBFFF. */
static bool video_mem_read(const Ibm5150State *s, uint32_t addr, uint8_t *val) {
    if (s->has_mda && addr >= IBM5150_MDA_BASE && addr < IBM5150_MDA_END) {
        *val = mda_mem_read(&s->mda, addr - IBM5150_MDA_BASE);
        return true;
    }
    if (s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END) {
        *val = cga_mem_read(&s->cga, addr - IBM5150_CGA_BASE);
        return true;
    }
    return false;
}

static bool video_mem_write(Ibm5150State *s, uint32_t addr, uint8_t val) {
    if (s->has_mda && addr >= IBM5150_MDA_BASE && addr < IBM5150_MDA_END) {
        mda_mem_write(&s->mda, addr - IBM5150_MDA_BASE, val);
        return true;
    }
    if (s->has_cga && addr >= IBM5150_CGA_BASE && addr < IBM5150_CGA_END) {
        cga_mem_write(&s->cga, addr - IBM5150_CGA_BASE, val);
        return true;
    }
    return false;
}

/* Registers: MDA at 0x3B0-0x3BB (0x3BC- is its printer port), CGA at
 * 0x3D0-0x3DF. */
static bool mda_port(const Ibm5150State *s, uint16_t port) {
    return s->has_mda && port >= 0x3B0 && port <= 0x3BB;
}

static bool cga_port(const Ibm5150State *s, uint16_t port) {
    return s->has_cga && port >= 0x3D0 && port <= 0x3DF;
}

/* Both cards run; the primary's frames pace the machine. */
static bool video_tick(Ibm5150State *s, uint32_t clocks) {
    bool mda_frame = s->has_mda && mda_tick(&s->mda, clocks);
    bool cga_frame = s->has_cga && cga_tick(&s->cga, clocks);
    return s->vga == X86_VGA_MDA ? mda_frame : cga_frame;
}

/* The card on the monitor the window shows (View menu). */
static X86VgaType shown_card(const Ibm5150State *s) {
    int head = s->display ? gemu_display_head(s->display) : 0;
    return s->head_card[head < s->n_heads ? head : 0];
}

static void render_card(Ibm5150State *s, X86VgaType card) {
    if (card == X86_VGA_MDA) {
        s->fb_w = MDA_WIDTH;
        s->fb_h = MDA_HEIGHT;
        mda_render(&s->mda, s->fb, mdafont8x14);
    } else {
        s->fb_w = IBM5150_CGA_FB_W;
        s->fb_h = IBM5150_CGA_FB_H;
        cga_render(&s->cga, s->fb, s->fb_w, s->fb_h, cgafont8x8);
    }
}

/* The main window's monitor (screendump). */
static void video_render(Ibm5150State *s) {
    render_card(s, shown_card(s));
}

/* Every monitor that's on screen: the main window's, and any detached into
 * a window of its own (View menu). */
static void video_present(Ibm5150State *s) {
    for (int i = 0; i < s->n_heads; i++) {
        if (!gemu_display_head_shown(s->display, i)) continue;
        render_card(s, s->head_card[i]);
        gemu_display_render_head(s->display, i, s->fb, s->fb_w, s->fb_h);
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

/* ── Speaker ─────────────────────────────────────────────────────────────── *
 *
 * The speaker sounds PIT channel 2's square wave when PB0 (gate) and PB1
 * (data) are both set; the tone generator is retuned whenever either the
 * port or the channel changes. */

static void update_speaker(Ibm5150State *s) {
    if (!s->speaker) return;
    const I8253Channel *c = &s->pit.ch[2];
    bool on = (s->ppi.port_b & 0x03) == 0x03 && c->mode == 3 && c->counting;
    if (on) {
        unsigned div = c->reload ? c->reload : 0x10000u;
        rca_pcspk_set_freq(s->speaker, PIT_HZ / div);
    }
    rca_pcspk_set_gate(s->speaker, on);
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */

static bool kb_enabled(const Ibm5150State *s) {
    return !(s->ppi.port_b & 0x80) && (s->ppi.port_b & 0x40);
}

static void kb_update_irq(Ibm5150State *s) {
    i8259_set_irq(&s->pic, 1, s->kb_full && !(s->ppi.port_b & 0x80));
}

/* Shift the next byte into the PPI if the keyboard may send one. */
static void kb_service(Ibm5150State *s) {
    if (s->kb_full || !kb_enabled(s) || s->cycles < s->kb_next_at) return;
    if (s->kb_reset_at) {
        if (s->cycles < s->kb_reset_at) return;
        s->kb_reset_at = 0;
        s->ppi.kb_data = 0xAA;
    } else if (s->kbq_len > 0) {
        s->ppi.kb_data = s->kbq[0];
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
        s->ppi.kb_data = 0;
    }
    kb_update_irq(s);
}

/* ── I/O ports ───────────────────────────────────────────────────────────── */

/* Standard adapters this machine doesn't have.  POST probes all of them,
 * so accesses are expected and not worth logging; the bus floats high. */
static bool empty_slot(uint16_t port) {
    return (port >= 0x200 && port <= 0x20F) ||   /* game port */
           (port >= 0x278 && port <= 0x27F) ||   /* LPT2 */
           (port >= 0x2F8 && port <= 0x2FF) ||   /* COM2 */
           (port >= 0x378 && port <= 0x37F) ||   /* LPT1 */
           (port >= 0x3B0 && port <= 0x3BF) ||   /* MDA + its printer port (when absent) */
           (port >= 0x3D0 && port <= 0x3DF) ||   /* CGA (when absent) */
           (port >= 0x3F0 && port <= 0x3F7) ||   /* diskette adapter (when absent) */
           (port >= 0x3F8 && port <= 0x3FF);     /* COM1 */
}

static uint8_t io_read8(uint16_t port, void *ud) {
    Ibm5150State *s = ud;
    port &= 0x3FF;   /* the PC decodes only A0-A9 */
    if (port <= 0x0F) return i8237_io_read(&s->dma, port);
    if (port >= 0x20 && port <= 0x21) return i8259_io_read(&s->pic, port);
    if (port >= 0x40 && port <= 0x43) return i8253_io_read(&s->pit, port);
    if (port >= 0x60 && port <= 0x63) return i8255_io_read(&s->ppi, port);
    if (port >= 0x80 && port <= 0x83) return i8237_page_read(&s->dma, port);
    if (mda_port(s, port)) return mda_io_read(&s->mda, port);
    if (cga_port(s, port)) return cga_io_read(&s->cga, port);
    if (s->has_fdc && port >= 0x3F0 && port <= 0x3F7) return upd765_read(&s->fdc, port);
    if (empty_slot(port)) return 0xFF;
    iolog(s, port, false, 0);
    return 0xFF;
}
static void io_write8(uint16_t port, uint8_t val, void *ud) {
    Ibm5150State *s = ud;
    port &= 0x3FF;
    if (port <= 0x0F) { i8237_io_write(&s->dma, port, val); return; }
    if (port >= 0x20 && port <= 0x21) { i8259_io_write(&s->pic, port, val); return; }
    if (port >= 0x40 && port <= 0x43) {
        i8253_io_write(&s->pit, port, val);
        update_speaker(s);
        return;
    }
    if (port >= 0x60 && port <= 0x63) {
        uint8_t old = s->ppi.port_b;
        i8255_io_write(&s->ppi, port, val);
        if ((port & 3) == 1) {
            i8253_set_gate(&s->pit, 2, val & 0x01);
            kb_port_b_changed(s, old, val);
            update_speaker(s);
        }
        return;
    }
    if (port >= 0x80 && port <= 0x83) { i8237_page_write(&s->dma, port, val); return; }
    if (port >= 0xA0 && port <= 0xAF) { s->nmi_mask = val; return; }
    if (mda_port(s, port)) { mda_io_write(&s->mda, port, val); return; }
    if (cga_port(s, port)) { cga_io_write(&s->cga, port, val); return; }
    if (s->has_fdc && port >= 0x3F0 && port <= 0x3F7) { upd765_write(&s->fdc, port, val); return; }
    if (empty_slot(port)) return;
    iolog(s, port, true, val);
}

/* ── Diskette adapter wiring: IRQ6, DMA channel 2 ────────────────────────── */

static void fdc_set_irq(void *ud, bool level) {
    Ibm5150State *s = ud;
    i8259_set_irq(&s->pic, 6, level);
}

static bool fdc_dma_from_mem(void *ud, uint8_t *val, bool *tc) {
    Ibm5150State *s = ud;
    uint32_t phys;
    if (!i8237_transfer(&s->dma, 2, &phys, tc)) return false;
    *val = bus_read8(phys, s);
    return true;
}

static bool fdc_dma_to_mem(void *ud, uint8_t val, bool *tc) {
    Ibm5150State *s = ud;
    uint32_t phys;
    if (!i8237_transfer(&s->dma, 2, &phys, tc)) return false;
    bus_write8(phys, val, s);
    return true;
}

/* ── Interrupts and timers ───────────────────────────────────────────────── */

static bool cpu_intr_pending(void *ud) {
    Ibm5150State *s = ud;
    return i8259_intr(&s->pic);
}

static uint8_t cpu_intr_ack(void *ud) {
    Ibm5150State *s = ud;
    return i8259_ack(&s->pic);
}

/* PIT outputs: channel 0 is IRQ0, channel 1 requests a DRAM refresh cycle
 * on DMA channel 0, channel 2 feeds the speaker and PPI port C bit 5. */
static void pit_out_changed(void *ud, unsigned ch, bool level) {
    Ibm5150State *s = ud;
    switch (ch) {
    case 0: i8259_set_irq(&s->pic, 0, level); break;
    case 1: if (level) i8237_refresh(&s->dma); break;
    case 2: s->ppi.timer2_out = level; break;
    }
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
        i8253_tick(&s->pit, s->pit_phase / 4);
        s->pit_phase %= 4;
    }
    kb_service(s);
    if (s->has_fdc) upd765_tick(&s->fdc, clocks);
    return video_tick(s, clocks);
}

static bool step_cpu(Ibm5150State *s) {
    return advance(s, x86_step(&s->cpu));
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
    video_render(s);
    return gemu_screendump_argb(path, s->fb, s->fb_w, s->fb_h);
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
    s->ppi.sw1 = (uint8_t)(((planar / 16 - 1) << 2) |
                           (s->vga == X86_VGA_MDA ? 0x30 : 0x20));   /* mono / 80x25 color */
    s->ppi.sw2 = (uint8_t)blocks;
    return (planar + blocks * 32) << 10;
}

/* Floppy drives come from -drive if=floppy / -fda / -fdb: the diskette
 * adapter is installed when there's at least one, with as many drives as
 * the highest unit used (the 5150 supports four). */
static void attach_floppies(Ibm5150State *s) {
    int count = 0;
    for (int u = 0; u < 4; u++) {
        s->fdc.drive[u] = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, u);
        if (s->fdc.drive[u]) count = u + 1;
    }
    if (!count) return;
    s->has_fdc = true;
    s->fdc.set_irq = fdc_set_irq;
    s->fdc.dma_from_mem = fdc_dma_from_mem;
    s->fdc.dma_to_mem = fdc_dma_to_mem;
    s->fdc.ud = s;
    s->ppi.sw1 = (uint8_t)(s->ppi.sw1 | 0x01 | ((count - 1) << 6));
}

static void ibm5150_reset(Ibm5150State *s) {
    x86_reset(&s->cpu);
    i8237_reset(&s->dma);
    i8259_reset(&s->pic);
    i8253_reset(&s->pit);
    i8255_reset(&s->ppi);
    cga_reset(&s->cga);
    mda_reset(&s->mda);
    if (s->has_fdc) { s->fdc.dor = 0; upd765_reset(&s->fdc); }
    /* The PIT's outputs start high without a change callback; the PPI's
     * copy of timer 2 must match or POST's cassette test misses an edge. */
    s->ppi.timer2_out = i8253_out(&s->pit, 2);
    s->nmi_mask = 0;
    s->pit_phase = 0;
    s->kbq_len = 0;
    s->kb_full = false;
    s->kb_reset_at = s->kb_next_at = 0;
    s->halted_for_debug = false;
    update_speaker(s);
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
        s->has_mda = true;
        const char *opts = i == 0 ? cfg->vga_opts : cfg->vga2_opts;
        char err[160];
        if (!mda_set_options(&s->mda, opts, err, sizeof(err))) {
            fprintf(stderr, "gemu: %s mda: %s\n", i == 0 ? "-vga" : "-device", err);
            free(s);
            return NULL;
        }
    }
    s->ram_size = configure_switches(s, ram);
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
    s->cpu.intr_pending = cpu_intr_pending;
    s->cpu.intr_ack = cpu_intr_ack;
    s->cpu.bus_ud = s;
    s->cpu.on_unknown_opcode = on_unknown_opcode;
    s->pit.out_changed = pit_out_changed;
    s->pit.ud = s;

    s->fb_w = s->vga == X86_VGA_MDA ? MDA_WIDTH  : IBM5150_CGA_FB_W;
    s->fb_h = s->vga == X86_VGA_MDA ? MDA_HEIGHT : IBM5150_CGA_FB_H;
    /* big enough for either card's picture */
    s->fb = calloc((size_t)MDA_WIDTH * IBM5150_CGA_FB_H, sizeof(uint32_t));
    if (!s->fb) { free(s->ram); free(s); return NULL; }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = ibm5150_mon_read, .write = ibm5150_mon_write, .ud = s,
        .addr_bits = 20, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, ibm5150_mon_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, ibm5150_screendump, s);
    gemu_monitor_bind(s->monitor, "sendkey", ibm5150_cmd_sendkey, s);
    attach_floppies(s);
    for (int u = 0; u < 4; u++)
        if (s->fdc.drive[u]) gemu_drive_attach_monitor(s->fdc.drive[u], s->monitor);

    if (gemu_display_wanted(cfg->display_type)) {
        GemuDisplayConfig dc = {
            .fb_width = s->fb_w, .fb_height = s->fb_h,
            .monitor = s->monitor,
            /* The MDA's 720x350 filled a 4:3 monitor; CGA's 640x400 is drawn
             * with doubled scanlines and already has its shape. */
            .aspect_w = s->vga == X86_VGA_MDA ? 4 : 0,
            .aspect_h = s->vga == X86_VGA_MDA ? 3 : 0,
            .scale = cfg->display_scale > 0 ? cfg->display_scale : 1, /* 640x400 is big enough at 1x */
            .no_menu = true, /* full keyboard machine - no gamepad-style input menu */
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (s->n_heads > 1) {
            GemuDisplayHead heads[2];
            for (int i = 0; i < 2; i++) {
                bool mda = s->head_card[i] == X86_VGA_MDA;
                static char names[2][32];
                snprintf(names[i], sizeof(names[i]), "Monitor %d (%s)", i + 1, mda ? "MDA" : "CGA");
                heads[i] = (GemuDisplayHead){
                    .name = names[i],
                    .width = mda ? MDA_WIDTH : IBM5150_CGA_FB_W,
                    .height = mda ? MDA_HEIGHT : IBM5150_CGA_FB_H,
                    .aspect_w = mda ? 4 : 0, .aspect_h = mda ? 3 : 0,
                };
            }
            gemu_display_set_heads(s->display, heads, 2);
        }
        s->speaker = rca_pcspk_create(440);
    }

    ibm5150_reset(s);
    return s;
}

void ibm5150_destroy(Ibm5150State *s) {
    if (!s) return;
    if (s->speaker) rca_pcspk_destroy(s->speaker);
    if (s->display) gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    free(s->fb);
    free(s->ram);
    free(s);
}

void ibm5150_run(Ibm5150State *s, const X86Config *cfg) {
    printf("gemu-x86: IBM PC 5150\n"
           "  RAM   : %u KiB\n"
           "  Video : %s\n"
           "  BASIC : %s\n",
           s->ram_size >> 10,
           s->n_heads > 1 ? (s->vga == X86_VGA_MDA ? "MDA (monochrome) + CGA, View menu switches monitors"
                                                   : "CGA + MDA (monochrome), View menu switches monitors")
                          : s->vga == X86_VGA_MDA ? "MDA (monochrome)" : "CGA",
           s->has_basic ? "Cassette BASIC present" : "(none loaded)");
    for (int u = 0; u < 4; u++)
        if (s->fdc.drive[u])
            printf("  %s: %s\n", gemu_drive_name(s->fdc.drive[u]),
                   gemu_drive_inserted(s->fdc.drive[u]) ? gemu_drive_filename(s->fdc.drive[u])
                                                        : "(empty)");

    gemu_monitor_start(s->monitor);

    const uint64_t perf_hz = SDL_GetPerformanceFrequency();
    uint64_t next_frame = SDL_GetPerformanceCounter();
    bool running = true;
    bool reported_halt = false;
    while (running) {
        /* Run until the display adapter starts its next frame (1/60 s of PC
         * time on the CGA, 1/50 s on the MDA). */
        if (!gemu_monitor_is_paused(s->monitor) && !s->halted_for_debug) {
            bool check_bp = gemu_monitor_has_exec_breakpoints(s->monitor);
            for (;;) {
                if (check_bp && !s->cpu.halted &&
                    gemu_monitor_check_exec(s->monitor, x86_linear(s->cpu.cs, s->cpu.ip)))
                    break;
                if (step_cpu(s)) break;
            }
        }

        /* HLT with interrupts off never resumes: POST does this on failure. */
        bool dead = s->cpu.halted && !(s->cpu.flags & X86_FLAG_IF);
        if (dead && !reported_halt && !s->halted_for_debug)
            fprintf(stderr, "ibm5150: CPU halted with interrupts disabled at %04x:%04x\n",
                    s->cpu.cs, (uint16_t)(s->cpu.ip - 1));
        reported_halt = dead;

        if (s->display) {
            gemu_display_poll(s->display);
            scan_keyboard(s);
            video_present(s);
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor) || dead);
            if (gemu_display_should_quit(s->display)) running = false;
            if (gemu_display_reset_requested(s->display)) {
                ibm5150_reset(s);
                gemu_display_clear_flags(s->display);
            }

            /* Pace to the adapter's frame rate (CGA 59.92 Hz, MDA 49.8 Hz). */
            uint32_t frame_clocks = s->vga == X86_VGA_MDA ? MDA_CLOCKS_PER_FRAME
                                                         : CGA_CLOCKS_PER_FRAME;
            next_frame += perf_hz * frame_clocks / IBM5150_CPU_HZ;
            uint64_t now = SDL_GetPerformanceCounter();
            if (next_frame > now) {
                uint64_t ms = (next_frame - now) * 1000 / perf_hz;
                if (ms) gemu_sleep_ms((unsigned)ms);
            } else if (now - next_frame > perf_hz / 4) {
                next_frame = now;   /* fell far behind (e.g. paused): resync */
            }
        }
        /* headless (-display none): deliberately unthrottled while running -
         * see machine_i2000.c's Sleep(0) history before adding a sleep. */
        else if (gemu_monitor_is_paused(s->monitor) || s->halted_for_debug || dead)
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
                for (uint32_t k = gemu_monitor_step_count(s->monitor); k; k--) step_cpu(s);
                break;
            default: break;
            }
        }
    }
    gemu_monitor_stop(s->monitor);
}
