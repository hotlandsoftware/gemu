#include "pokey.h"
#include <string.h>

void pokey_init(Pokey *p) {
    memset(p, 0, sizeof(*p));
    p->skstat = 0xFF;      /* all inputs idle (active low) */
    p->poly17 = 0x1FFFF;
}

static uint8_t poly17_step(Pokey *p) {
    /* 17-bit LFSR, taps 17 and 12 (XNOR form used by POKEY). Step a full
     * byte's worth of bits per read - callers only care that it varies. */
    uint32_t s = p->poly17;
    for (int i = 0; i < 8; i++) {
        uint32_t bit = (~((s >> 16) ^ (s >> 11))) & 1u;
        s = ((s << 1) | bit) & 0x1FFFFu;
    }
    p->poly17 = s;
    return (uint8_t)s;
}

static uint16_t timer_reload(const Pokey *p, int timer) {
    int idx = (timer == 2) ? 3 : timer;
    uint16_t period = (uint16_t)p->audf[idx] + 1u;
    return period < 2u ? 2u : period;
}

static void reload_timers(Pokey *p) {
    p->timer_count[0] = timer_reload(p, 0);
    p->timer_count[1] = timer_reload(p, 1);
    p->timer_count[2] = timer_reload(p, 2);
}

uint8_t pokey_read(Pokey *p, uint8_t reg) {
    switch (reg & 0x0F) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
        return 228;                        /* POT0-7: paddles absent */
    case 0x08: return 0xFF;                /* ALLPOT: all absent pots complete */
    case 0x09: return p->kbcode;           /* KBCODE */
    case 0x0A: return poly17_step(p);      /* RANDOM */
    case 0x0D: return 0;                   /* SERIN - line silent */
    case 0x0E: return (uint8_t)~p->irq_pending;  /* IRQST, active low */
    case 0x0F: return p->skstat;           /* SKSTAT */
    default:   return 0xFF;
    }
}

void pokey_write(Pokey *p, uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
    case 0x00: case 0x02: case 0x04: case 0x06:
        p->audf[(reg & 0x0F) >> 1] = v; break;
    case 0x01: case 0x03: case 0x05: case 0x07:
        p->audc[(reg & 0x0F) >> 1] = v; break;
    case 0x08: p->audctl = v; break;
    case 0x09: reload_timers(p); break;    /* STIMER */
    case 0x0A: p->skstat |= 0xE0; break;   /* SKRES */
    case 0x0B: break;                      /* POTGO */
    case 0x0D:                             /* SEROUT - no attached SIO target */
        p->serout = v;
        p->irq_pending &= (uint8_t)~POKEY_IRQ_SEROC;
        p->irq_pending |= POKEY_IRQ_SEROR;
        p->serout_delay = 4;
        break;
    case 0x0E:                             /* IRQEN: 0-bits also clear pending */
        p->irqen = v;
        p->irq_pending &= v;
        break;
    case 0x0F: p->skctl = v; break;
    default: break;
    }
}

void pokey_tick(Pokey *p) {
    /* With no SIO device attached, SEROUT immediately raises one-shot ready
     * and complete sources. Do not re-arm SEROR here; the OS has a real
     * vector for that source and will spin if it is held pending forever. */
    if (p->serout_delay && --p->serout_delay == 0)
        p->irq_pending |= POKEY_IRQ_SEROC;

    static const uint8_t irq_bits[3] = {
        POKEY_IRQ_TIMER1, POKEY_IRQ_TIMER2, POKEY_IRQ_TIMER4
    };
    for (int i = 0; i < 3; i++) {
        if (!p->timer_count[i]) p->timer_count[i] = timer_reload(p, i);
        if (--p->timer_count[i] == 0) {
            p->irq_pending |= irq_bits[i];
            p->timer_count[i] = timer_reload(p, i);
        }
    }
}

void pokey_key_down(Pokey *p, uint8_t code) {
    p->kbcode = code;
    p->irq_pending |= POKEY_IRQ_KEY;
    p->skstat &= (uint8_t)~0x04;           /* key currently pressed */
    if (code & 0x40) p->skstat &= (uint8_t)~0x08;  /* shift held */
}

void pokey_key_up(Pokey *p) {
    p->skstat |= 0x04 | 0x08;
}

void pokey_break_key(Pokey *p) {
    p->irq_pending |= POKEY_IRQ_BREAK;
}
