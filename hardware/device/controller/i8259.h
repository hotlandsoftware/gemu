#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8259A Programmable Interrupt Controller: one chip (the 5150/5160's,
 * ports 0x20-0x21, IRQ0-7), or the AT-style cascaded pair below.
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
 * low->high transition makes a request; in level mode a high line does.
 * Either way the request goes away if the line drops before it is
 * acknowledged (an acknowledge then finds nothing: a spurious IRQ7). */
void    i8259_set_irq(I8259 *p, unsigned line, bool level);

/* CPU-facing INTR/INTA: whether an unmasked request outranks everything in
 * service, and the acknowledge cycle that returns its vector and marks it
 * in service (IRQ7's vector if the request vanished: a spurious
 * interrupt). */
bool    i8259_intr(const I8259 *p);
uint8_t i8259_ack(I8259 *p);

/* Cascaded pair, as in the PC/AT and its successors: the slave's INT drives
 * the master's IRQ2, the master at ports 0x20-0x21 and the slave at
 * 0xA0-0xA1.  IRQs are numbered 0-15 (8-15 on the slave); IRQ2 is the
 * cascade and can't be driven directly. */
typedef struct {
    I8259 master, slave;
} I8259Pair;

void i8259_pair_reset(I8259Pair *p);
/* Ports 0x20, 0x21, 0xA0, 0xA1; false for any other port. */
bool i8259_pair_io_read(I8259Pair *p, uint16_t port, uint8_t *val);
bool i8259_pair_io_write(I8259Pair *p, uint16_t port, uint8_t val);
void i8259_pair_set_irq(I8259Pair *p, unsigned irq, bool level);
/* The master's INT output. */
bool i8259_pair_intr(const I8259Pair *p);
/* The INTA cycles: the vector, from the slave's base for IRQs 8-15.  *irq is
 * the IRQ acknowledged (0-15), or -1 for a spurious acknowledge (IRQ7's
 * vector of whichever chip found nothing). */
uint8_t i8259_pair_ack(I8259Pair *p, int *irq);
