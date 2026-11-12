#pragma once
/*
 * National INS8250 UART, as on the IBM Asynchronous Communications Adapter
 * (COM1 at 3F8h/IRQ 4, COM2 at 2F8h/IRQ 3).
 *
 *   +0  RBR (R) / THR (W), or the divisor's low byte with LCR bit 7 (DLAB)
 *   +1  IER: 0 received data, 1 THR empty, 2 line status, 3 modem status
 *       (the divisor's high byte with DLAB)
 *   +2  IIR (R): bit 0 clear when an interrupt is pending, bits 1-2 which:
 *       3 line status, 2 received data, 1 THR empty, 0 modem status
 *   +3  LCR   +4  MCR: DTR, RTS, OUT1, OUT2, loopback
 *   +5  LSR: DR, OE, PE, FE, BI, THRE, TEMT
 *   +6  MSR: delta CTS/DSR/RI/DCD, then CTS, DSR, RI, DCD
 *   +7  no register on the 8250 (the 16450 added a scratch register there)
 *
 * The adapter routes the interrupt through OUT2.  Characters move at the
 * rate the divisor and LCR give (1.8432 MHz crystal, x16 clock): a byte
 * written to THR reaches the device a character time later, and the device
 * is asked for its next byte each time the receiver is free, so a byte
 * the program doesn't read in time is overrun as on the chip.  Bytes pass
 * whatever the format; framing and parity errors aren't modelled.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct Ins8250 {
    /* Wiring, set before ins8250_reset() */
    uint32_t clock_hz;                          /* what ins8250_tick() counts */
    void (*set_irq)(void *ud, bool level);
    void *ud;
    /* The device on the port: NULL members for none */
    void (*dev_tx)(void *dev, uint8_t b);       /* a byte the PC sent */
    void (*dev_lines)(void *dev, bool dtr, bool rts);   /* the MCR outputs changed */
    bool (*dev_rx)(void *dev, uint8_t *b);      /* the device's next byte, if it has one */
    void *dev;

    /* Registers */
    uint8_t  rbr, thr, ier, lcr, mcr, lsr, msr;
    uint16_t divisor;
    bool     thre_int;      /* THR-empty interrupt pending (cleared by reading IIR) */

    /* Shift registers */
    bool     tx_busy;
    uint8_t  tsr;
    uint32_t tx_wait;       /* clocks until the TSR's byte is sent */
    bool     rx_busy;
    uint8_t  rsr;
    uint32_t rx_wait;       /* clocks until the RSR's byte is in */
    bool     loop_pending;  /* loopback: a byte on its way from TSR to RSR */
    uint8_t  loop_byte;
} Ins8250;

void    ins8250_reset(Ins8250 *u);
uint8_t ins8250_read(Ins8250 *u, unsigned reg);        /* reg 0-7 */
void    ins8250_write(Ins8250 *u, unsigned reg, uint8_t val);
void    ins8250_tick(Ins8250 *u, uint32_t clocks);
