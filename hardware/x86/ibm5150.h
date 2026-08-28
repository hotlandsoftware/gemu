#pragma once
#include "x86.h"
#include "i8259.h"
#include "i8253.h"
#include "i8255.h"
#include "cga.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include <stdlib.h>

/* ── IBM PC 5150 (1981) machine state ───────────────────────────────────── */

#define IBM5150_RAM_MAX   0xA0000u /* 640K conventional RAM ceiling */
#define IBM5150_BIOS_SIZE 0x2000u  /* 8K, mapped at 0xFE000-0xFFFFF */
#define IBM5150_BASIC_SIZE 0x8000u /* 32K Cassette BASIC, 0xF6000-0xFDFFF */

/* Distinct "first unhandled access to this port" log entries, mirroring
 * machine_i2000.c's mmio_log() - bounded so a scanning loop can't spam. */
#define IBM5150_IOLOG_N 256

typedef struct { uint16_t port; bool is_write; uint8_t val; unsigned count; } Ibm5150IoLogEnt;

typedef struct Ibm5150State {
    X86Cpu cpu;

    uint8_t *ram;       /* ram_size bytes, conventional memory */
    uint32_t ram_size;
    uint8_t  bios[IBM5150_BIOS_SIZE];
    uint8_t  basic[IBM5150_BASIC_SIZE];
    bool     has_basic;

    I8259     pic;
    I8253     pit;
    I8255     ppi;
    CgaDevice cga;

    Ibm5150IoLogEnt iolog[IBM5150_IOLOG_N];
    int             iolog_n;

    uint8_t kbq[16]; /* pending XT scan codes awaiting the PPI/IRQ1 handshake */
    int     kbq_len;

    GemuMonitor *monitor;
    GemuDisplay *display;
    uint32_t    *fb; /* 640x200 ARGB scratch, rendered from cga each frame */

    bool halted_for_debug; /* set on an unimplemented/trapped opcode - keeps
                             * the machine parked so the monitor can inspect
                             * state instead of spinning on garbage */
} Ibm5150State;

Ibm5150State *ibm5150_create(const X86Config *cfg);
void          ibm5150_destroy(Ibm5150State *s);
void          ibm5150_run(Ibm5150State *s, const X86Config *cfg);
