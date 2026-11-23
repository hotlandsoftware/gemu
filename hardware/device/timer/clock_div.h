#pragma once
/*
 * Converting a running count of one clock (a CPU's time base, say) into
 * whole ticks of a slower one (a timer's input), with no drift: the
 * fraction of a tick left over each time is carried to the next.
 *
 *   ClockDiv d;
 *   clock_div_init(&d, 733084000, 1193182, now);   // ITC -> 8254 input
 *   uint64_t ticks = clock_div_advance(&d, later);
 *
 * A source that runs backwards (a time base set to an earlier value)
 * restarts from there and gives no ticks.
 */
#include <stdint.h>

typedef struct {
    uint64_t from_hz, to_hz;
    uint64_t mark;          /* source count last advanced to */
    uint64_t remainder;     /* leftover source counts x to_hz */
} ClockDiv;

static inline void clock_div_init(ClockDiv *d, uint64_t from_hz, uint64_t to_hz,
                                  uint64_t now) {
    d->from_hz = from_hz;
    d->to_hz = to_hz;
    d->mark = now;
    d->remainder = 0;
}

/* Carry on from `now` without counting the time since the last mark (the
 * source's count was replaced, as by a state load). */
static inline void clock_div_rebase(ClockDiv *d, uint64_t now) {
    d->mark = now;
}

/* Whole target ticks since the last call. */
static inline uint64_t clock_div_advance(ClockDiv *d, uint64_t now) {
    if (now < d->mark) {
        d->mark = now;
        return 0;
    }
    uint64_t scaled = (now - d->mark) * d->to_hz + d->remainder;
    d->mark = now;
    d->remainder = scaled % d->from_hz;
    return scaled / d->from_hz;
}
