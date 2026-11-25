/* Time, timers and executors (core/src/dev/scheduler.c).
 * Run: ninja check-scheduler */
#include "gemu/dev/scheduler.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static GemuMachine *m;

/* ── Timer callbacks that log "<name>@<time>" ── */
static char log_buf[2048];
static void logt(const char *name) {
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s%s@%" PRIu64, n ? " " : "", name, gemu_now(m));
}
static void named_cb(void *ud, int param) { (void)param; logt(ud); }

/* ── A fake CPU: instructions of `cost` cycles; a hook after each ── */
typedef struct Cpu {
    GemuExecutor *e;
    const char *name;
    unsigned cost;
    uint64_t done, insns, runs;
    void (*hook)(struct Cpu *c);
} Cpu;

static uint64_t cpu_run(void *ud, uint64_t budget) {
    Cpu *c = ud;
    (void)budget;
    c->done = 0;
    c->runs++;
    while (c->done < gemu_executor_budget(c->e)) {
        c->done += c->cost;
        c->insns++;
        if (c->hook)
            c->hook(c);
    }
    return c->done;
}
static uint64_t cpu_elapsed(void *ud) { return ((Cpu *)ud)->done; }
static const GemuExecutorOps cpu_ops = { cpu_run, cpu_elapsed };
static const GemuExecutorOps cpu_ops_blind = { cpu_run, NULL };

static Cpu *cpu_new(const char *name, uint64_t hz, unsigned cost, const GemuExecutorOps *ops) {
    Cpu *c = calloc(1, sizeof *c);
    c->name = name;
    c->cost = cost;
    c->e = gemu_executor_new(gemu_machine_root(m), hz, ops, c);
    CHECK(c->e);
    return c;
}

/* An executor that just takes its budget (exactness over long runs). */
static uint64_t take_all(void *ud, uint64_t budget) { (void)ud; return budget; }
static const GemuExecutorOps take_ops = { take_all, NULL };

static void new_machine(void) {
    if (m)
        gemu_machine_free(m);
    m = gemu_machine_new("sched");
    log_buf[0] = 0;
}

/* ── Conversions ── */
static void conversions(void) {
    CHECK(gemu_clocks(14318180, 14318180) == GEMU_PS_PER_SEC);
    CHECK(gemu_clocks(1193182, 1) == 838095);                /* 838095.34 */
    CHECK(gemu_clocks(1193182, 1193182ull * 3600) == GEMU_SEC(3600));
    CHECK(gemu_clocks(3, 1) == 333333333333ull);
    CHECK(gemu_clocks(0, 1) == GEMU_TIME_NEVER);
    CHECK(gemu_clocks(1, UINT64_MAX) == GEMU_TIME_NEVER);    /* saturates */
    CHECK(gemu_cycles_in(1193182, GEMU_SEC(1)) == 1193182);
    CHECK(gemu_cycles_in(1193182, 838095) == 0 && gemu_cycles_in(1193182, 838096) == 1);
    CHECK(gemu_cycles_to(1193182, 838095) == 1 && gemu_cycles_to(1193182, 838096) == 2);
    CHECK(gemu_cycles_to(1000000, GEMU_USEC(5)) == 5 && gemu_cycles_to(1000000, 0) == 0);
    /* clocks and cycles_to agree: reaching clocks(hz, n) takes n cycles */
    for (uint64_t n = 0; n < 5000; n += 7)
        CHECK(gemu_cycles_to(1193182, gemu_clocks(1193182, n)) <= n &&
              gemu_clocks(1193182, gemu_cycles_to(1193182, gemu_clocks(1193182, n))) ==
                  gemu_clocks(1193182, n));
}

/* ── Timers alone ── */
static GemuTimer *chain;
static void chain_cb(void *ud, int param) {
    logt(ud);
    if (param < 2)
        gemu_timer_start(chain, 0, 0, param + 1);   /* due now: fires this run */
}
static GemuTimer *periodic;
static int periodic_fires;
static void periodic_cb(void *ud, int param) {
    (void)param;
    logt(ud);
    if (++periodic_fires == 3)
        gemu_timer_stop(periodic);                  /* from its own callback */
}

static void timers(void) {
    new_machine();
    GemuDevice *root = gemu_machine_root(m);
    GemuTimer *t1 = gemu_timer_new(root, named_cb, "t1");
    GemuTimer *t2 = gemu_timer_new(root, named_cb, "t2");
    GemuTimer *t3 = gemu_timer_new(root, named_cb, "t3");
    GemuTimer *t4 = gemu_timer_new(root, named_cb, "t4");
    CHECK(!gemu_timer_running(t1) && gemu_timer_expires(t1) == GEMU_TIME_NEVER);
    gemu_timer_start(t1, 300, 0, 0);
    gemu_timer_start(t2, 100, 0, 0);
    gemu_timer_start(t3, 100, 0, 0);                  /* same time: after t2 */
    gemu_timer_start(t4, 200, 0, 0);
    CHECK(gemu_timer_expires(t4) == 200 && gemu_timer_remaining(t4) == 200);
    gemu_timer_stop(t4);
    gemu_timer_stop(t4);                              /* twice: fine */
    CHECK(!gemu_timer_running(t4));
    gemu_timer_start(t1, 250, 0, 0);                  /* restart moves it */
    CHECK(gemu_machine_run_until(m, 1000));
    CHECK(!strcmp(log_buf, "t2@100 t3@100 t1@250"));
    CHECK(gemu_machine_time(m) == 1000 && gemu_now(m) == 1000);
    CHECK(!gemu_timer_running(t1));
    /* relative to now */
    gemu_timer_start(t4, 5, 0, 0);
    CHECK(gemu_timer_expires(t4) == 1005);
    log_buf[0] = 0;
    gemu_machine_run_until(m, 1005);                  /* due exactly at the end */
    CHECK(!strcmp(log_buf, "t4@1005"));

    /* started from a callback with no delay: the same run, the same time */
    chain = gemu_timer_new(root, chain_cb, "c");
    gemu_timer_start(chain, 10, 0, 0);
    log_buf[0] = 0;
    gemu_machine_run_until(m, 2000);
    CHECK(!strcmp(log_buf, "c@1015 c@1015 c@1015"));

    /* periodic in picoseconds, stopped from its own callback */
    periodic = gemu_timer_new(root, periodic_cb, "p");
    gemu_timer_start(periodic, 250, 100, 0);
    log_buf[0] = 0;
    gemu_machine_run_until(m, 3000);
    CHECK(!strcmp(log_buf, "p@2250 p@2350 p@2450"));
    gemu_timer_free(t2);
    gemu_timer_start(t3, 10, 0, 0);
    gemu_timer_free(t3);                              /* running: removed */
    log_buf[0] = 0;
    gemu_machine_run_until(m, 4000);
    CHECK(log_buf[0] == 0);
    /* a long delay saturates instead of wrapping */
    gemu_timer_start(t1, GEMU_TIME_NEVER - 10, 0, 0);
    CHECK(gemu_timer_expires(t1) == GEMU_TIME_NEVER);
}

/* ── No drift over hours ── */
static uint64_t pit_fires;
static uint64_t pit_hz = 1193182;
static GemuTime pit_anchor;
static bool pit_exact = true;
static void pit_cb(void *ud, int param) {
    (void)ud; (void)param;
    pit_fires++;
    if (gemu_now(m) != pit_anchor + gemu_clocks(pit_hz, pit_fires * 65536))
        pit_exact = false;
}

static void no_drift(void) {
    new_machine();
    GemuDevice *root = gemu_machine_root(m);
    gemu_machine_run_until(m, 12345);                 /* start off zero */
    GemuTimer *pit = gemu_timer_new(root, pit_cb, NULL);
    pit_anchor = gemu_now(m);
    gemu_timer_start_clocks(pit, pit_hz, 65536, 65536, 0);   /* 18.2 Hz */
    GemuExecutor *cpu = gemu_executor_new(root, 4772727, &take_ops, NULL);
    /* ten hours, in uneven steps */
    GemuTime end = GEMU_SEC(36000);
    for (GemuTime t = 0; t < end;) {
        t += GEMU_MSEC(997) + 13;
        if (t > end)
            t = end;
        CHECK(gemu_machine_run_until(m, t));
    }
    CHECK(pit_exact);
    uint64_t want = gemu_cycles_in(pit_hz, end - pit_anchor) / 65536;
    CHECK(pit_fires == want);                          /* 655,000-odd */
    /* the same period in rounded picoseconds would be this far off by now */
    GemuTime naive = pit_fires * gemu_clocks(pit_hz, 65536);
    GemuTime exact = gemu_clocks(pit_hz, pit_fires * 65536);
    CHECK(exact - naive > 100000);                     /* > 100 ns */
    /* the executor took exactly the cycles of ten hours */
    CHECK(cpu->cycles == gemu_cycles_to(4772727, end - 0));
    CHECK(gemu_executor_time(cpu) >= end && gemu_executor_time(cpu) - end < gemu_clocks(4772727, 1));
}

/* ── An executor: gemu_now() mid-slice; a timer shortening the slice ── */
static GemuTime seen_now;
static GemuTimer *late;
static GemuTime late_fired_at, late_cpu_at;
static Cpu *cpu1;

static void hook_now(Cpu *c) {
    if (c->insns == 10)
        seen_now = gemu_now(m);
    if (c->insns == 25)                               /* at 125 us */
        gemu_timer_start(late, GEMU_USEC(50), 0, 0);  /* due 175 us: before 1 ms */
}
static void late_cb(void *ud, int param) {
    (void)ud; (void)param;
    late_fired_at = gemu_now(m);
    late_cpu_at = gemu_executor_time(cpu1->e);
}

static void executor(void) {
    new_machine();
    GemuDevice *root = gemu_machine_root(m);
    cpu1 = cpu_new("a", 1000000, 5, &cpu_ops);       /* 1 MHz, 5-cycle insns */
    cpu1->hook = hook_now;
    late = gemu_timer_new(root, late_cb, NULL);
    CHECK(gemu_machine_run_until(m, GEMU_MSEC(1)));
    CHECK(seen_now == GEMU_USEC(50));                  /* 10 insns of 5 us */
    CHECK(late_fired_at == GEMU_USEC(175));
    CHECK(late_cpu_at == GEMU_USEC(175));              /* the slice stopped there */
    CHECK(cpu1->runs == 2);                            /* 0-175 us, 175 us-1 ms */
    CHECK(cpu1->e->cycles == 1000 && gemu_executor_time(cpu1->e) == GEMU_MSEC(1));

    /* without the elapsed hook, gemu_now() inside a run is its start */
    free(cpu1);
    new_machine();
    Cpu *b = cpu1 = cpu_new("b", 1000000, 5, &cpu_ops_blind);   /* late_cb reads cpu1 */
    b->hook = hook_now;
    late = gemu_timer_new(gemu_machine_root(m), late_cb, NULL);
    gemu_machine_run_until(m, GEMU_USEC(10));
    gemu_machine_run_until(m, GEMU_MSEC(1));
    CHECK(seen_now == GEMU_USEC(10));                  /* insn 10 is in the 2nd run */
    free(b);
}

/* ── Two executors: quantum, suspend, clocks ── */
static Cpu *ca, *cb_;
static GemuTime max_skew;
static void skew_hook(Cpu *c) {
    (void)c;
    GemuTime ta = gemu_executor_time(ca->e), tb = gemu_executor_time(cb_->e);
    GemuTime d = ta > tb ? ta - tb : tb - ta;
    if (d > max_skew)
        max_skew = d;
}

static void two_executors(void) {
    /* quantum 10 us: they take turns, never far apart */
    new_machine();
    ca = cpu_new("a", 3000000, 3, &cpu_ops);
    cb_ = cpu_new("b", 1000000, 7, &cpu_ops);
    ca->hook = cb_->hook = skew_hook;
    gemu_machine_set_quantum(m, GEMU_USEC(10));
    max_skew = 0;
    CHECK(gemu_machine_run_until(m, GEMU_MSEC(1)));
    CHECK(ca->runs == 100 && cb_->runs == 100);
    CHECK(max_skew <= GEMU_USEC(10) + GEMU_USEC(7));   /* a quantum + one insn */
    CHECK(gemu_executor_time(ca->e) == GEMU_MSEC(1));  /* 3000 cycles, 3 per insn */
    GemuTime tb = gemu_executor_time(cb_->e);
    CHECK(tb >= GEMU_MSEC(1) && tb < GEMU_MSEC(1) + GEMU_USEC(7));
    free(ca);
    free(cb_);

    /* no quantum: each runs to the next timer in one go */
    new_machine();
    ca = cpu_new("a", 3000000, 3, &cpu_ops);
    cb_ = cpu_new("b", 1000000, 7, &cpu_ops);
    max_skew = 0;
    gemu_machine_run_until(m, GEMU_MSEC(1));
    CHECK(ca->runs == 1 && cb_->runs == 1);

    /* suspend b for 100 us; resumed, it starts from then */
    gemu_executor_suspend(cb_->e, true);
    uint64_t binsns = cb_->insns;
    gemu_machine_run_until(m, GEMU_MSEC(1) + GEMU_USEC(100));
    CHECK(cb_->insns == binsns && ca->runs == 2);
    CHECK(gemu_executor_time(cb_->e) == GEMU_MSEC(1) + GEMU_USEC(100));
    gemu_executor_suspend(cb_->e, false);
    gemu_machine_run_until(m, GEMU_MSEC(1) + GEMU_USEC(200));
    CHECK(cb_->insns - binsns == 15);                  /* 100 cycles in 7s: 15 */

    /* change a's clock: 3 MHz -> 6 MHz from 1.2 ms */
    uint64_t ai = ca->insns;
    gemu_executor_set_clock(ca->e, 6000000);
    gemu_machine_run_until(m, GEMU_MSEC(1) + GEMU_USEC(300));
    CHECK(ca->insns - ai == 200);                      /* 600 cycles / 3 */
    CHECK(gemu_executor_time(ca->e) == GEMU_MSEC(1) + GEMU_USEC(300));
    free(ca);
    free(cb_);
}

/* a clock change from inside the run */
static Cpu *cc;
static void midclock_hook(Cpu *c) {
    if (c->insns == 10)                               /* 50 cycles at 1 MHz = 50 us */
        gemu_executor_set_clock(c->e, 2000000);
}
static void mid_run_clock(void) {
    new_machine();
    cc = cpu_new("c", 1000000, 5, &cpu_ops);
    cc->hook = midclock_hook;
    gemu_machine_run_until(m, GEMU_USEC(100));
    /* 50 us at 1 MHz (10 insns), 50 us at 2 MHz (100 cycles: 20 insns) */
    CHECK(cc->insns == 30 && cc->runs == 1);
    CHECK(gemu_executor_time(cc->e) == GEMU_USEC(100));
    gemu_machine_run_until(m, GEMU_USEC(200));
    CHECK(cc->insns == 70);                            /* 200 cycles at 2 MHz */
    free(cc);
}

/* stop from inside a run; then carry on */
static Cpu *cs;
static void stop_hook(Cpu *c) {
    if (c->insns == 20)
        gemu_machine_stop(m);
}
static void stopping(void) {
    new_machine();
    cs = cpu_new("s", 1000000, 1, &cpu_ops);
    cs->hook = stop_hook;
    CHECK(!gemu_machine_run_until(m, GEMU_MSEC(1)));
    CHECK(cs->insns == 20);
    CHECK(gemu_machine_run_until(m, GEMU_MSEC(1)));
    CHECK(cs->insns == 1000 && gemu_executor_time(cs->e) == GEMU_MSEC(1));
    /* the machine frees what's left: timers (one running) and executors */
    GemuTimer *t = gemu_timer_new(gemu_machine_root(m), named_cb, "x");
    gemu_timer_start(t, 5, 5, 0);
    gemu_machine_free(m);
    m = NULL;
    free(cs);
}

int main(void) {
    conversions();
    timers();
    no_drift();
    executor();
    two_executors();
    mid_run_clock();
    stopping();
    CHECK(!gemu_timer_new(NULL, named_cb, NULL));
    printf("scheduler: %u checks passed\n", checks);
    return 0;
}
