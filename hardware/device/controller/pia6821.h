#pragma once
#include <stdint.h>

/* Motorola/Synertek 6821 PIA (Peripheral Interface Adapter) - minimal
 * register-level model.
 *
 * Covers the ORA/DDRA/CRA + ORB/DDRB/CRB register decode (the "which
 * register does offset 0-3 mean" logic every 6821 has) and the standard
 * DDR-masked read-back behavior. Does NOT model CA1/CA2/CB1/CB2 handshake
 * lines or IRQ flag bits in the control registers - no known GEMU machine
 * needs that yet; add it if one shows up.
 */

typedef struct {
    uint8_t ora, ddra, cra;
    uint8_t orb, ddrb, crb;

    /* Input side: called on a port read when the corresponding DDR bit is
     * 0 (pin configured as input). May be NULL (input pins read as 0). */
    uint8_t (*read_pa)(void *ud);
    uint8_t (*read_pb)(void *ud);

    /* Output side: called whenever ORA/ORB or their DDR changes, with the
     * port value masked to just the pins driven as outputs (reg & ddr).
     * May be NULL. */
    void (*write_pa)(void *ud, uint8_t val);
    void (*write_pb)(void *ud, uint8_t val);

    void *ud;
} Pia6821;

void    pia6821_init(Pia6821 *p);
uint8_t pia6821_read(Pia6821 *p, unsigned reg);        /* reg = addr & 3 */
void    pia6821_write(Pia6821 *p, unsigned reg, uint8_t val);
