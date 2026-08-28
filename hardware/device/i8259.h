#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8259 Programmable Interrupt Controller. The 5150/5160 have a single
 * PIC (ports 0x20-0x21, 8 lines, IRQ0-7) - no slave/cascade until the AT's
 * second 8259, which is a later machine's problem. Simplified: real IRQ
 * masking/priority/EOI, but no special fully-nested/rotating/polled modes
 * beyond what PC BIOS/DOS-era software actually exercises. */

typedef struct {
    uint8_t imr;   /* interrupt mask register (OCW1) */
    uint8_t irr;   /* interrupt request register - device-asserted lines */
    uint8_t isr;   /* in-service register */
    uint8_t vector_base; /* ICW2: vector for IRQ0 (IRQn -> vector_base+n) */
    bool    auto_eoi;    /* ICW4 bit 1 */

    /* Initialization Command Word sequencing */
    unsigned icw_step;   /* 0 = idle/ready, 1..3 = expecting ICW2/3/4 */
    bool     expect_icw4;
} I8259;

void    i8259_reset(I8259 *p);
uint8_t i8259_io_read(I8259 *p, uint16_t port);
void    i8259_io_write(I8259 *p, uint16_t port, uint8_t val);

/* Device-facing edge/level interface: a peripheral calls _raise when its
 * IRQ line goes active and _lower when it clears (PIT/keyboard are
 * effectively edge-triggered in practice - raise then immediately allow
 * the next raise once serviced). */
void i8259_raise_irq(I8259 *p, unsigned line);
void i8259_lower_irq(I8259 *p, unsigned line);

/* Called once per CPU-facing poll: returns the interrupt vector to deliver
 * and marks it in-service, or -1 if nothing is currently deliverable
 * (masked, or a higher/equal-priority line already in service). Priority
 * is fixed IRQ0 (highest) .. IRQ7 (lowest), matching PC/XT wiring. */
int i8259_pending_vector(I8259 *p);
