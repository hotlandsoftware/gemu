#pragma once
/*
 * MOS 6532 RIOT - 128 bytes of RAM, two 8-bit I/O ports and an interval
 * timer (the Atari 2600's PIA).
 *
 * Register selection follows the chip's pins (the caller passes the CPU
 * address; RS = RAM select is decided by the caller):
 *   A2=0:        port A data, A DDR, port B data, B DDR   (A1..A0)
 *   A2=1, read:  A0=0 timer, A0=1 interrupt flags          (A3 = IRQ enable)
 *   A2=1, write: A4=1 timer with interval 1/8/64/1024 (A1..A0),
 *                A4=0 PA7 edge-detect control
 *
 * The timer is evaluated lazily from the CPU cycle count: after a write of
 * N it counts down once every interval cycles, then on underflow sets the
 * timer flag and keeps counting from $FF once per cycle until written
 * again.  Reading the timer clears the flag.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct M6532 {
    uint8_t  ram[128];
    uint8_t  ora, ddra, orb, ddrb;   /* output registers and directions */

    uint64_t timer_start;    /* cycle of the last timer write */
    uint8_t  timer_value;    /* value written */
    uint8_t  timer_shift;    /* log2 of the interval */
    uint64_t flag_cleared;   /* cycle the timer flag was last cleared */
    bool     pa7_flag;
    uint8_t  edge_ctrl;

    /* Pin levels driven by whatever is plugged in (pull-ups = 1). */
    uint8_t (*read_pa)(void *ud);
    uint8_t (*read_pb)(void *ud);
    void    *ud;
} M6532;

void    m6532_init(M6532 *r);
void    m6532_reset(M6532 *r, uint64_t cycle);
uint8_t m6532_read(M6532 *r, uint16_t addr, uint64_t cycle);
void    m6532_write(M6532 *r, uint16_t addr, uint8_t val, uint64_t cycle);
/* Side-effect-free register read for the debugger. */
uint8_t m6532_peek(const M6532 *r, uint16_t addr, uint64_t cycle);
uint8_t m6532_timer(const M6532 *r, uint64_t cycle);
