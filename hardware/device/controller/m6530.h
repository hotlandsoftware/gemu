#pragma once
/*
 * MOS 6530 RRIOT (ROM-RAM-I/O-Timer), I/O, timer and RAM parts: two 8-bit
 * ports with direction registers, an 8-bit interval timer and 64 bytes of
 * RAM.  The chip's 1 KB mask ROM is the machine's (its contents and where
 * it decodes are board-specific).
 *
 * I/O and timer registers, by A3..A0 (the caller decodes the chip select):
 *   0 PA   1 DDRA   2 PB   3 DDRB
 *   writes 4-7: load the timer and start it, prescaler /1, /8, /64, /1024
 *          (A1..A0); A3 set (C-F) also lets its underflow drive IRQ
 *   reads 4, 5, C, D: the count, no side effects
 *   reads 6, 7, E, F: bit 7 the timer flag, bits 6-0 the count; clears the
 *          flag
 * This is the decoding the KIM-1's software relies on.
 *
 * The timer counts from the value loaded down to 0 once per prescaler
 * period, sets the flag at the underflow, then keeps counting down from
 * $FF once per period.  It is evaluated from the caller's cycle count;
 * m6530_tick() notices an underflow between accesses.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct M6530 {
    uint8_t  ram[64];
    uint8_t  pa, pb;            /* output registers as written */
    uint8_t  ddra, ddrb;
    uint8_t  reload;            /* the value loaded */
    uint16_t prescale;          /* 1, 8, 64 or 1024 */
    bool     irq_en;            /* loaded through C-F: underflow drives IRQ */
    bool     running;
    bool     irq_flag;
    uint64_t next_fire;         /* cycle of the underflow */

    /* Wiring (kept by m6530_reset).  Pins read 1 (pulled up) without a
     * read callback.  The write callbacks get the register as written. */
    uint8_t (*read_pa)(void *ud);
    uint8_t (*read_pb)(void *ud);
    void    (*write_pa)(void *ud, uint8_t val);
    void    (*write_pb)(void *ud, uint8_t val);
    void    (*irq_changed)(void *ud);   /* m6530_irq() may have changed */
    void    *ud;
} M6530;

/* Reset: registers, timer and flag cleared (prescaler /1), wiring and RAM
 * kept. */
void    m6530_reset(M6530 *r);
uint8_t m6530_read(M6530 *r, unsigned reg, uint64_t cycle);
void    m6530_write(M6530 *r, unsigned reg, uint8_t val, uint64_t cycle);
/* The register's value without side effects (the flag isn't cleared). */
uint8_t m6530_peek(const M6530 *r, unsigned reg, uint64_t cycle);
/* Notice a timer underflow since the last access. */
void    m6530_tick(M6530 *r, uint64_t cycle);
/* The IRQ output: the timer flag, when loaded through C-F. */
bool    m6530_irq(const M6530 *r);
