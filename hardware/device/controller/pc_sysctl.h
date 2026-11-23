#pragma once
/*
 * The AT-style system control port and POST port of a PC-compatible
 * chipset.
 *
 *   61h  system control port B.  Bits 0-3 are a latch: bit 0 gates 8254
 *        channel 2, bit 1 enables the speaker, bits 2-3 are the parity and
 *        I/O-check enables.  Reads add bit 4, the memory-refresh toggle
 *        (it flips every 18 timer clocks, about 15 us), and bit 5, channel
 *        2's output.
 *   80h  the POST code: firmware writes its progress here; the last value
 *        is kept for a diagnostic display.
 *
 * The 5150's port 61h is its 8255's port B instead (controller/i8255.h).
 */
#include "timer/i8253.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    I8253  *pit;            /* wiring: channel 2's gate and output */
    uint8_t port_b;         /* 61h bits 0-3 */
    uint8_t post;           /* last byte written to 80h */
} PcSysctl;

void pc_sysctl_init(PcSysctl *s, I8253 *pit);
/* Latch and POST code cleared; channel 2 ungated. */
void pc_sysctl_reset(PcSysctl *s);
/* pit_clocks: timer input clocks so far, for the refresh toggle.  Bring
 * the timer up to date first: bit 5 is its channel 2's output now. */
bool pc_sysctl_io_read(const PcSysctl *s, uint16_t port, uint64_t pit_clocks,
                       uint8_t *val);
bool pc_sysctl_io_write(PcSysctl *s, uint16_t port, uint8_t val);
