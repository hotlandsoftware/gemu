#pragma once
/*
 * The cartridge connector as a debugging tool (Debug > Cartridge Pins and
 * the monitor's `cartpin` command): any of the Famicom's 60 contacts can be
 * given a bad contact - open all the time, or flaky (open on a random
 * quarter of the accesses through it) - and the CPU clock (M2) changed.
 *
 * A fault acts where the signal does on real hardware:
 *   - a cartridge input left floating reads as 1 (TTL), so a broken CPU or
 *     PPU address line is stuck high as the cartridge sees it, and a
 *     broken data line is high in what the CPU writes to the mapper;
 *   - the CPU reading through a broken data line gets the bus's last value
 *     (open bus) in that bit; the PPU gets the bit of the address low byte
 *     its multiplexed AD bus still holds;
 *   - /ROMSEL, R/W, M2, PPU /RD and /WR, /IRQ, CIRAM /CE and A10, /A13 and
 *     the sound loop each cut what they carry (see nes_pins.c's table);
 *   - the two ground and two +5V pins are redundant: the cartridge only
 *     loses power when both of a pair are open.
 *
 * Tilting the cartridge (nes_pins_set_tilt) pivots it on the end that
 * stays seated: the contacts toward the end that lifts lose wipe, first
 * going flaky, then open.  Those faults add to the ones set by hand.
 *
 * Pins are numbered as on the Famicom connector (1-30 on one side,
 * 31-60 on the other); the NES's 72-pin slot carries the same signals.
 */
#include <stdbool.h>
#include <stdint.h>

#define NES_PINS_N 60
#define NES_PIN_BIT(n) (1ull << ((n) - 1))

/* The pins the emulation acts on, by number. */
enum {
    NP_GND_A = 1, NP_CPU_RW = 14, NP_IRQ = 15, NP_GND_B = 16,
    NP_PPU_RD = 17, NP_CIRAM_A10 = 18, NP_VCC_A = 30, NP_VCC_B = 31,
    NP_M2 = 32, NP_ROMSEL = 44, NP_SOUND_IN = 45, NP_SOUND_OUT = 46,
    NP_PPU_WR = 47, NP_CIRAM_CE = 48, NP_PPU_A13N = 49, NP_PPU_A12 = 55,
};

typedef enum { NES_PIN_GOOD, NES_PIN_BROKEN, NES_PIN_FLAKY } NesPinState;

typedef struct {
    const char *label;     /* as printed by the connector, e.g. "CPU A3" */
    const char *what;      /* one line: the signal, and what losing it does */
} NesPinInfo;

extern const NesPinInfo nes_pin_info[NES_PINS_N + 1];   /* [1..60] */

#define NES_PIN_TILT_MAX 5.0    /* degrees either way */

typedef struct NesPins {
    uint64_t broken, flaky;     /* set by hand; bit n-1 = pin n */
    double   tilt_deg;          /* + lifts the pin 30/60 end, - the pin 1/31 end */
    uint64_t tilt_broken, tilt_flaky;   /* what the tilt adds */
    uint32_t rng;
    unsigned version;           /* bumped on every change */

    /* M2: the CPU clock (the PPU and APU follow it) */
    uint32_t clock_hz, clock_default;

    /* Test points: set when the line was asserted since the last look */
    bool     tp_nmi, tp_apu_irq, tp_map_irq;
} NesPins;

void        nes_pins_init(NesPins *p, uint32_t clock_hz);
void        nes_pins_reinsert(NesPins *p);           /* seated straight, every contact good */
NesPinState nes_pins_get(const NesPins *p, int pin);       /* as it is now, tilt included */
NesPinState nes_pins_get_user(const NesPins *p, int pin);  /* as set by hand */
void        nes_pins_set(NesPins *p, int pin, NesPinState st);
void        nes_pins_set_tilt(NesPins *p, double deg);
double      nes_pins_lift_mm(const NesPins *p, int pin);   /* how far the tilt raised it */
static inline uint64_t nes_pins_open_mask(const NesPins *p)  { return p->broken | p->tilt_broken; }
static inline uint64_t nes_pins_flaky_mask(const NesPins *p) {
    return (p->flaky | p->tilt_flaky) & ~nes_pins_open_mask(p);
}
static inline bool nes_pins_any(const NesPins *p) {
    return (p->broken | p->flaky | p->tilt_broken | p->tilt_flaky) != 0;
}

/* The contacts open for one access: the broken ones, and each flaky one
 * a quarter of the time. */
uint64_t nes_pins_sample(NesPins *p);

/* Decoding an open-pin mask */
bool     nes_pins_dead(uint64_t open);               /* no ground or no +5V */
uint16_t nes_pins_cpu_addr_open(uint64_t open);      /* CPU A0-A14 */
uint8_t  nes_pins_cpu_data_open(uint64_t open);      /* CPU D0-D7 */
uint16_t nes_pins_ppu_addr_open(uint64_t open);      /* PPU A0-A13 */
uint8_t  nes_pins_ppu_data_open(uint64_t open);      /* PPU D0-D7 */

/* A pin by number ("32") or label ("m2", "cpu_a3", "ppu/rd"); 0 if none. */
int      nes_pins_find(const char *name);
