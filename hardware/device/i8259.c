#include "i8259.h"
#include <string.h>

void i8259_reset(I8259 *p) {
    memset(p, 0, sizeof *p);
    p->imr = 0xFF; /* all lines masked until the BIOS programs it */
}

void i8259_io_write(I8259 *p, uint16_t port, uint8_t val) {
    if ((port & 1) == 0) {
        if (val & 0x10) { /* ICW1: starts (re)initialization */
            p->icw_step = 1;
            p->expect_icw4 = (val & 0x01) != 0;
            p->irr = p->isr = 0;
            p->imr = 0;
            return;
        }
        if (val & 0x08) return; /* OCW3 (read register select / poll) - reads handled in io_read */
        /* OCW2: EOI family. Only the common "non-specific EOI" (0x20) and
         * "specific EOI" (0x60 | level) forms are implemented - the
         * rotating-priority variants aren't something PC BIOS/DOS-era code
         * exercises. */
        if (val & 0x20) {
            if (val & 0x40) { /* specific EOI: level in bits 2:0 */
                p->isr = (uint8_t)(p->isr & ~(1u << (val & 7)));
            } else if (p->isr) { /* non-specific: clear highest-priority in-service bit */
                for (unsigned i = 0; i < 8; i++) {
                    if (p->isr & (1u << i)) { p->isr = (uint8_t)(p->isr & ~(1u << i)); break; }
                }
            }
        }
        return;
    }
    /* port 0x21 */
    if (p->icw_step == 1) { p->icw_step = 2; return; } /* ICW2 arrives as vector_base below */
    if (p->icw_step == 2) {
        p->vector_base = (uint8_t)(val & 0xF8);
        p->icw_step = p->expect_icw4 ? 3 : 0;
        return;
    }
    if (p->icw_step == 3) { p->icw_step = 0; return; } /* ICW4 - auto_eoi bit unused by real PC BIOS; ignore */
    p->imr = val; /* OCW1 */
}

uint8_t i8259_io_read(I8259 *p, uint16_t port) {
    if ((port & 1) == 0) return p->irr; /* OCW3 register-read defaults to IRR, matches typical BIOS usage */
    return p->imr;
}

void i8259_raise_irq(I8259 *p, unsigned line) {
    if (line < 8) p->irr = (uint8_t)(p->irr | (1u << line));
}
void i8259_lower_irq(I8259 *p, unsigned line) {
    if (line < 8) p->irr = (uint8_t)(p->irr & ~(1u << line));
}

int i8259_pending_vector(I8259 *p) {
    uint8_t requested = (uint8_t)(p->irr & ~p->imr);
    if (!requested) return -1;
    for (unsigned i = 0; i < 8; i++) {
        if (!(requested & (1u << i))) continue;
        /* A higher-or-equal priority line already in service blocks this one. */
        uint8_t higher_mask = (uint8_t)((1u << i) - 1u);
        if (p->isr & higher_mask) return -1;
        if (p->isr & (1u << i)) return -1;
        p->isr = (uint8_t)(p->isr | (1u << i));
        p->irr = (uint8_t)(p->irr & ~(1u << i)); /* edge-style: clear request once accepted */
        return p->vector_base + (int)i;
    }
    return -1;
}
