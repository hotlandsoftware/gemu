/* The SSEM as a core device: see ssem_dev.h. */
#include "ssem_dev.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    Ssem          m;
    bool          running;    /* the Stop/Run switch */
    GemuExecutor *exec;
    bool        (*stop_before)(void *ud);
    void         *stop_ud;
    uint64_t      ran;
    GemuSsemEnd   ended;
} Cpu;

Ssem *gemu_ssem(GemuDevice *d) { return &((Cpu *)d->priv)->m; }
GemuSsemEnd gemu_ssem_ended(GemuDevice *d) { return ((Cpu *)d->priv)->ended; }
void gemu_ssem_set_running(GemuDevice *d, bool run) { ((Cpu *)d->priv)->running = run; }
bool gemu_ssem_running(GemuDevice *d) { return ((Cpu *)d->priv)->running; }

void gemu_ssem_set_stop_before(GemuDevice *d, bool (*fn)(void *ud), void *ud) {
    Cpu *c = d->priv;
    c->stop_before = fn;
    c->stop_ud = ud;
}

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", m, member))
static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    Ssem *m = &c->m;
    bool ok = true;
    M(store); M(a); M(ci); M(pi); M(stopped); M(steps);
    return ok && GEMU_STATE_MEMBER(d, "", c, running);
}
#undef M

static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Cpu *c = d->priv;
    char pi[16] = "-";
    if (c->running && !c->m.stopped) ssem_disasm(c->m.pi, pi, sizeof pi);
    snprintf(buf, len, "CI %08X (line %u)  PI %s  A %08X (%d)\n%s%s, %" PRIu64 " instructions",
             (unsigned)c->m.ci, SSEM_LINE(c->m.ci), pi, (unsigned)c->m.a, (int32_t)c->m.a,
             c->running ? "run" : "stop", c->m.stopped ? " (stop lamp lit)" : "", c->m.steps);
}

const GemuDeviceType gemu_ssem_type = {
    .name = "ssem", .desc = "Manchester Baby (SSEM) processor", .size = sizeof(Cpu),
    .init = cpu_init, .describe = cpu_describe,
};

static uint64_t exec_run(void *ud, uint64_t budget) {
    GemuDevice *d = ud;
    Cpu *c = d->priv;
    (void)budget;
    uint64_t limit = gemu_executor_budget(c->exec), n = 0;
    c->ended = GEMU_SSEM_BUDGET;
    c->ran = 0;
    while (c->running && !c->m.stopped && n < limit) {
        if (c->stop_before && c->stop_before(c->stop_ud)) {
            c->ended = GEMU_SSEM_STOPPED;
            gemu_machine_stop(d->machine);
            return n;
        }
        ssem_step(&c->m);
        c->ran = ++n;
    }
    return limit;   /* stopped (the switch, or STP): the rest is idle */
}

static uint64_t exec_elapsed(void *ud) { return ((Cpu *)((GemuDevice *)ud)->priv)->ran; }

GemuExecutor *gemu_ssem_exec(GemuDevice *d, uint64_t hz) {
    Cpu *c = d->priv;
    static const GemuExecutorOps ops = { .run = exec_run, .elapsed = exec_elapsed };
    if (!c->exec) c->exec = gemu_executor_new(d, hz, &ops, d);
    return c->exec;
}
