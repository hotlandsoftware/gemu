#include "pia6821.h"
#include <string.h>

void pia6821_init(Pia6821 *p) {
    void *ud = p->ud;
    uint8_t (*read_pa)(void *) = p->read_pa;
    uint8_t (*read_pb)(void *) = p->read_pb;
    void (*write_pa)(void *, uint8_t) = p->write_pa;
    void (*write_pb)(void *, uint8_t) = p->write_pb;
    memset(p, 0, sizeof(*p));
    p->ud = ud;
    p->read_pa = read_pa;
    p->read_pb = read_pb;
    p->write_pa = write_pa;
    p->write_pb = write_pb;
}

uint8_t pia6821_read(Pia6821 *p, unsigned reg) {
    switch (reg & 3) {
    case 0: { /* ORA or DDRA, selected by CRA bit 2 */
        if (!(p->cra & 0x04)) return p->ddra;
        uint8_t pin = p->read_pa ? p->read_pa(p->ud) : 0;
        return (uint8_t)((p->ora & p->ddra) | (pin & ~p->ddra));
    }
    case 1:
        return p->cra;
    case 2: { /* ORB or DDRB, selected by CRB bit 2 */
        if (!(p->crb & 0x04)) return p->ddrb;
        uint8_t pin = p->read_pb ? p->read_pb(p->ud) : 0;
        return (uint8_t)((p->orb & p->ddrb) | (pin & ~p->ddrb));
    }
    default:
        return p->crb;
    }
}

void pia6821_write(Pia6821 *p, unsigned reg, uint8_t val) {
    switch (reg & 3) {
    case 0:
        if (p->cra & 0x04) {
            p->ora = val;
            if (p->write_pa) p->write_pa(p->ud, (uint8_t)(p->ora & p->ddra));
        } else {
            p->ddra = val;
        }
        break;
    case 1:
        p->cra = val;
        break;
    case 2:
        if (p->crb & 0x04) {
            p->orb = val;
            if (p->write_pb) p->write_pb(p->ud, (uint8_t)(p->orb & p->ddrb));
        } else {
            p->ddrb = val;
        }
        break;
    default:
        p->crb = val;
        break;
    }
}
