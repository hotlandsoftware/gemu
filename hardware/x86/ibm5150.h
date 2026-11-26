#pragma once
#include "x86.h"
#include "controller/i8237.h"
#include "controller/i8259.h"
#include "timer/i8253.h"
#include "controller/i8255.h"
#include "storage/upd765.h"
#include "storage/ibm_hdc.h"
#include "serial/ins8250.h"
#include "input/msmouse.h"
#include "cga.h"
#include "mda.h"
#include "hercules.h"
#include "gemu/audio_out.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/space.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include <stdlib.h>

/* ── IBM PC 5150 (1981) machine state ───────────────────────────────────── */

#define IBM5150_RAM_MAX   0xA0000u /* 640K conventional RAM ceiling */
#define IBM5150_BIOS_SIZE 0x2000u  /* 8K, mapped at 0xFE000-0xFFFFF */
#define IBM5150_BASIC_SIZE 0x8000u /* 32K Cassette BASIC, 0xF6000-0xFDFFF */
#define IBM5150_HDC_ROM_BASE 0xC8000u /* fixed disk adapter BIOS */
#define IBM5150_HDC_ROM_MAX  0x2000u
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

    /* The machine as a core device tree (stage D2): board/{dma,pic,pit,ppi,
     * kbd}, isa/{fdc,hdc,com1,com2}; the chips' state lives in the devices,
     * these point at it. */
    GemuMachine *machine;
    GemuSpace   *io;        /* ports: A0-A9 decoded (a mirror of 0-3FFh) */
    GemuSpace   *bus;       /* the memory bus as DMA sees it */
    GemuDevice  *dev_cpu, *dev_dma, *dev_pic, *dev_pit, *dev_ppi, *dev_kbd, *dev_speaker;
    GemuExecutor *cpu_exec; /* the 8088 on the scheduler (stage D3) */
    uint64_t     run_done;  /* CPU clocks in the executor's current run */
    uint64_t     steps_left;/* monitor "step N": instructions still to run */
    bool         check_bp;  /* this run checks exec breakpoints */
    GemuDevice  *dev_fdc, *dev_hdc, *dev_com[2];
    GemuLine    *kb_irq;    /* the keyboard's IRQ 1 */
    I8237     *dma;
    I8259     *pic;
    I8253     *pit;
    I8255     *ppi;
    uint8_t    sw1, sw2;    /* the DIP switches, worked out from the configuration */
    uint8_t    port_b;      /* port B as last written (the keyboard's edge detection) */
    X86VgaType vga;       /* the primary display adapter (-vga): POST uses it */
    bool      has_cga, has_mda; /* installed cards: -vga plus an optional
                                 * second one (-device), each on its own monitor */
    CgaDevice *cga;       /* the cards' state lives in their devices (stage D4) */
    MdaDevice *mda;
    HerculesDevice *herc;
    GemuDevice *dev_cga, *dev_mda, *dev_herc;
    bool      has_herc;
    int       n_heads;    /* monitors: 1, or 2 with both cards */
    X86VgaType head_card[2]; /* card shown on each monitor, primary first */
    const char *head_opts[2]; /* each card's options (-vga ..., -device ...) */
    GemuScreen *head_screen[2]; /* its screen */
    Upd765   *fdc;        /* diskette adapter, present when -drive if=floppy is */
    GemuDrive *floppy[4];
    bool      has_fdc;
    IbmHdc   *hdc;        /* fixed disk adapter, -device ibm-fixed-disk */
    bool      has_hdc;
    uint8_t   hdc_rom[IBM5150_HDC_ROM_MAX];
    uint32_t  hdc_rom_size;
    uint32_t  hdc_rom_mapped;   /* bytes on the bus at C8000h: 0 with rom=off */
    uint8_t   hdc_irq;          /* IRQ jumper: 5 (IBM's setting) or 2 */
    Ins8250  *com[2];     /* asynchronous communications adapters: COM1, COM2 */
    bool      has_com[2];
    MsMouse   mouse;      /* -device msmouse, on one of them */
    bool      has_mouse;
    uint8_t   mouse_buttons; /* set by the monitor's mouse_button */
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
    GemuAudioOut *audio;   /* the mixer's output (with a display) */

    bool halted_for_debug; /* set on an unimplemented/trapped opcode - keeps
                             * the machine parked so the monitor can inspect
                             * state instead of spinning on garbage */
} Ibm5150State;

Ibm5150State *ibm5150_create(const X86Config *cfg);
void          ibm5150_destroy(Ibm5150State *s);
void          ibm5150_run(Ibm5150State *s, const X86Config *cfg);
