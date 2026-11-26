/*
 * GEMU's 86Box layer: timers (<86box/timer.h>) on core timers.
 *
 * A pc_timer_t's timestamp is 32.32 in clocks of the 86Box devices' clock
 * (tsc).  Enabling one starts its core timer for that exact moment (rounded
 * up to the picosecond, so tsc there has reached the timestamp); when it
 * fires the timer is disabled and its callback runs with tsc = now, as in
 * 86Box.  timer_add() must be called in the device's init: it makes the core
 * timer and registers the pc_timer_t's fields as save state.
 *
 * timer_stop(), timer_on_auto() and the splitting of periods over a second
 * follow 86Box's src/timer.c (GPLv2 or later).
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/timer.h>
#include "gemu/dev/state.h"
#include "gemu/dev/u128.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t tsc;
uint64_t TIMER_USEC;

struct Gemu86BoxTimer {
    Gemu86BoxDev   *owner;
    pc_timer_t     *pt;
    GemuTimer      *t;
    Gemu86BoxTimer *next;          /* the owner's */
};

/* The moment of a 32.32 timestamp, rounded up. */
static GemuTime moment(uint64_t ts_integer, uint32_t ts_frac) {
    GemuU128 fixed = gemu_u128_add(gemu_u128_shl(gemu_u128(ts_integer), 32), gemu_u128(ts_frac));
    uint64_t rem, q = gemu_u128_div64_sat(gemu_u128_mul(fixed, GEMU_PS_PER_SEC),
                                          gemu_86box_hz << 32, &rem);
    return q == UINT64_MAX ? GEMU_TIME_NEVER : (GemuTime)(q + (rem != 0));
}

static void timer_advance_ex(pc_timer_t *timer, int start);

static void fire(void *ud, int param) {
    (void)param;
    Gemu86BoxTimer *bt = ud;
    pc_timer_t *timer = bt->pt;
    Gemu86BoxDev *prev = gemu_86box_enter(bt->owner);
    timer->flags &= ~TIMER_ENABLED;
    if (timer->flags & TIMER_SPLIT)
        timer_advance_ex(timer, 0);     /* the next second of a long period */
    else if (timer->callback) {
        timer->in_callback = 1;
        timer->callback(timer->priv);
        timer->in_callback = 0;
    }
    gemu_86box_leave(prev);
}

static Gemu86BoxTimer *record(pc_timer_t *timer, const char *what) {
    Gemu86BoxTimer *bt = timer->gemu;
    if (!bt)
        fatal("%s: a timer that timer_add() didn't set up\n", what);
    return bt;
}

void timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer) {
    Gemu86BoxDev *b = gemu_86box_need("timer_add");
    if (!b->in_init)
        fatal("timer_add outside the device's init (GEMU's timers are made there)\n");
    memset(timer, 0, sizeof(pc_timer_t));
    timer->callback = callback;
    timer->priv = priv;
    Gemu86BoxTimer *bt = calloc(1, sizeof *bt);
    if (!bt)
        fatal("timer_add: out of memory\n");
    bt->owner = b;
    bt->pt = timer;
    if (!(bt->t = gemu_timer_new(b->dev, fire, bt)))
        fatal("timer_add: no core timer\n");
    bt->next = b->timers;
    b->timers = bt;
    timer->gemu = bt;
    /* its fields are the device's state (the core timer is the core's) */
    char n[32];
    unsigned k = b->n_timers++;
    snprintf(n, sizeof n, "timer%u.ts", k);
    GEMU_STATE_VAR(b->dev, n, timer->ts_integer);
    snprintf(n, sizeof n, "timer%u.frac", k);
    GEMU_STATE_VAR(b->dev, n, timer->ts_frac);
    snprintf(n, sizeof n, "timer%u.flags", k);
    GEMU_STATE_VAR(b->dev, n, timer->flags);
    snprintf(n, sizeof n, "timer%u.period", k);
    GEMU_STATE_VAR(b->dev, n, timer->period);
    if (start_timer)
        timer_set_delay_u64(timer, 0);
}

void timer_enable(pc_timer_t *timer) {
    Gemu86BoxTimer *bt = record(timer, "timer_enable");
    timer->flags |= TIMER_ENABLED;
    GemuTime at = moment(timer->ts_integer, timer->ts_frac);
    GemuTime now = gemu_now(bt->owner->dev->machine);
    gemu_timer_start(bt->t, at > now ? at - now : 0, 0, 0);
}

void timer_disable(pc_timer_t *timer) {
    if (!timer || !(timer->flags & TIMER_ENABLED))
        return;
    Gemu86BoxTimer *bt = record(timer, "timer_disable");
    timer->flags &= ~TIMER_ENABLED;
    timer->in_callback = 0;
    gemu_timer_stop(bt->t);
}

/* ── Long periods (86Box's src/timer.c) ───────────────────────────────────── */

void timer_stop(pc_timer_t *timer) {
    if (!timer)
        return;
    timer->period = 0.0;
    if (timer->flags & TIMER_ENABLED)
        timer_disable(timer);
    timer->flags &= ~TIMER_SPLIT;
    timer->in_callback = 0;
}

static void timer_do_period(pc_timer_t *timer, uint64_t period, int start) {
    if (start)
        timer_set_delay_u64(timer, period);
    else
        timer_advance_u64(timer, period);
}

static void timer_advance_ex(pc_timer_t *timer, int start) {
    if (timer->period > MAX_USEC) {
        timer_do_period(timer, MAX_USEC64 * TIMER_USEC, start);
        timer->period -= MAX_USEC;
        timer->flags |= TIMER_SPLIT;
    } else {
        if (timer->period > 0.0)
            timer_do_period(timer, (uint64_t)(timer->period * ((double)TIMER_USEC)), start);
        else
            timer_disable(timer);
        timer->period = 0.0;
        timer->flags &= ~TIMER_SPLIT;
    }
}

void timer_on_auto(pc_timer_t *timer, double period) {
    if (!timer)
        return;
    if (period > 0.0) {
        /* in its callback: on from the due time (timer_advance_u64), not now */
        int start = (timer->period <= 0.0) && !timer->in_callback;
        timer->period = period;
        timer_advance_ex(timer, start);
    } else
        timer_stop(timer);
}

void gemu_86box_timers_free(Gemu86BoxDev *b) {
    while (b->timers) {           /* the core timers go with the device */
        Gemu86BoxTimer *bt = b->timers;
        b->timers = bt->next;
        free(bt);
    }
}
