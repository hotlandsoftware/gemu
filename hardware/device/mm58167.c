/*
 * MM58167A real-time clock - see mm58167.h.
 */
#include "mm58167.h"
#include <string.h>

#define CRYSTAL_HZ 32768u

enum { R_MS, R_CS, R_SEC, R_MIN, R_HOUR, R_DOW, R_DAY, R_MONTH };

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void mm58167_init(Mm58167 *r) {
    memset(r, 0, sizeof(*r));
    r->cnt[R_DOW] = r->cnt[R_DAY] = r->cnt[R_MONTH] = 1;
}

void mm58167_set_time(Mm58167 *r, int month, int mday, int wday, int hour, int min, int sec) {
    r->cnt[R_MS] = r->cnt[R_CS] = 0;
    r->cnt[R_SEC] = to_bcd(sec);
    r->cnt[R_MIN] = to_bcd(min);
    r->cnt[R_HOUR] = to_bcd(hour);
    r->cnt[R_DOW] = (uint8_t)wday;
    r->cnt[R_DAY] = to_bcd(mday);
    r->cnt[R_MONTH] = to_bcd(month);
    r->sub = 0;
}

/* A two-digit BCD counter whose top value is `last` (BCD): returns true
 * when it wrapped to `first`.  Digits count independently, the units
 * carrying into the tens only from 9. */
static bool count2(uint8_t *v, uint8_t first, uint8_t last) {
    if (*v == last) { *v = first; return true; }
    uint8_t lo = *v & 0x0Fu, hi = *v >> 4;
    if (lo == 9) { lo = 0; hi = (hi + 1u) & 0x0Fu; }
    else lo = (lo + 1u) & 0x0Fu;
    *v = (uint8_t)(hi << 4 | lo);
    return false;
}

static uint8_t month_last_day(uint8_t month) {
    switch (month) {
    case 0x02: return 0x28;
    case 0x04: case 0x06: case 0x09: case 0x11: return 0x30;
    default: return 0x31;
    }
}

static void raise_int(Mm58167 *r, uint8_t bits) {
    r->int_status |= bits & r->int_control;
    r->int_out = r->int_status != 0;
}

static void compare(Mm58167 *r) {
    for (int i = 0; i < 8; i++) {
        if ((r->ram[i] & 0xC0u) == 0xC0u) continue;   /* don't care */
        if (r->ram[i] != r->cnt[i]) return;
    }
    raise_int(r, 0x01);
}

/* One millisecond. */
static void tick_ms(Mm58167 *r) {
    uint8_t ev = 0;
    uint8_t *c = r->cnt;
    uint8_t ms = c[R_MS] >> 4;
    if (ms != 9) { c[R_MS] = (uint8_t)(((ms + 1u) & 0x0Fu) << 4); compare(r); return; }
    c[R_MS] = 0;
    /* hundredths (low digit) and tenths (high digit) */
    uint8_t lo = c[R_CS] & 0x0Fu, hi = c[R_CS] >> 4;
    bool sec = false;
    if (lo == 9) {
        lo = 0;
        ev |= 0x02;   /* a tenth of a second */
        if (hi == 9) { hi = 0; sec = true; }
        else hi = (hi + 1u) & 0x0Fu;
    } else {
        lo = (lo + 1u) & 0x0Fu;
    }
    c[R_CS] = (uint8_t)(hi << 4 | lo);
    if (sec) {
        ev |= 0x04;
        if (count2(&c[R_SEC], 0x00, 0x59)) {
            ev |= 0x08;
            if (count2(&c[R_MIN], 0x00, 0x59)) {
                ev |= 0x10;
                if (count2(&c[R_HOUR], 0x00, 0x23)) {
                    ev |= 0x20;
                    if (c[R_DOW] >= 7) { c[R_DOW] = 1; ev |= 0x40; }
                    else c[R_DOW] = (uint8_t)((c[R_DOW] + 1u) & 0x0Fu);
                    if (count2(&c[R_DAY], 0x01, month_last_day(c[R_MONTH]))) {
                        ev |= 0x80;
                        count2(&c[R_MONTH], 0x01, 0x12);
                    }
                }
            }
        }
    }
    raise_int(r, ev);
    compare(r);
}

void mm58167_sync(Mm58167 *r, uint64_t cycle, double bus_hz) {
    if (cycle <= r->last_cycle) return;
    /* crystal ticks elapsed, as a whole number of bus cycles' worth */
    uint64_t dc = cycle - r->last_cycle;
    uint64_t ticks = (uint64_t)((double)dc * CRYSTAL_HZ / bus_hz);
    if (!ticks) return;
    r->last_cycle += (uint64_t)((double)ticks * bus_hz / CRYSTAL_HZ);
    /* 1000 ms per 32768 ticks */
    uint64_t acc = r->sub + ticks * 1000u;
    uint64_t ms = acc / CRYSTAL_HZ;
    r->sub = (uint32_t)(acc % CRYSTAL_HZ);
    while (ms--) tick_ms(r);
}

uint8_t mm58167_peek(const Mm58167 *r, uint8_t reg) {
    reg &= 0x1Fu;
    if (reg < 8) return r->cnt[reg];
    if (reg < 16) return r->ram[reg - 8];
    switch (reg) {
    case 0x10: return r->int_status;
    case 0x11: return r->int_control;
    case 0x16: return r->standby_int ? 1 : 0;
    default:   return 0;   /* 14: status bit; write-only registers */
    }
}

uint8_t mm58167_read(Mm58167 *r, uint8_t reg) {
    uint8_t v = mm58167_peek(r, reg);
    if ((reg & 0x1Fu) == 0x10) {
        r->int_status = 0;
        r->int_out = false;
    }
    return v;
}

void mm58167_write(Mm58167 *r, uint8_t reg, uint8_t val) {
    reg &= 0x1Fu;
    if (reg < 8) { r->cnt[reg] = val; return; }
    if (reg < 16) { r->ram[reg - 8] = val; return; }
    switch (reg) {
    case 0x11: r->int_control = val; break;
    case 0x12:
        for (int i = 0; i < 8; i++) if (val & (1u << i)) r->cnt[i] = 0;
        break;
    case 0x13:
        for (int i = 0; i < 8; i++) if (val & (1u << i)) r->ram[i] = 0;
        break;
    case 0x15:   /* GO */
        r->cnt[R_MS] = r->cnt[R_CS] = r->cnt[R_SEC] = 0;
        r->sub = 0;
        break;
    case 0x16: r->standby_int = val & 1u; break;
    default: break;   /* 10, 14: read-only; 1F: test mode (counters fast-clocked) not modelled */
    }
}
