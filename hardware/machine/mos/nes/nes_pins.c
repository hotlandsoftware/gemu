/*
 * Cartridge connector faults - see nes_pins.h.
 */
#include "nes_pins.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const NesPinInfo nes_pin_info[NES_PINS_N + 1] = {
    [0]  = { "", "" },
    [1]  = { "GND",        "Ground (with pin 16): the cartridge dies only if both are open" },
    [2]  = { "CPU A11",    "CPU address bit 11: the cartridge sees it stuck high" },
    [3]  = { "CPU A10",    "CPU address bit 10: the cartridge sees it stuck high" },
    [4]  = { "CPU A9",     "CPU address bit 9: the cartridge sees it stuck high" },
    [5]  = { "CPU A8",     "CPU address bit 8: the cartridge sees it stuck high" },
    [6]  = { "CPU A7",     "CPU address bit 7: the cartridge sees it stuck high" },
    [7]  = { "CPU A6",     "CPU address bit 6: the cartridge sees it stuck high" },
    [8]  = { "CPU A5",     "CPU address bit 5: the cartridge sees it stuck high" },
    [9]  = { "CPU A4",     "CPU address bit 4: the cartridge sees it stuck high" },
    [10] = { "CPU A3",     "CPU address bit 3: the cartridge sees it stuck high" },
    [11] = { "CPU A2",     "CPU address bit 2: the cartridge sees it stuck high" },
    [12] = { "CPU A1",     "CPU address bit 1: the cartridge sees it stuck high" },
    [13] = { "CPU A0",     "CPU address bit 0: the cartridge sees it stuck high" },
    [14] = { "CPU R/W",    "Read/write: the cartridge sees only reads (mapper and PRG-RAM writes lost)" },
    [15] = { "/IRQ",       "Mapper interrupt to the CPU: scanline and timer IRQs never arrive" },
    [16] = { "GND",        "Ground (with pin 1): the cartridge dies only if both are open" },
    [17] = { "PPU /RD",    "PPU read strobe: CHR ROM/RAM never answers (tiles read the address bus)" },
    [18] = { "CIRAM A10",  "Nametable select from the cart: floats high, one screen shown everywhere" },
    [19] = { "PPU A6",     "PPU address bit 6: the cartridge sees it stuck high" },
    [20] = { "PPU A5",     "PPU address bit 5: the cartridge sees it stuck high" },
    [21] = { "PPU A4",     "PPU address bit 4: the cartridge sees it stuck high" },
    [22] = { "PPU A3",     "PPU address bit 3: the cartridge sees it stuck high" },
    [23] = { "PPU A2",     "PPU address bit 2: the cartridge sees it stuck high" },
    [24] = { "PPU A1",     "PPU address bit 1: the cartridge sees it stuck high" },
    [25] = { "PPU A0",     "PPU address bit 0: the cartridge sees it stuck high" },
    [26] = { "PPU D0",     "PPU data bit 0: CHR reads get the address bus's bit instead" },
    [27] = { "PPU D1",     "PPU data bit 1: CHR reads get the address bus's bit instead" },
    [28] = { "PPU D2",     "PPU data bit 2: CHR reads get the address bus's bit instead" },
    [29] = { "PPU D3",     "PPU data bit 3: CHR reads get the address bus's bit instead" },
    [30] = { "+5V",        "Power (with pin 31): the cartridge dies only if both are open" },
    [31] = { "+5V",        "Power (with pin 30): the cartridge dies only if both are open" },
    [32] = { "M2",         "CPU clock phase 2: PRG-RAM and $4020-$7FFF go dead, mapper timers stop" },
    [33] = { "CPU A12",    "CPU address bit 12: the cartridge sees it stuck high" },
    [34] = { "CPU A13",    "CPU address bit 13: the cartridge sees it stuck high" },
    [35] = { "CPU A14",    "CPU address bit 14: the cartridge sees it stuck high" },
    [36] = { "CPU D7",     "CPU data bit 7: reads get open bus there, mapper writes see a 1" },
    [37] = { "CPU D6",     "CPU data bit 6: reads get open bus there, mapper writes see a 1" },
    [38] = { "CPU D5",     "CPU data bit 5: reads get open bus there, mapper writes see a 1" },
    [39] = { "CPU D4",     "CPU data bit 4: reads get open bus there, mapper writes see a 1" },
    [40] = { "CPU D3",     "CPU data bit 3: reads get open bus there, mapper writes see a 1" },
    [41] = { "CPU D2",     "CPU data bit 2: reads get open bus there, mapper writes see a 1" },
    [42] = { "CPU D1",     "CPU data bit 1: reads get open bus there, mapper writes see a 1" },
    [43] = { "CPU D0",     "CPU data bit 0: reads get open bus there, mapper writes see a 1" },
    [44] = { "/ROMSEL",    "PRG ROM select ($8000-$FFFF): the program reads open bus and crashes" },
    [45] = { "SOUND IN",   "Console audio into the cart's mixer: only expansion audio is heard" },
    [46] = { "SOUND OUT",  "Mixed audio back to the console: silence" },
    [47] = { "PPU /WR",    "PPU write strobe: CHR-RAM writes are lost" },
    [48] = { "CIRAM /CE",  "Nametable RAM enable from the cart: floats, background reads garbage" },
    [49] = { "PPU /A13",   "Inverted PPU A13, wired to CIRAM /CE on most boards: nametables off" },
    [50] = { "PPU A7",     "PPU address bit 7: the cartridge sees it stuck high" },
    [51] = { "PPU A8",     "PPU address bit 8: the cartridge sees it stuck high" },
    [52] = { "PPU A9",     "PPU address bit 9: the cartridge sees it stuck high" },
    [53] = { "PPU A10",    "PPU address bit 10: stuck high (H/V mirroring follows it)" },
    [54] = { "PPU A11",    "PPU address bit 11: stuck high (H/V mirroring follows it)" },
    [55] = { "PPU A12",    "PPU address bit 12: stuck high; MMC3-style scanline IRQs stop" },
    [56] = { "PPU A13",    "PPU address bit 13: the cartridge never enables CHR" },
    [57] = { "PPU D7",     "PPU data bit 7: CHR reads get the address bus's bit instead" },
    [58] = { "PPU D6",     "PPU data bit 6: CHR reads get the address bus's bit instead" },
    [59] = { "PPU D5",     "PPU data bit 5: CHR reads get the address bus's bit instead" },
    [60] = { "PPU D4",     "PPU data bit 4: CHR reads get the address bus's bit instead" },
};

static const uint8_t cpu_a_pin[15] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 33, 34, 35 };
static const uint8_t cpu_d_pin[8]  = { 43, 42, 41, 40, 39, 38, 37, 36 };
static const uint8_t ppu_a_pin[14] = { 25, 24, 23, 22, 21, 20, 19, 50, 51, 52, 53, 54, 55, 56 };
static const uint8_t ppu_d_pin[8]  = { 26, 27, 28, 29, 60, 59, 58, 57 };

void nes_pins_init(NesPins *p, uint32_t clock_hz) {
    memset(p, 0, sizeof(*p));
    p->rng = 0x9E3779B9u;
    p->clock_hz = p->clock_default = clock_hz;
}

void nes_pins_reinsert(NesPins *p) {
    p->broken = p->flaky = 0;
    nes_pins_set_tilt(p, 0.0);
    p->version++;
}

static NesPinState state_in(uint64_t open, uint64_t flaky, int pin) {
    if (pin < 1 || pin > NES_PINS_N) return NES_PIN_GOOD;
    if (open & NES_PIN_BIT(pin)) return NES_PIN_BROKEN;
    if (flaky & NES_PIN_BIT(pin)) return NES_PIN_FLAKY;
    return NES_PIN_GOOD;
}

NesPinState nes_pins_get(const NesPins *p, int pin) {
    return state_in(nes_pins_open_mask(p), nes_pins_flaky_mask(p), pin);
}

NesPinState nes_pins_get_user(const NesPins *p, int pin) {
    return state_in(p->broken, p->flaky, pin);
}

/* ── Tilt ────────────────────────────────────────────────────────────────────
 * The contacts sit on a 2.54 mm (0.1") pitch, the two rows facing each other
 * across the board: pins n and n+30 are at the same place along the edge.
 * Tilted by an angle, the cartridge pivots on the end still seated, and a
 * contact rises by its distance from that end times sin(angle).  The slot's
 * springs wipe about 1.5 mm of the finger: past 0.75 mm a contact only
 * touches some of the time, past 1.5 mm not at all. */

#define PIN_PITCH_MM   2.54
#define LIFT_FLAKY_MM  0.75
#define LIFT_OPEN_MM   1.5

double nes_pins_lift_mm(const NesPins *p, int pin) {
    if (pin < 1 || pin > NES_PINS_N || p->tilt_deg == 0.0) return 0.0;
    int col = (pin - 1) % 30;                       /* 0 at the pin 1/31 end */
    double from_pivot = p->tilt_deg > 0 ? col + 0.5 : 29.5 - col;
    return from_pivot * PIN_PITCH_MM * sin(fabs(p->tilt_deg) * M_PI / 180.0);
}

void nes_pins_set_tilt(NesPins *p, double deg) {
    if (deg > NES_PIN_TILT_MAX) deg = NES_PIN_TILT_MAX;
    if (deg < -NES_PIN_TILT_MAX) deg = -NES_PIN_TILT_MAX;
    p->tilt_deg = deg;
    p->tilt_broken = p->tilt_flaky = 0;
    for (int pin = 1; pin <= NES_PINS_N; pin++) {
        double lift = nes_pins_lift_mm(p, pin);
        if (lift > LIFT_OPEN_MM) p->tilt_broken |= NES_PIN_BIT(pin);
        else if (lift > LIFT_FLAKY_MM) p->tilt_flaky |= NES_PIN_BIT(pin);
    }
    p->version++;
}

void nes_pins_set(NesPins *p, int pin, NesPinState st) {
    if (pin < 1 || pin > NES_PINS_N) return;
    uint64_t b = NES_PIN_BIT(pin);
    p->broken &= ~b;
    p->flaky &= ~b;
    if (st == NES_PIN_BROKEN) p->broken |= b;
    if (st == NES_PIN_FLAKY) p->flaky |= b;
    p->version++;
}

static uint32_t next_rand(NesPins *p) {
    uint32_t x = p->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return p->rng = x;
}

uint64_t nes_pins_sample(NesPins *p) {
    uint64_t open = nes_pins_open_mask(p), flaky = nes_pins_flaky_mask(p);
    if (!flaky) return open;
    uint64_t r1 = (uint64_t)next_rand(p) << 32 | next_rand(p);
    uint64_t r2 = (uint64_t)next_rand(p) << 32 | next_rand(p);
    return open | (flaky & r1 & r2);
}

bool nes_pins_dead(uint64_t open) {
    uint64_t gnd = NES_PIN_BIT(NP_GND_A) | NES_PIN_BIT(NP_GND_B);
    uint64_t vcc = NES_PIN_BIT(NP_VCC_A) | NES_PIN_BIT(NP_VCC_B);
    return (open & gnd) == gnd || (open & vcc) == vcc;
}

static unsigned gather(uint64_t open, const uint8_t *pins, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        if (open & NES_PIN_BIT(pins[i])) v |= 1u << i;
    return v;
}

uint16_t nes_pins_cpu_addr_open(uint64_t open) { return (uint16_t)gather(open, cpu_a_pin, 15); }
uint8_t  nes_pins_cpu_data_open(uint64_t open) { return (uint8_t)gather(open, cpu_d_pin, 8); }
uint16_t nes_pins_ppu_addr_open(uint64_t open) { return (uint16_t)gather(open, ppu_a_pin, 14); }
uint8_t  nes_pins_ppu_data_open(uint64_t open) { return (uint8_t)gather(open, ppu_d_pin, 8); }

/* "CPU A3", "cpu_a3", "cpua3" all match: case, spaces and '_' ignored. */
static void squash(const char *in, char *out, size_t len) {
    size_t n = 0;
    for (; *in && n + 1 < len; in++)
        if (*in != ' ' && *in != '_' && *in != '-') out[n++] = (char)tolower((unsigned char)*in);
    out[n] = '\0';
}

int nes_pins_find(const char *name) {
    char *end;
    long n = strtol(name, &end, 10);
    if (*name && *end == '\0') return n >= 1 && n <= NES_PINS_N ? (int)n : 0;
    char want[32], have[32];
    squash(name, want, sizeof(want));
    for (int i = 1; i <= NES_PINS_N; i++) {
        squash(nes_pin_info[i].label, have, sizeof(have));
        if (strcmp(want, have) == 0) return i;   /* the first GND / +5V */
    }
    return 0;
}
