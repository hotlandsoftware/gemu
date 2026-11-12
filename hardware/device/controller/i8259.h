#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8259A Programmable Interrupt Controller. The 5150/5160 have a single
 * PIC (ports 0x20-0x21, 8 lines, IRQ0-7) - no slave/cascade until the AT's
 * second 8259, which is a later machine's problem.
 *
 * Modelled: ICW1-4 initialization, edge- and level-triggered requests,
 * masking, fixed priority with the in-service register, specific and
 * non-specific EOI, automatic EOI, IRR/ISR readback and poll mode via
 * OCW3, and spurious IRQ7 on an acknowledge with nothing pending.
 * Rotating priority and special mask mode aren't (no PC software of the
 * era depends on them). */

typedef struct {
    uint8_t imr;   /* interrupt mask register (OCW1) */
    uint8_t irr;   /* interrupt request register */
    uint8_t isr;   /* in-service register */
    uint8_t lines; /* current input line levels */
    uint8_t vector_base; /* ICW2: vector for IRQ0 (IRQn -> vector_base+n) */
    bool    auto_eoi;    /* ICW4 bit 1 */
    bool    level_mode;  /* ICW1 LTIM */
    bool    read_isr;    /* OCW3 RR/RIS: next read of port 0x20 returns ISR */
    bool    poll;        /* OCW3 P: next read of port 0x20 is a poll */

    /* Initialization Command Word sequencing */
    unsigned icw_step;   /* 0 = ready, 2/3/4 = expecting that ICW */
    bool     single;     /* ICW1 SNGL: no ICW3 */
    bool     expect_icw4;
} I8259;

void    i8259_reset(I8259 *p);
uint8_t i8259_io_read(I8259 *p, uint16_t port);
void    i8259_io_write(I8259 *p, uint16_t port, uint8_t val);

/* Device-facing interrupt line.  In edge mode (what the PC BIOS selects) a
 * request is latched on a low->high transition; in level mode it follows
 * the line. */
void    i8259_set_irq(I8259 *p, unsigned line, bool level);

/* CPU-facing INTR/INTA: whether an unmasked request outranks everything in
 * service, and the acknowledge cycle that returns its vector and marks it
 * in service (IRQ7's vector if the request vanished: a spurious
 * interrupt). */
bool    i8259_intr(const I8259 *p);
uint8_t i8259_ack(I8259 *p);
