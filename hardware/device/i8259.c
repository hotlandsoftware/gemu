#include "i8259.h"
#include <string.h>

void i8259_reset(I8259 *p) {
    memset(p, 0, sizeof *p);
    p->imr = 0xFF; /* all lines masked until the BIOS programs it */
}

/* Highest-priority (lowest-numbered) bit set in v, or 8 if none. */
static unsigned highest(uint8_t v) {
    for (unsigned i = 0; i < 8; i++)
        if (v & (1u << i)) return i;
    return 8;
}

void i8259_io_write(I8259 *p, uint16_t port, uint8_t val) {
    if ((port & 1) == 0) {
        if (val & 0x10) {                         /* ICW1 */
            p->level_mode  = (val & 0x08) != 0;
            p->single      = (val & 0x02) != 0;
            p->expect_icw4 = (val & 0x01) != 0;
            p->imr = 0;
            p->isr = 0;
            p->irr = p->level_mode ? p->lines : 0;
            p->auto_eoi = false;
            p->read_isr = p->poll = false;
            p->icw_step = 2;
            return;
        }
        if (val & 0x08) {                         /* OCW3 */
            if (val & 0x02) p->read_isr = (val & 0x01) != 0;
            p->poll = (val & 0x04) != 0;
            return;
        }
        /* OCW2: EOI family (rotation commands are accepted as plain EOIs) */
        switch (val >> 5) {
        case 1: case 5: {                         /* non-specific EOI */
            unsigned l = highest(p->isr);
            if (l < 8) p->isr = (uint8_t)(p->isr & ~(1u << l));
            break;
        }
        case 3: case 7:                           /* specific EOI */
            p->isr = (uint8_t)(p->isr & ~(1u << (val & 7)));
            break;
        default:
            break;
        }
        return;
    }

    switch (p->icw_step) {
    case 2:                                       /* ICW2 */
        p->vector_base = (uint8_t)(val & 0xF8);
        p->icw_step = !p->single ? 3 : p->expect_icw4 ? 4 : 0;
        return;
    case 3:                                       /* ICW3 (cascade wiring) */
        p->icw_step = p->expect_icw4 ? 4 : 0;
        return;
    case 4:                                       /* ICW4 */
        p->auto_eoi = (val & 0x02) != 0;
        p->icw_step = 0;
        return;
    default:                                      /* OCW1 */
        p->imr = val;
        return;
    }
}

/* The request that would be delivered next, or 8 if none can be: the
 * highest-priority unmasked request, provided nothing of equal or higher
 * priority is in service. */
static unsigned deliverable(const I8259 *p) {
    unsigned req = highest((uint8_t)(p->irr & ~p->imr));
    if (req == 8 || highest(p->isr) <= req) return 8;
    return req;
}

static uint8_t acknowledge(I8259 *p, unsigned line) {
    p->irr = (uint8_t)(p->irr & ~(1u << line));
    if (!p->auto_eoi) p->isr = (uint8_t)(p->isr | (1u << line));
    if (p->level_mode && (p->lines & (1u << line)))
        p->irr = (uint8_t)(p->irr | (1u << line));
    return (uint8_t)line;
}

uint8_t i8259_io_read(I8259 *p, uint16_t port) {
    if ((port & 1) == 0) {
        if (p->poll) {                            /* poll: ack + report */
            p->poll = false;
            unsigned l = deliverable(p);
            if (l == 8) return 0;
            return (uint8_t)(0x80 | acknowledge(p, l));
        }
        return p->read_isr ? p->isr : p->irr;
    }
    return p->imr;
}

void i8259_set_irq(I8259 *p, unsigned line, bool level) {
    if (line >= 8) return;
    uint8_t bit = (uint8_t)(1u << line);
    bool was = (p->lines & bit) != 0;
    if (level) p->lines |= bit; else p->lines = (uint8_t)(p->lines & ~bit);
    if (p->level_mode) {
        if (level) p->irr |= bit; else p->irr = (uint8_t)(p->irr & ~bit);
    } else if (level && !was) {
        p->irr |= bit;
    }
}

bool i8259_intr(const I8259 *p) {
    return deliverable(p) < 8;
}

uint8_t i8259_ack(I8259 *p) {
    unsigned l = deliverable(p);
    if (l == 8) return (uint8_t)(p->vector_base + 7);   /* spurious */
    return (uint8_t)(p->vector_base + acknowledge(p, l));
}
