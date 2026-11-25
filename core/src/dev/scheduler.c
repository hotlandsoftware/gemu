/* Time, timers and executors: see gemu/dev/scheduler.h. */
#include "dev_priv.h"
#include <stdlib.h>

struct GemuTimer {
    GemuMachine *m;
    void       (*cb)(void *ud, int param);
    void        *ud;
    int          param;
    bool         on;
    GemuTime     deadline;
    uint64_t     seq;
    size_t       hidx;               /* in the heap, while on */
    /* periodic: in picoseconds, or in cycles of hz from anchor */
    GemuTime     period;
    uint64_t     hz, k, period_cycles;
    GemuTime     anchor;
    GemuTimer   *next, *prev;        /* all timers */
};

typedef unsigned __int128 u128;

GemuTime gemu_clocks(uint64_t hz, uint64_t cycles) {
    if (!hz)
        return GEMU_TIME_NEVER;
    u128 v = (u128)cycles * GEMU_PS_PER_SEC / hz;
    return v > GEMU_TIME_NEVER ? GEMU_TIME_NEVER : (GemuTime)v;
}

uint64_t gemu_cycles_in(uint64_t hz, GemuTime t) {
    u128 v = (u128)t * hz / GEMU_PS_PER_SEC;
    return v > UINT64_MAX ? UINT64_MAX : (uint64_t)v;
}

uint64_t gemu_cycles_to(uint64_t hz, GemuTime t) {
    u128 v = ((u128)t * hz + GEMU_PS_PER_SEC - 1) / GEMU_PS_PER_SEC;
    return v > UINT64_MAX ? UINT64_MAX : (uint64_t)v;
}

static GemuTime add_sat(GemuTime a, GemuTime b) {
    return a > GEMU_TIME_NEVER - b ? GEMU_TIME_NEVER : a + b;
}

/* ── Executors' time ──────────────────────────────────────────────────────── */

static GemuTime exec_time(const GemuExecutor *e) {
    uint64_t c = e->cycles;
    if (e->running && e->ops.elapsed)
        c += e->ops.elapsed(e->ud);
    return add_sat(e->anchor, gemu_clocks(e->hz, c));
}

GemuTime gemu_now(GemuMachine *m) {
    GemuSched *s = &m->sched;
    return s->running ? exec_time(s->running) : s->cur;
}

GemuTime gemu_machine_time(GemuMachine *m) { return m->sched.now; }

GemuTime gemu_executor_time(const GemuExecutor *e) {
    return e->suspended ? e->machine->sched.cur : exec_time(e);
}

/* Cycles e may run in its current run to reach the slice end. */
static uint64_t budget_to(const GemuExecutor *e, GemuTime end) {
    if (end <= e->anchor)
        return 0;
    /* signed: mid-run after a clock change, cycles is "minus the cycles run
     * so far" (see gemu_executor_set_clock) */
    int64_t d = (int64_t)(gemu_cycles_to(e->hz, end - e->anchor) - e->cycles);
    return d > 0 ? (uint64_t)d : 0;
}

/* A deadline before the end of the running slice shortens it. */
static void shorten_slice(GemuSched *s, GemuTime deadline) {
    if (!s->running || deadline >= s->slice_end)
        return;
    s->slice_end = deadline;
    GemuExecutor *e = s->running;
    uint64_t b = budget_to(e, deadline);
    uint64_t el = e->ops.elapsed ? e->ops.elapsed(e->ud) : 0;
    e->budget = b > el ? b : el;
}

/* ── The timer heap ───────────────────────────────────────────────────────── */

static bool before(const GemuTimer *a, const GemuTimer *b) {
    return a->deadline < b->deadline || (a->deadline == b->deadline && a->seq < b->seq);
}

static void heap_set(GemuSched *s, size_t i, GemuTimer *t) {
    s->heap[i] = t;
    t->hidx = i;
}

static void sift_up(GemuSched *s, size_t i) {
    GemuTimer *t = s->heap[i];
    while (i) {
        size_t p = (i - 1) / 2;
        if (!before(t, s->heap[p]))
            break;
        heap_set(s, i, s->heap[p]);
        i = p;
    }
    heap_set(s, i, t);
}

static void sift_down(GemuSched *s, size_t i) {
    GemuTimer *t = s->heap[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= s->nheap)
            break;
        if (c + 1 < s->nheap && before(s->heap[c + 1], s->heap[c]))
            c++;
        if (!before(s->heap[c], t))
            break;
        heap_set(s, i, s->heap[c]);
        i = c;
    }
    heap_set(s, i, t);
}

static void heap_remove(GemuSched *s, GemuTimer *t) {
    size_t i = t->hidx;
    GemuTimer *last = s->heap[--s->nheap];
    if (i < s->nheap) {
        heap_set(s, i, last);
        sift_down(s, i);
        sift_up(s, last->hidx);
    }
}

static void heap_push(GemuSched *s, GemuTimer *t) {
    if (s->nheap == s->cap) {
        size_t n = s->cap ? s->cap * 2 : 32;
        GemuTimer **h = realloc(s->heap, n * sizeof *h);
        if (!h)
            abort();
        s->heap = h;
        s->cap = n;
    }
    heap_set(s, s->nheap++, t);
    sift_up(s, s->nheap - 1);
}

static void arm(GemuTimer *t, GemuTime deadline) {
    GemuSched *s = &t->m->sched;
    if (t->on)
        heap_remove(s, t);
    t->on = true;
    t->deadline = deadline;
    t->seq = ++s->seq;
    heap_push(s, t);
    shorten_slice(s, deadline);
}

/* ── Timers ───────────────────────────────────────────────────────────────── */

GemuTimer *gemu_timer_new(GemuDevice *d, void (*cb)(void *ud, int param), void *ud) {
    if (!d || !cb)
        return NULL;
    GemuTimer *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    GemuSched *s = &d->machine->sched;
    t->m = d->machine;
    t->cb = cb;
    t->ud = ud;
    t->deadline = GEMU_TIME_NEVER;
    t->next = s->timers;
    if (s->timers)
        s->timers->prev = t;
    s->timers = t;
    return t;
}

void gemu_timer_free(GemuTimer *t) {
    if (!t)
        return;
    GemuSched *s = &t->m->sched;
    gemu_timer_stop(t);
    if (t->prev) t->prev->next = t->next;
    else         s->timers = t->next;
    if (t->next) t->next->prev = t->prev;
    free(t);
}

void gemu_timer_start(GemuTimer *t, GemuTime delay, GemuTime period, int param) {
    t->param = param;
    t->period = period;
    t->hz = 0;
    arm(t, add_sat(gemu_now(t->m), delay));
}

void gemu_timer_start_clocks(GemuTimer *t, uint64_t hz, uint64_t delay,
                             uint64_t period, int param) {
    if (!hz) {
        gemu_timer_stop(t);
        return;
    }
    t->param = param;
    t->period = 0;
    t->hz = hz;
    t->anchor = gemu_now(t->m);
    t->k = delay;
    t->period_cycles = period;
    arm(t, add_sat(t->anchor, gemu_clocks(hz, delay)));
}

void gemu_timer_stop(GemuTimer *t) {
    if (!t->on)
        return;
    heap_remove(&t->m->sched, t);
    t->on = false;
    t->deadline = GEMU_TIME_NEVER;
}

bool gemu_timer_running(const GemuTimer *t) { return t->on; }
GemuTime gemu_timer_expires(const GemuTimer *t) { return t->on ? t->deadline : GEMU_TIME_NEVER; }

GemuTime gemu_timer_remaining(const GemuTimer *t) {
    if (!t->on)
        return GEMU_TIME_NEVER;
    GemuTime now = gemu_now(t->m);
    return t->deadline > now ? t->deadline - now : 0;
}

/* Fire every timer due by the machine time. */
static void fire_due(GemuSched *s) {
    while (s->nheap && s->heap[0]->deadline <= s->now && !s->stop) {
        GemuTimer *t = s->heap[0];
        GemuTime due = t->deadline;
        s->cur = due;
        /* re-arm before the callback, which may stop or restart it */
        if (t->hz && t->period_cycles) {
            t->k += t->period_cycles;
            arm(t, add_sat(t->anchor, gemu_clocks(t->hz, t->k)));
        } else if (!t->hz && t->period) {
            arm(t, add_sat(due, t->period));
        } else {
            gemu_timer_stop(t);
        }
        t->cb(t->ud, t->param);
    }
    s->cur = s->now;
}

/* ── Executors ────────────────────────────────────────────────────────────── */

GemuExecutor *gemu_executor_new(GemuDevice *d, uint64_t hz, const GemuExecutorOps *ops,
                                void *ud) {
    if (!d || !hz || !ops || !ops->run)
        return NULL;
    GemuExecutor *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    GemuSched *s = &d->machine->sched;
    e->machine = d->machine;
    e->device = d;
    e->ops = *ops;
    e->ud = ud;
    e->hz = hz;
    e->anchor = gemu_now(d->machine);
    if (s->execs_tail) s->execs_tail->next = e;
    else               s->execs = e;
    s->execs_tail = e;
    s->nexecs++;
    return e;
}

void gemu_executor_free(GemuExecutor *e) {
    if (!e)
        return;
    GemuSched *s = &e->machine->sched;
    if (e->running)
        abort();                    /* not from inside its own run */
    GemuExecutor *prev = NULL;
    for (GemuExecutor *x = s->execs; x; prev = x, x = x->next)
        if (x == e) {
            if (prev) prev->next = e->next;
            else      s->execs = e->next;
            if (s->execs_tail == e)
                s->execs_tail = prev;
            s->nexecs--;
            break;
        }
    free(e);
}

void gemu_executor_set_clock(GemuExecutor *e, uint64_t hz) {
    if (!hz || hz == e->hz)
        return;
    uint64_t el = e->running && e->ops.elapsed ? e->ops.elapsed(e->ud) : 0;
    e->anchor = exec_time(e);
    /* the run's cycles so far are already in the anchor: when it returns,
     * cycles += done leaves the cycles after the change */
    e->cycles = 0 - el;
    e->hz = hz;
    if (e->running) {
        uint64_t b = budget_to(e, e->machine->sched.slice_end);
        e->budget = b > el ? b : el;
    }
}

void gemu_executor_suspend(GemuExecutor *e, bool on) {
    if (on == e->suspended)
        return;
    if (on) {
        e->suspended = true;
        gemu_executor_yield(e);
    } else {
        e->suspended = false;
        e->anchor = gemu_now(e->machine);
        e->cycles = 0;
    }
}

void gemu_executor_yield(GemuExecutor *e) {
    if (e->running)
        e->budget = 0;
}

/* ── Running ──────────────────────────────────────────────────────────────── */

void gemu_machine_set_quantum(GemuMachine *m, GemuTime q) { m->sched.quantum = q; }

void gemu_machine_stop(GemuMachine *m) {
    GemuSched *s = &m->sched;
    s->stop = true;
    if (s->running)
        gemu_executor_yield(s->running);
}

bool gemu_machine_run_until(GemuMachine *m, GemuTime t) {
    GemuSched *s = &m->sched;
    s->stop = false;
    fire_due(s);
    while (s->now < t && !s->stop) {
        GemuTime end = t;
        if (s->nheap && s->heap[0]->deadline < end)
            end = s->heap[0]->deadline;
        if (s->quantum && s->nexecs > 1 && s->quantum < end - s->now)
            end = s->now + s->quantum;
        s->slice_end = end;
        for (GemuExecutor *e = s->execs; e && !s->stop; e = e->next) {
            if (e->suspended)
                continue;
            uint64_t b = budget_to(e, s->slice_end);
            if (!b)
                continue;          /* already there (overshot last time) */
            e->budget = b;
            e->running = true;
            s->running = e;
            uint64_t done = e->ops.run(e->ud, b);
            s->running = NULL;
            e->running = false;
            e->cycles += done;
            e->budget = 0;
        }
        /* a timer started during the slice may have moved its end in; a
         * stop leaves the time where the executors got to */
        GemuTime reached = s->slice_end;
        if (s->stop)
            for (GemuExecutor *e = s->execs; e; e = e->next)
                if (!e->suspended && exec_time(e) < reached)
                    reached = exec_time(e);
        if (reached > s->now)
            s->now = reached;
        s->cur = s->now;
        fire_due(s);
    }
    return !s->stop;
}

void gemu_sched_free_all(GemuMachine *m) {
    GemuSched *s = &m->sched;
    while (s->timers)
        gemu_timer_free(s->timers);
    while (s->execs)
        gemu_executor_free(s->execs);
    free(s->heap);
    s->heap = NULL;
    s->nheap = s->cap = 0;
}
