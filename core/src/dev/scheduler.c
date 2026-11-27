/* Time, timers and executors: see gemu/dev/scheduler.h. */
#include "dev_priv.h"
#include "gemu/dev/u128.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

/* These are hot (every slice, every timer): with __int128 the division by
 * the constant GEMU_PS_PER_SEC compiles to a multiplication; elsewhere
 * (32-bit hosts) gemu/dev/u128.h does it the long way. */
#if defined(__SIZEOF_INT128__)
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
#else
GemuTime gemu_clocks(uint64_t hz, uint64_t cycles) {
    if (!hz)
        return GEMU_TIME_NEVER;
    uint64_t v = gemu_muldiv_sat(cycles, GEMU_PS_PER_SEC, hz);
    return v > GEMU_TIME_NEVER ? GEMU_TIME_NEVER : (GemuTime)v;
}

uint64_t gemu_cycles_in(uint64_t hz, GemuTime t) {
    return gemu_muldiv_sat(t, hz, GEMU_PS_PER_SEC);
}

uint64_t gemu_cycles_to(uint64_t hz, GemuTime t) {
    return gemu_muldiv_ceil_sat(t, hz, GEMU_PS_PER_SEC);
}
#endif

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
    t->owner = d;
    t->index = d->core ? d->core->ntimers++ : 0;
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
    e->index = d->core ? d->core->nexecs++ : 0;
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

GemuTime gemu_machine_next_deadline(GemuMachine *m) {
    GemuSched *s = &m->sched;
    return s->nheap ? s->heap[0]->deadline : GEMU_TIME_NEVER;
}

bool gemu_machine_run_until(GemuMachine *m, GemuTime t) {
    GemuSched *s = &m->sched;
    s->stop = false;
    s->in_run = true;
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
    s->in_run = false;
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

void gemu_sched_remove_device(GemuDevice *d) {
    GemuSched *s = &d->machine->sched;
    for (GemuTimer *t = s->timers, *n; t; t = n) {
        n = t->next;
        if (t->owner == d)
            gemu_timer_free(t);
    }
    for (GemuExecutor *e = s->execs, *n; e; e = n) {
        n = e->next;
        if (e->device == d)
            gemu_executor_free(e);
    }
}

void gemu_sched_rebuild_heap(GemuMachine *m) {
    GemuSched *s = &m->sched;
    s->nheap = 0;
    for (GemuTimer *t = s->timers; t; t = t->next)
        if (t->on)
            heap_push(s, t);
}

/* ── Save state ───────────────────────────────────────────────────────────── */

void gemu_sched_state_save(GemuMachine *m, GemuStateWriter *w) {
    GemuSched *s = &m->sched;
    gemu_sw_u64(w, s->now);
    gemu_sw_u64(w, s->seq);
    uint64_t n = 0;
    for (GemuTimer *t = s->timers; t; t = t->next)
        n++;
    gemu_sw_u64(w, n);
    for (GemuTimer *t = s->timers; t; t = t->next) {
        gemu_sw_str(w, t->owner->path);
        gemu_sw_u64(w, t->index);
        gemu_sw_u8(w, t->on);
        gemu_sw_u64(w, t->deadline);
        gemu_sw_u64(w, t->seq);
        gemu_sw_u64(w, t->period);
        gemu_sw_u64(w, t->hz);
        gemu_sw_u64(w, t->k);
        gemu_sw_u64(w, t->period_cycles);
        gemu_sw_u64(w, t->anchor);
        gemu_sw_u64(w, (uint64_t)(int64_t)t->param);
    }
    gemu_sw_u64(w, s->nexecs);
    for (GemuExecutor *e = s->execs; e; e = e->next) {
        gemu_sw_str(w, e->device->path);
        gemu_sw_u64(w, e->index);
        gemu_sw_u64(w, e->hz);
        gemu_sw_u64(w, e->anchor);
        gemu_sw_u64(w, e->cycles);
        gemu_sw_u8(w, e->suspended);
    }
}

static GemuTimer *find_timer(GemuSched *s, const char *path, uint64_t index) {
    for (GemuTimer *t = s->timers; t; t = t->next)
        if (t->index == index && !strcmp(t->owner->path, path))
            return t;
    return NULL;
}

static GemuExecutor *find_exec(GemuSched *s, const char *path, uint64_t index) {
    for (GemuExecutor *e = s->execs; e; e = e->next)
        if (e->index == index && !strcmp(e->device->path, path))
            return e;
    return NULL;
}

#define BAD() do { gemu_machine_set_error(m, "state: the scheduler section is damaged"); return false; } while (0)

bool gemu_sched_state_load(GemuMachine *m, GemuStateReader *r, bool apply, unsigned epoch) {
    GemuSched *s = &m->sched;
    uint64_t now, seq, n;
    if (!gemu_sr_u64(r, &now) || !gemu_sr_u64(r, &seq) || !gemu_sr_u64(r, &n))
        BAD();
    uint64_t live = 0;
    for (GemuTimer *t = s->timers; t; t = t->next)
        live++;
    if (n != live) {
        gemu_machine_set_error(m, "state: %" PRIu64 " timers saved, %" PRIu64 " in this machine",
                               n, live);
        return false;
    }
    for (uint64_t i = 0; i < n; i++) {
        char path[GEMU_DEVICE_PATH_MAX];
        uint64_t index, deadline, tseq, period, hz, k, pc, anchor, param;
        uint8_t on;
        if (!gemu_sr_str(r, path, sizeof path) || !gemu_sr_u64(r, &index) || !gemu_sr_u8(r, &on) ||
            !gemu_sr_u64(r, &deadline) || !gemu_sr_u64(r, &tseq) || !gemu_sr_u64(r, &period) ||
            !gemu_sr_u64(r, &hz) || !gemu_sr_u64(r, &k) || !gemu_sr_u64(r, &pc) ||
            !gemu_sr_u64(r, &anchor) || !gemu_sr_u64(r, &param))
            BAD();
        GemuTimer *t = find_timer(s, path, index);
        if (!t) {
            gemu_machine_set_error(m, "state: %s has no timer %" PRIu64 " here",
                                   path[0] ? path : "(machine)", index);
            return false;
        }
        if (!apply) {
            if (t->load_mark == epoch)
                BAD();                          /* saved twice */
            t->load_mark = epoch;
            continue;
        }
        t->on = on;
        t->deadline = deadline;
        t->seq = tseq;
        t->period = period;
        t->hz = hz;
        t->k = k;
        t->period_cycles = pc;
        t->anchor = anchor;
        t->param = (int)(int64_t)param;
    }
    if (!gemu_sr_u64(r, &n))
        BAD();
    if (n != s->nexecs) {
        gemu_machine_set_error(m, "state: %" PRIu64 " executors saved, %u in this machine",
                               n, s->nexecs);
        return false;
    }
    for (uint64_t i = 0; i < n; i++) {
        char path[GEMU_DEVICE_PATH_MAX];
        uint64_t index, hz, anchor, cycles;
        uint8_t susp;
        if (!gemu_sr_str(r, path, sizeof path) || !gemu_sr_u64(r, &index) || !gemu_sr_u64(r, &hz) ||
            !gemu_sr_u64(r, &anchor) || !gemu_sr_u64(r, &cycles) || !gemu_sr_u8(r, &susp))
            BAD();
        GemuExecutor *e = find_exec(s, path, index);
        if (!e || !hz) {
            gemu_machine_set_error(m, "state: %s has no executor %" PRIu64 " here",
                                   path[0] ? path : "(machine)", index);
            return false;
        }
        if (!apply) {
            if (e->load_mark == epoch)
                BAD();
            e->load_mark = epoch;
            continue;
        }
        e->hz = hz;
        e->anchor = anchor;
        e->cycles = cycles;
        e->suspended = susp;
    }
    if (r->left)
        BAD();
    if (apply) {
        s->now = s->cur = now;
        s->seq = seq;
        gemu_sched_rebuild_heap(m);
    }
    return true;
}
#undef BAD

/* ── Real-time pacing ─────────────────────────────────────────────────────── */

uint64_t gemu_host_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return gemu_muldiv_sat((uint64_t)c.QuadPart, 1000000000u, (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

static void host_sleep_ns(uint64_t ns) {
#ifdef _WIN32
    Sleep((DWORD)(ns / 1000000u));
#else
    struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    nanosleep(&ts, NULL);
#endif
}

void gemu_machine_pace_reset(GemuMachine *m) { m->sched.paced = false; }

GemuTime gemu_machine_pace(GemuMachine *m, GemuTime slack) {
    GemuSched *s = &m->sched;
    uint64_t now = gemu_host_ns();
    if (!slack)
        slack = GEMU_MSEC(250);
    if (!s->paced || s->now < s->pace_machine) {
        s->paced = true;
        s->pace_host_ns = now;
        s->pace_machine = s->now;
        return 0;
    }
    uint64_t due = s->pace_host_ns + (s->now - s->pace_machine) / 1000u;   /* ps -> ns */
    if (due > now) {
        host_sleep_ns(due - now);
        return (due - now) * 1000u;
    }
    if ((now - due) * 1000u > slack) {               /* far behind: start over from here */
        s->pace_host_ns = now;
        s->pace_machine = s->now;
    }
    return 0;
}
