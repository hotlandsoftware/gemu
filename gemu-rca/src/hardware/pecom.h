#pragma once
#include "cdp1802.h"
#include "cdp1869.h"
#include "rca.h"
#include "devices/pcspk.h"
#include "gemu/monitor.h"
#include "gemu/vnc.h"
#include "gemu/gemu_display.h"
#include <stdlib.h>

/* ── Pecom 32/64 machine state ───────────────────────────────────────────── */

/*
 * Pecom 32 memory map:
 *   0x0000–0x7FFF  RAM bank 1 (32 KB)         — ram1[]
 *   0x8000–0xBFFF  ROM (16 KB, read-only)      — rom[0x0000-0x3FFF]
 *   0xC000–0xF3FF  RAM bank 2 (13 KB)          — ram2[]
 *   0xF400–0xF7FF  VIS-1870 char RAM           — cdp1869 char RAM
 *   0xF800–0xFFFF  VIS-1870 page RAM           — cdp1869 page RAM
 *
 * Pecom 64 memory map:
 *   0x0000–0x7FFF  RAM (32 KB)                 — ram1[]
 *   0x8000–0xBFFF  ROM chip 1 (16 KB)          — rom[0x0000-0x3FFF]
 *   0xC000–0xF3FF  ROM chip 2 (13 KB visible)  — rom[0x4000-0x73FF]
 *   0xF400–0xF7FF  VIS-1870 char RAM           — cdp1869 char RAM
 *   0xF800–0xFFFF  VIS-1870 page RAM           — cdp1869 page RAM
 *
 * Bootstrap: ROM also appears at 0x0000–0x3FFF on reset until OUT 1 fires.
 * rom_size discriminates variants: <= 0x4000 = Pecom 32, > 0x4000 = Pecom 64.
 */
#define PECOM32_ROM_SIZE      0x4000u   /* 16 KB — Pecom 32 ROM / first chip */
#define PECOM64_ROM_SIZE      0x8000u   /* 32 KB — Pecom 64 ROM (two chips)  */
#define PECOM32_RAM1_SIZE     0x8000u   /* 32 KB: 0x0000–0x7FFF */
#define PECOM32_RAM2_SIZE     0x4000u   /* 16 KB: 0xC000–0xFFFF (Pecom 32 only) */
#define PECOM32_CRAM_BASE     0xF400u   /* VIS-1870 char RAM start */
#define PECOM32_PRAM_BASE     0xF800u   /* VIS-1870 page RAM start */

#define PECOM32_FRAME_HZ      50u
/* CDP1869 PAL timing: 312 lines × 14 machine cycles per line */
#define PECOM32_MCYCLES_PER_FRAME  CDP1869_MCYCLES_PER_FRAME

typedef struct RcaPecom32State {
    Cdp1802          cpu;
    Cdp1869          vis;
    const RcaConfig *cfg;

    uint8_t  rom[PECOM64_ROM_SIZE];   /* 32 KB buffer; Pecom 32 uses first 16 KB */
    uint32_t rom_size;                 /* actual loaded ROM bytes: <=0x4000=P32, >0x4000=P64 */
    uint8_t  ram1[PECOM32_RAM1_SIZE];
    uint8_t  ram2[PECOM32_RAM2_SIZE]; /* Pecom 32 only; unused for Pecom 64 */

    bool     boot_mirror;   /* ROM mirrored at 0x0000 until first OUT 1 */
    uint8_t  iogroup;       /* current I/O group: 0=keyboard, 2=VIS-1870 */

    /* ── Keyboard matrix ────────────────────────────────────────────── */
    /* The Pecom 32 uses a matrix keyboard scanned via IN 3.
     * The IN 3 instruction uses the memory address bus (R[X] & 0x3F) as the
     * row selector; the returned byte has bits 0–1 set for each pressed key
     * in that row (active-high: 1 = pressed).
     * EF1 = CTRL (AND with VIS display), EF2 = SHIFT, EF3 = CAPS (pol=rev),
     * EF4 = ESC. */
    uint8_t  keys[64];      /* key matrix read by ROM: live | latch */
    uint8_t  keys_live[64]; /* VNC: current physical state (down=1, up=0) */
    uint8_t  keys_latch[64];/* VNC: set on key-down, cleared each poll — ensures
                             *  a fast tap (down+up in same frame) is held for
                             *  at least one CPU frame so the ROM scan sees it */

    bool     key_shift;
    bool     key_ctrl;
    bool     key_esc;
    bool     caps_locked;

    GemuMonitor  *monitor;
    GemuVncServer *vnc;
    RcaPcSpeaker *speaker;
} RcaPecom32State;

/* ── Public API ──────────────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg);
void             rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg);
void             rca_pecom32_destroy(RcaPecom32State *s);
void             rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg);
