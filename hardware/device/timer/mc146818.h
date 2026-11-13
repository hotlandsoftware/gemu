#pragma once

/* Real-time clock of the Intel IFB (82801-style, "functionally compatible
 * with the Motorola MC146818B"): two 128-byte banks of battery-backed RAM,
 * the first 14 bytes of the standard bank being the clock and registers A-D,
 * index and target ports 70h-77h, and a 32.768 kHz divider chain that runs
 * an update cycle once a second and raises periodic, alarm and
 * update-ended interrupt flags.
 *
 * Timing. The divider counts while register A's DV selects normal operation
 * (010b). An update cycle ends every 32768 counts; UIP rises 16 counts
 * (488 us) before the count wraps and the cycle runs 65 counts (1984 us), at
 * whose end the time advances and UF and AF are set. Leaving divider reset,
 * the first update comes half a second later. The clock is evaluated lazily
 * from the board clock whenever it is accessed or polled. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum { MC146818_NV_SIZE = 256 };

/* Board wiring. `clock` returns ticks at `clock_hz`; `irq` receives IRQ8 as a
 * level (high from IRQF until register C is read). Both are optional. */
typedef struct Mc146818Config {
    uint64_t (*clock)(void *opaque);
    uint64_t clock_hz;
    void (*irq)(void *opaque, bool level);
    void *opaque;
    bool debug;
} Mc146818Config;

typedef struct Mc146818 {
    uint8_t nv[MC146818_NV_SIZE];   /* [0..127] standard bank, [128..255] extended */
    bool battery;                   /* the RAM holds state a battery kept */
    bool dirty;                     /* a RAM byte changed since collected */
    bool nmi_disabled;
    uint8_t std_index, ext_index;
    bool running;                   /* DV = 010b */
    uint64_t divider;               /* divider count at osc_mark */
    uint64_t osc_mark;              /* 32.768 kHz count at the last evaluation */
    bool irq;                       /* IRQ8 as last driven */
    unsigned reported;
    /* Host wiring. Not guest state. */
    Mc146818Config config;
} Mc146818;

void mc146818_init(Mc146818 *rtc, const Mc146818Config *config);
/* Standby power applied. On the first application with no battery-backed
 * state, returns true: the RAM is undefined and the caller initialises it. */
bool mc146818_power_up(Mc146818 *rtc);
/* PCI reset: the index registers and NMI enable return to their defaults. */
void mc146818_core_reset(Mc146818 *rtc);
/* The board clock restarts from zero (a processor reset): account for the
 * time so far, then count from the new origin. */
void mc146818_clock_restart(Mc146818 *rtc);
/* A port 70h-77h access. `rtc_conf` is the IFB's RTC configuration (function
 * 0, C8h) and `alt_access` its GEN_CNTL.ALTACC_EN. Returns false when the
 * port or direction is not the RTC's. */
bool mc146818_io(Mc146818 *rtc, bool write, uint16_t port, uint8_t *value,
                 uint8_t rtc_conf, bool alt_access);
/* Bring the clock up to date so interrupt flags are raised without waiting
 * for a register access. */
void mc146818_poll(Mc146818 *rtc);

/* The calendar time, as the registers hold it. */
void mc146818_get_time(Mc146818 *rtc, struct tm *tm);
void mc146818_set_time(Mc146818 *rtc, const struct tm *tm);
/* Standard register defaults for a fresh battery: binary-coded decimal,
 * 24-hour mode, divider running, 15.6 ms periodic rate. */
void mc146818_set_defaults(Mc146818 *rtc);
/* True once if RAM changed since the last call. */
bool mc146818_take_dirty(Mc146818 *rtc);
