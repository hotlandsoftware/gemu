#include "m6532.h"
#include <string.h>

/* Cycles after the timer write at which it underflows to $FF. */
static uint64_t underflow_at(const M6532 *r) {
    return ((uint64_t)r->timer_value + 1u) << r->timer_shift;
}

uint8_t m6532_timer(const M6532 *r, uint64_t cycle) {
    uint64_t d = cycle - r->timer_start;
    uint64_t u = underflow_at(r);
    if (d < u) return (uint8_t)(r->timer_value - (d >> r->timer_shift));
    return (uint8_t)(0xFFu - ((d - u) & 0xFFu));
}

/* Has the timer reached $FF since the flag was last cleared?  After the
 * first underflow it passes $FF again every 256 cycles. */
static bool timer_flag(const M6532 *r, uint64_t cycle) {
    uint64_t d = cycle - r->timer_start;
    uint64_t c = r->flag_cleared > r->timer_start ? r->flag_cleared - r->timer_start : 0;
    uint64_t u = underflow_at(r);
    if (d < u) return false;
    if (c < u) return true;
    uint64_t next = u + ((c - u) / 256u + 1u) * 256u;
    return next <= d;
}

static uint8_t port_a(const M6532 *r) {
    uint8_t pins = r->read_pa ? r->read_pa(r->ud) : 0xFFu;
    return (uint8_t)((r->ora & r->ddra) | (pins & ~r->ddra)) & pins;
}

static uint8_t port_b(const M6532 *r) {
    uint8_t pins = r->read_pb ? r->read_pb(r->ud) : 0xFFu;
    return (uint8_t)((r->orb & r->ddrb) | (pins & ~r->ddrb));
}

uint8_t m6532_peek(const M6532 *r, uint16_t addr, uint64_t cycle) {
    if (!(addr & 0x04u)) {
        switch (addr & 0x03u) {
        case 0:  return port_a(r);
        case 1:  return r->ddra;
        case 2:  return port_b(r);
        default: return r->ddrb;
        }
    }
    if (!(addr & 0x01u)) return m6532_timer(r, cycle);
    return (uint8_t)((timer_flag(r, cycle) ? 0x80u : 0) | (r->pa7_flag ? 0x40u : 0));
}

uint8_t m6532_read(M6532 *r, uint16_t addr, uint64_t cycle) {
    uint8_t v = m6532_peek(r, addr, cycle);
    if (addr & 0x04u) {
        if (!(addr & 0x01u)) r->flag_cleared = cycle;   /* reading the timer */
        else                 r->pa7_flag = false;       /* reading the flags */
    }
    return v;
}

void m6532_write(M6532 *r, uint16_t addr, uint8_t val, uint64_t cycle) {
    if (!(addr & 0x04u)) {
        switch (addr & 0x03u) {
        case 0: r->ora  = val; break;
        case 1: r->ddra = val; break;
        case 2: r->orb  = val; break;
        case 3: r->ddrb = val; break;
        }
        return;
    }
    if (addr & 0x10u) {
        static const uint8_t shift[4] = { 0, 3, 6, 10 };
        r->timer_start  = cycle;
        r->timer_value  = val;
        r->timer_shift  = shift[addr & 0x03u];
        r->flag_cleared = cycle;
    } else {
        r->edge_ctrl = addr & 0x03u;
    }
}

void m6532_reset(M6532 *r, uint64_t cycle) {
    r->ora = r->ddra = r->orb = r->ddrb = 0;
    r->pa7_flag = false;
    r->edge_ctrl = 0;
    /* The timer runs from power-on; programs always set it before use. */
    r->timer_start = cycle;
    r->timer_value = 0;
    r->timer_shift = 10;
    r->flag_cleared = cycle;
}

void m6532_init(M6532 *r) {
    memset(r, 0, sizeof(*r));
    m6532_reset(r, 0);
}
