/* 6821 PIA: see pia6821.h. */
#include "controller/pia6821.h"
#include <string.h>

#define CR_C1_IRQ   0x01        /* C1 flag drives IRQ */
#define CR_C1_RISE  0x02        /* C1 active edge: rising */
#define CR_OR       0x04        /* offset 0/2 is the output register */
#define CR_C2_B3    0x08        /* C2 input: IRQ enable; output: see header */
#define CR_C2_B4    0x10        /* C2 input: rising edge; output: manual */
#define CR_C2_OUT   0x20
#define CR_IRQ2     0x40
#define CR_IRQ1     0x80

void pia6821_init(Pia6821 *p) {
    Pia6821 wiring = *p;
    memset(p, 0, sizeof(*p));
    p->read_pa = wiring.read_pa;
    p->read_pb = wiring.read_pb;
    p->write_pa = wiring.write_pa;
    p->write_pb = wiring.write_pb;
    p->write_ca2 = wiring.write_ca2;
    p->write_cb2 = wiring.write_cb2;
    p->set_irq_a = wiring.set_irq_a;
    p->set_irq_b = wiring.set_irq_b;
    p->ud = wiring.ud;
    p->irq_ud = wiring.irq_ud;
}

/* One side's state, so the A and B logic is written once. */
typedef struct {
    uint8_t *cr;
    bool    *c1, *c2;
    bool    *irq;
    void   (*write_c2)(void *ud, bool level);
    void   (*set_irq)(void *ud, bool level);
} Side;

static Side side(Pia6821 *p, bool b) {
    return b ? (Side){ &p->crb, &p->cb1, &p->cb2, &p->irq_b, p->write_cb2, p->set_irq_b }
             : (Side){ &p->cra, &p->ca1, &p->ca2, &p->irq_a, p->write_ca2, p->set_irq_a };
}

static void update_irq(Pia6821 *p, Side s) {
    uint8_t cr = *s.cr;
    bool irq = ((cr & CR_IRQ1) && (cr & CR_C1_IRQ)) ||
               ((cr & CR_IRQ2) && (cr & CR_C2_B3) && !(cr & CR_C2_OUT));
    if (irq != *s.irq) {
        *s.irq = irq;
        if (s.set_irq) s.set_irq(p->irq_ud ? p->irq_ud : p->ud, irq);
    }
}

/* Drive C2 as an output; `force` reports the level even if unchanged (C2
 * has just become an output). */
static void drive_c2_force(Pia6821 *p, Side s, bool level, bool force) {
    if (*s.c2 == level && !force) return;
    *s.c2 = level;
    if (s.write_c2) s.write_c2(p->ud, level);
}
static void drive_c2(Pia6821 *p, Side s, bool level) {
    drive_c2_force(p, s, level, false);
}

/* The data register was accessed (A: read, B: write): a handshake output
 * goes low, and a pulse comes straight back. */
static void strobe(Pia6821 *p, Side s) {
    uint8_t cr = *s.cr;
    if ((cr & (CR_C2_OUT | CR_C2_B4)) != CR_C2_OUT) return;
    drive_c2(p, s, false);
    if (cr & CR_C2_B3) drive_c2(p, s, true);
}

static void set_c1(Pia6821 *p, Side s, bool level) {
    bool was = *s.c1;
    *s.c1 = level;
    if (was == level) return;
    uint8_t cr = *s.cr;
    if (level != !!(cr & CR_C1_RISE)) return;           /* not the active edge */
    *s.cr |= CR_IRQ1;
    /* Handshake mode 0: the active C1 edge restores C2. */
    if ((cr & (CR_C2_OUT | CR_C2_B4 | CR_C2_B3)) == CR_C2_OUT)
        drive_c2(p, s, true);
    update_irq(p, s);
}

static void set_c2(Pia6821 *p, Side s, bool level) {
    if (*s.cr & CR_C2_OUT) return;                      /* it's an output */
    bool was = *s.c2;
    *s.c2 = level;
    if (was == level || level != !!(*s.cr & CR_C2_B4)) return;
    *s.cr |= CR_IRQ2;
    update_irq(p, s);
}

static void write_cr(Pia6821 *p, Side s, uint8_t val) {
    bool was_out = (*s.cr & CR_C2_OUT) != 0;
    *s.cr = (uint8_t)((*s.cr & (CR_IRQ1 | CR_IRQ2)) | (val & 0x3F));
    if (val & CR_C2_OUT) {
        bool level = (val & CR_C2_B4) ? (val & CR_C2_B3) != 0   /* manual */
                                      : true;                   /* handshake idles high */
        drive_c2_force(p, s, level, !was_out);
    }
    update_irq(p, s);
}

uint8_t pia6821_peek(const Pia6821 *p, unsigned reg) {
    switch (reg & 3) {
    case 0: {
        if (!(p->cra & CR_OR)) return p->ddra;
        uint8_t pin = p->read_pa ? p->read_pa(p->ud) : 0;
        return (uint8_t)((p->ora & p->ddra) | (pin & ~p->ddra));
    }
    case 1:
        return p->cra;
    case 2: {
        if (!(p->crb & CR_OR)) return p->ddrb;
        uint8_t pin = p->read_pb ? p->read_pb(p->ud) : 0;
        return (uint8_t)((p->orb & p->ddrb) | (pin & ~p->ddrb));
    }
    default:
        return p->crb;
    }
}

uint8_t pia6821_read(Pia6821 *p, unsigned reg) {
    uint8_t v = pia6821_peek(p, reg);
    if ((reg & 1) == 0) {
        bool b = (reg & 2) != 0;
        Side s = side(p, b);
        if (*s.cr & CR_OR) {
            *s.cr &= (uint8_t)~(CR_IRQ1 | CR_IRQ2);
            update_irq(p, s);
            if (!b) strobe(p, s);                       /* port A: read strobe */
        }
    }
    return v;
}

void pia6821_write(Pia6821 *p, unsigned reg, uint8_t val) {
    switch (reg & 3) {
    case 0:
        if (p->cra & CR_OR) {
            p->ora = val;
            if (p->write_pa) p->write_pa(p->ud, (uint8_t)(p->ora & p->ddra));
        } else {
            p->ddra = val;
        }
        break;
    case 1:
        write_cr(p, side(p, false), val);
        break;
    case 2:
        if (p->crb & CR_OR) {
            p->orb = val;
            if (p->write_pb) p->write_pb(p->ud, (uint8_t)(p->orb & p->ddrb));
            strobe(p, side(p, true));                   /* port B: write strobe */
        } else {
            p->ddrb = val;
        }
        break;
    default:
        write_cr(p, side(p, true), val);
        break;
    }
}

void pia6821_set_ca1(Pia6821 *p, bool level) { set_c1(p, side(p, false), level); }
void pia6821_set_cb1(Pia6821 *p, bool level) { set_c1(p, side(p, true), level); }
void pia6821_set_ca2(Pia6821 *p, bool level) { set_c2(p, side(p, false), level); }
void pia6821_set_cb2(Pia6821 *p, bool level) { set_c2(p, side(p, true), level); }
