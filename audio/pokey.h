#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * POKEY (Atari C012294) - keyboard / IRQ / random-number model.
 *
 * Scope: everything the Atari 400/800 OS needs to boot and take keyboard
 * input - KBCODE + keyboard/BREAK IRQs, IRQEN/IRQST semantics, the 17-bit
 * polynomial RANDOM register, SKSTAT shift/key-down bits, and just enough
 * serial-port behavior (SEROUT raises one-shot ready/complete IRQs) and
 * coarse timer IRQs for SIO's disk-boot attempt to run to its timeout instead
 * of hanging.  No audio synthesis yet (AUDF/AUDC are stored, not played) and
 * no paddle (POT) scanning.
 */

/* IRQEN / IRQST bits */
#define POKEY_IRQ_TIMER1   0x01
#define POKEY_IRQ_TIMER2   0x02
#define POKEY_IRQ_TIMER4   0x04
#define POKEY_IRQ_SEROC    0x08   /* serial output complete */
#define POKEY_IRQ_SEROR    0x10   /* serial output register ready */
#define POKEY_IRQ_SERIN    0x20   /* serial input ready */
#define POKEY_IRQ_KEY      0x40   /* keyboard scan match */
#define POKEY_IRQ_BREAK    0x80   /* BREAK key */

typedef struct Pokey {
    uint8_t audf[4], audc[4], audctl;
    uint8_t irqen;
    uint8_t irq_pending;   /* raw pending sources (1 = pending) */
    uint8_t skctl, skstat;
    uint8_t kbcode;
    uint8_t serout;
    uint8_t serout_delay;
    uint16_t timer_count[3]; /* IRQ timers 1, 2 and 4 */
    uint32_t poly17;       /* RANDOM LFSR state */
} Pokey;

void    pokey_init(Pokey *p);
uint8_t pokey_read (Pokey *p, uint8_t reg);   /* reg = addr & 0x0F */
void    pokey_write(Pokey *p, uint8_t reg, uint8_t v);

/* Feed a keypress: code = 6-bit keyboard scan code | 0x40 shift | 0x80 ctrl. */
void pokey_key_down(Pokey *p, uint8_t code);
void pokey_key_up  (Pokey *p);
void pokey_break_key(Pokey *p);

/* Call periodically (e.g. once per scanline): advances serial-complete delay
 * and the coarse IRQ timers used by the OS timeout paths. */
void pokey_tick(Pokey *p);

static inline bool pokey_irq_asserted(const Pokey *p) {
    return (p->irq_pending & p->irqen) != 0;
}
