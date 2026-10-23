#pragma once
#include "x86.h"
#include "i8237.h"
#include "i8259.h"
#include "i8253.h"
#include "i8255.h"
#include "upd765.h"
#include "cga.h"
#include "mda.h"
#include "pcspk.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include <stdlib.h>

/* ── IBM PC 5150 (1981) machine state ───────────────────────────────────── */

#define IBM5150_RAM_MAX   0xA0000u /* 640K conventional RAM ceiling */
#define IBM5150_BIOS_SIZE 0x2000u  /* 8K, mapped at 0xFE000-0xFFFFF */
#define IBM5150_BASIC_SIZE 0x8000u /* 32K Cassette BASIC, 0xF6000-0xFDFFF */
#define IBM5150_CPU_HZ    4772727u /* 14.31818 MHz / 3 */

/* Distinct "first unhandled access to this port" log entries, mirroring
 * machine_i2000.c's mmio_log() - bounded so a scanning loop can't spam. */
#define IBM5150_IOLOG_N 256

typedef struct { uint16_t port; bool is_write; uint8_t val; unsigned count; } Ibm5150IoLogEnt;

typedef struct Ibm5150State {
    X86Cpu cpu;
    uint64_t cycles;       /* CPU clocks since power-on */
    unsigned pit_phase;    /* CPU clocks not yet turned into PIT clocks */

    uint8_t *ram;       /* ram_size bytes, conventional memory */
    uint32_t ram_size;
    uint8_t  bios[IBM5150_BIOS_SIZE];
    uint8_t  basic[IBM5150_BASIC_SIZE];
    bool     has_basic;

    I8237     dma;
    I8259     pic;
    I8253     pit;
    I8255     ppi;
    X86VgaType vga;       /* which display adapter is installed (-vga) */
    CgaDevice cga;
    MdaDevice mda;
    int       fb_w, fb_h; /* the adapter's framebuffer size */
    Upd765    fdc;        /* diskette adapter, present when -drive if=floppy is */
    bool      has_fdc;
    uint8_t   nmi_mask;  /* port 0xA0 */

    Ibm5150IoLogEnt iolog[IBM5150_IOLOG_N];
    int             iolog_n;

    /* Keyboard: host keys become XT scan codes queued here, then shifted
     * into the PPI one at a time while the keyboard is enabled (PB7 low,
     * clock high), raising IRQ1.  Holding the clock low resets it, after
     * which it answers 0xAA. */
    uint8_t  kbq[512];
    int      kbq_len;
    bool     kb_full;        /* a scan code is waiting in the PPI */
    uint64_t kb_clock_low_at;
    uint64_t kb_reset_at;    /* when to send 0xAA (0 = no reset pending) */
    uint64_t kb_next_at;     /* next byte can't arrive before this */
    bool     key_down[128];

    GemuMonitor  *monitor;
    GemuDisplay  *display;
    RcaPcSpeaker *speaker;
    uint32_t     *fb; /* fb_w x fb_h ARGB, rendered from the adapter each frame */

    bool halted_for_debug; /* set on an unimplemented/trapped opcode - keeps
                             * the machine parked so the monitor can inspect
                             * state instead of spinning on garbage */
} Ibm5150State;

Ibm5150State *ibm5150_create(const X86Config *cfg);
void          ibm5150_destroy(Ibm5150State *s);
void          ibm5150_run(Ibm5150State *s, const X86Config *cfg);
