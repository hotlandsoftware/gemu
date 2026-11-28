/* 6530 RRIOT: see m6530.h. */
#include "controller/m6530.h"

void m6530_reset(M6530 *r) {
    r->pa = r->pb = r->ddra = r->ddrb = 0;
    r->reload = 0;
    r->prescale = 1;
    r->irq_en = r->running = r->irq_flag = false;
    r->next_fire = 0;
    if (r->irq_changed) r->irq_changed(r->irq_ud ? r->irq_ud : r->ud);
}

bool m6530_irq(const M6530 *r) {
    return r->irq_flag && r->irq_en;
}

static void set_flag(M6530 *r, bool flag) {
    if (r->irq_flag == flag) return;
    r->irq_flag = flag;
    if (r->irq_changed) r->irq_changed(r->irq_ud ? r->irq_ud : r->ud);
}

/* The live count: from the value loaded down to 0 (the underflow), then
 * on down from $FF. */
static uint8_t count(const M6530 *r, uint64_t cycle) {
    if (!r->running) return r->reload;
    if (cycle < r->next_fire)
        return (uint8_t)((r->next_fire - cycle - 1u) / r->prescale);
    return (uint8_t)(-(uint8_t)((cycle - r->next_fire) / r->prescale));
}

void m6530_tick(M6530 *r, uint64_t cycle) {
    if (!r->running || r->irq_flag || cycle < r->next_fire) return;
    set_flag(r, true);
}

uint8_t m6530_peek(const M6530 *r, unsigned reg, uint64_t cycle) {
    switch (reg & 0x0F) {
    case 0x0: {
        uint8_t pins = r->read_pa ? r->read_pa(r->ud) : 0xFF;
        return (uint8_t)((r->pa & r->ddra) | (pins & ~r->ddra));
    }
    case 0x1: return r->ddra;
    case 0x2: {
        uint8_t pins = r->read_pb ? r->read_pb(r->ud) : 0xFF;
        return (uint8_t)((r->pb & r->ddrb) | (pins & ~r->ddrb));
    }
    case 0x3: return r->ddrb;
    case 0x4: case 0x5: case 0xC: case 0xD:
        return count(r, cycle);
    case 0x6: case 0x7: case 0xE: case 0xF:
        return (uint8_t)((r->irq_flag ? 0x80 : 0) | (count(r, cycle) & 0x7F));
    default:
        return 0;
    }
}

uint8_t m6530_read(M6530 *r, unsigned reg, uint64_t cycle) {
    uint8_t v = m6530_peek(r, reg, cycle);
    switch (reg & 0x0F) {
    case 0x6: case 0x7: case 0xE: case 0xF:
        set_flag(r, false);
        break;
    }
    return v;
}

void m6530_write(M6530 *r, unsigned reg, uint8_t val, uint64_t cycle) {
    static const uint16_t prescales[4] = { 1, 8, 64, 1024 };
    switch (reg & 0x0F) {
    case 0x0:
        r->pa = val;
        if (r->write_pa) r->write_pa(r->ud, val);
        break;
    case 0x1: r->ddra = val; break;
    case 0x2:
        r->pb = val;
        if (r->write_pb) r->write_pb(r->ud, val);
        break;
    case 0x3: r->ddrb = val; break;
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0xC: case 0xD: case 0xE: case 0xF:
        r->reload = val;
        r->prescale = prescales[reg & 3];
        r->irq_en = (reg & 0x08) != 0;
        r->running = true;
        r->next_fire = cycle + (uint64_t)(val ? val : 256u) * r->prescale;
        r->irq_flag = false;
        if (r->irq_changed) r->irq_changed(r->irq_ud ? r->irq_ud : r->ud);   /* flag and irq_en both changed */
        break;
    default:
        break;
    }
}
