/*
 * Time, timers and CPU executors.  (.work/modular-devices, 2.3; question 1:
 * picoseconds)
 *
 * A machine has one clock, GemuTime picoseconds since power-on (2^64 ps is
 * 213 days).  Clocks that don't divide a picosecond evenly don't drift:
 * timers and executors started from a clock keep (hz, cycles) and compute
 * every deadline as anchor + gemu_clocks(hz, cycles), exactly.
 *
 * Executors are CPUs (or anything that runs in cycles).  The scheduler
 * hands each a slice up to the next timer deadline (or the quantum, when
 * there are several executors), in the order they were added; timers fire
 * between slices, in deadline order (equal deadlines: the order started).
 * While an executor runs, gemu_now() is *its* time - so a device read in
 * the middle of a slice sees when it happened - provided the executor says
 * how far it has got (the `elapsed` hook).  A timer started then, due before
 * the slice ends, shortens the slice: the run loop should watch
 * gemu_executor_budget().
 *
 * During a timer callback, gemu_now() is the timer's deadline.
 *
 * Real-time pacing is not here yet (stage D3): gemu_machine_run_until()
 * runs as fast as it can.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gemu/dev/device.h"

typedef uint64_t GemuTime;                   /* picoseconds */
typedef struct GemuTimer    GemuTimer;
typedef struct GemuExecutor GemuExecutor;

#define GEMU_PS_PER_SEC 1000000000000ull
#define GEMU_TIME_NEVER UINT64_MAX
#define GEMU_USEC(n) ((GemuTime)(n) * 1000000ull)
#define GEMU_MSEC(n) ((GemuTime)(n) * 1000000000ull)
#define GEMU_SEC(n)  ((GemuTime)(n) * GEMU_PS_PER_SEC)

/* cycles of an hz clock in picoseconds, rounded down (exact: 128-bit). */
GemuTime gemu_clocks(uint64_t hz, uint64_t cycles);
/* Whole cycles of an hz clock completed in t (rounded down), and the
 * cycles needed to reach at least t (rounded up). */
uint64_t gemu_cycles_in(uint64_t hz, GemuTime t);
uint64_t gemu_cycles_to(uint64_t hz, GemuTime t);

/* The current time (see above) and the scheduler's time (where every
 * executor has got to; timers before it have fired). */
GemuTime gemu_now(GemuMachine *m);
GemuTime gemu_machine_time(GemuMachine *m);

/* Run until time t: executors and timers.  Returns early (false) if
 * gemu_machine_stop() is called; the machine time is then where it got. */
bool gemu_machine_run_until(GemuMachine *m, GemuTime t);
void gemu_machine_stop(GemuMachine *m);

/* With several executors, the longest slice each gets before the others
 * catch up (MAME's quantum).  0 = up to the next timer (the default). */
void gemu_machine_set_quantum(GemuMachine *m, GemuTime q);

/* ── Timers ───────────────────────────────────────────────────────────────── */

/* d: the owner (its machine's scheduler); destroying d frees its timers
 * (and executors); the machine frees what's left. */
GemuTimer *gemu_timer_new(GemuDevice *d, void (*cb)(void *ud, int param), void *ud);
void       gemu_timer_free(GemuTimer *t);
/* First after delay from gemu_now(), then every period (0 = once). */
void       gemu_timer_start(GemuTimer *t, GemuTime delay, GemuTime period, int param);
/* The same in cycles of an hz clock: fire n is at
 * now + gemu_clocks(hz, delay + n * period), exactly. */
void       gemu_timer_start_clocks(GemuTimer *t, uint64_t hz, uint64_t delay,
                                   uint64_t period, int param);
void       gemu_timer_stop(GemuTimer *t);
bool       gemu_timer_running(const GemuTimer *t);
/* The next deadline (GEMU_TIME_NEVER if stopped) and the time left to it. */
GemuTime   gemu_timer_expires(const GemuTimer *t);
GemuTime   gemu_timer_remaining(const GemuTimer *t);

/* ── Executors ────────────────────────────────────────────────────────────── */

typedef struct {
    /* Run up to `budget` cycles (more is fine: finish the instruction);
     * return how many ran.  Keep going while elapsed < the executor's
     * current budget (gemu_executor_budget), which can shrink mid-run. */
    uint64_t (*run)(void *ud, uint64_t budget);
    /* Cycles run so far in the current run; NULL = the executor can't
     * tell, and gemu_now() inside it is the start of the slice. */
    uint64_t (*elapsed)(void *ud);
} GemuExecutorOps;

/* Read-only to users. */
struct GemuExecutor {
    uint64_t         budget;      /* cycles allowed in the current run */
    GemuMachine     *machine;
    GemuDevice      *device;
    unsigned         index;       /* the device's nth executor (its name in states) */
    GemuExecutorOps  ops;
    void            *ud;
    uint64_t         hz;
    GemuTime         anchor;      /* local time = anchor + gemu_clocks(hz, cycles) */
    uint64_t         cycles;      /* cycles since the anchor, up to the current run */
    bool             suspended, running;
    unsigned         load_mark;   /* (state.c) */
    GemuExecutor    *next;
};

GemuExecutor *gemu_executor_new(GemuDevice *d, uint64_t hz, const GemuExecutorOps *ops,
                                void *ud);
void     gemu_executor_free(GemuExecutor *e);
/* Its own time (mid-run: as far as it has got). */
GemuTime gemu_executor_time(const GemuExecutor *e);
/* Change the clock from now on. */
void     gemu_executor_set_clock(GemuExecutor *e, uint64_t hz);
/* A suspended executor doesn't run (a halted CPU, one held in reset);
 * resumed, it carries on from the current time. */
void     gemu_executor_suspend(GemuExecutor *e, bool on);
/* End the current run as soon as possible (e.g. after waking another CPU). */
void     gemu_executor_yield(GemuExecutor *e);

static inline uint64_t gemu_executor_budget(const GemuExecutor *e) { return e->budget; }
