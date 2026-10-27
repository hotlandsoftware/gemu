#pragma once
#include "z80.h"
#include "z80cfg.h"
#include "gemu/monitor.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Generic Z80 machine: 64 KB of flat RAM, ROM images overlaid read-only
 * where -rom puts them, and a few I/O ports (the same peripherals as the
 * generic 6502 machine, in I/O space):
 *
 *   00h  Timer reload low byte     (R/W)
 *   01h  Timer reload high byte    (R/W)
 *   02h  Timer control             (R/W)
 *          bit 0  enable (1=counting)
 *          bit 1  interrupt enable
 *          bit 2  auto-reload on expiry (0=disable on expiry)
 *          bit 7  interrupt pending - write 1 to clear, read to test
 *   10h  Serial data               (W=TX to stdout, R=RX from stdin)
 *   11h  Serial status             (R)
 *          bit 0  RX byte available
 *          bit 1  TX ready (always 1)
 *   12h  Serial control            (R/W)
 *          bit 0  RX interrupt enable
 *
 * The timer counts T-states; reload 0 means 65536.  Interrupts drive /INT
 * with FFh on the data bus: RST 38h in IM 0 and 1, vector I:FFh in IM 2.
 *
 * -cpm FILE runs a CP/M .COM program (zexdoc, zexall, ...): it loads at
 * 0100h, with enough of the BDOS for console output (functions 2 and 9)
 * and a warm boot (jump to 0000h) that ends the run.
 */

#define Z80GEN_PORT_BDOS 0xFEu   /* -cpm: the BDOS entry's OUT */
#define Z80GEN_PORT_EXIT 0xFFu   /* -cpm: warm boot */

typedef struct Z80GenericState {
    Z80              cpu;
    const Z80Config *cfg;
    uint8_t          mem[0x10000];
    uint8_t          rom_map[0x10000];   /* 1 = read-only */

    uint16_t timer_reload;
    uint8_t  timer_ctrl;
    bool     timer_pending;
    uint64_t timer_next_fire;

    uint8_t  serial_ctrl;
    uint8_t  serial_rxq[16];
    uint8_t  serial_rxq_r, serial_rxq_w, serial_rxq_cnt;

    bool     exit_requested;   /* -cpm warm boot */
    uint64_t insn_count;

    GemuMonitor *monitor;
} Z80GenericState;

Z80GenericState *z80_generic_create (const Z80Config *cfg);
void             z80_generic_destroy(Z80GenericState *s);
void             z80_generic_run    (Z80GenericState *s, const Z80Config *cfg);
