#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Motorola/Synertek 6821 PIA (Peripheral Interface Adapter).
 *
 * Two 8-bit ports, A and B, each with a data direction register, an output
 * register and a control register, at offsets 0-3: ORA/DDRA, CRA, ORB/DDRB,
 * CRB (CR bit 2 selects the output register rather than the DDR).
 *
 * Each side also has two control lines.  C1 (CA1/CB1) is an input: its
 * active edge (CR bit 1: 1 rising, 0 falling) sets IRQ1, CR bit 7.  C2
 * (CA2/CB2) is set up by CR bits 3-5:
 *   bit 5 = 0  input: its active edge (bit 4) sets IRQ2, CR bit 6;
 *              bit 3 enables that flag onto IRQ
 *   bit 5 = 1, bit 4 = 0  handshake output.  Port A: goes low when ORA is
 *              read; port B: when ORB is written.  Restored by the next
 *              active C1 edge (bit 3 = 0) or after one cycle (bit 3 = 1,
 *              a pulse)
 *   bit 5 = 1, bit 4 = 1  output set to bit 3
 * The flags are read-only; reading the side's output register clears
 * them.  IRQA/IRQB are asserted while IRQ1 is set with CR bit 0, or IRQ2
 * with CR bit 3 (C2 as input).
 *
 * Port pins: a read returns output bits for pins programmed as outputs and
 * the read_pa/read_pb callback's value for inputs.  write_pa/write_pb get
 * the output register masked to the output pins when it's written.
 */

typedef struct Pia6821 {
    uint8_t ora, ddra, cra;
    uint8_t orb, ddrb, crb;
    /* control-line levels: inputs as last driven, C2 outputs as driven */
    bool ca1, ca2, cb1, cb2;
    bool irq_a, irq_b;                  /* the IRQ outputs as last reported */

    /* Wiring (kept by pia6821_init).  Any member may be NULL: inputs read 0
     * and outputs go nowhere. */
    uint8_t (*read_pa)(void *ud);
    uint8_t (*read_pb)(void *ud);
    void (*write_pa)(void *ud, uint8_t val);
    void (*write_pb)(void *ud, uint8_t val);
    void (*write_ca2)(void *ud, bool level);   /* CA2 as an output */
    void (*write_cb2)(void *ud, bool level);
    void (*set_irq_a)(void *ud, bool level);   /* IRQA, active high here */
    void (*set_irq_b)(void *ud, bool level);
    void *ud;
} Pia6821;

/* Reset: everything cleared (ports and lines as inputs), wiring kept. */
void    pia6821_init(Pia6821 *p);
uint8_t pia6821_read(Pia6821 *p, unsigned reg);        /* reg = addr & 3 */
void    pia6821_write(Pia6821 *p, unsigned reg, uint8_t val);
/* A read without its side effects (flags, strobes), for debuggers. */
uint8_t pia6821_peek(const Pia6821 *p, unsigned reg);

/* Control-line inputs.  CA2/CB2 are only inputs while CR bit 5 is clear. */
void    pia6821_set_ca1(Pia6821 *p, bool level);
void    pia6821_set_ca2(Pia6821 *p, bool level);
void    pia6821_set_cb1(Pia6821 *p, bool level);
void    pia6821_set_cb2(Pia6821 *p, bool level);
