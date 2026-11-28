/* The Z80 as a core device: see z80_dev.h. */
#include "z80_dev.h"
#include "z80_state.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Z80           cpu;
    uint64_t      insns;
    GemuExecutor *exec;
    GemuZ80Hooks  h;
    uint64_t      run_start;
    GemuZ80End    ended;
    bool          end_req;     /* gemu_z80_end_run: end the run after this instruction */
    GemuZ80End    end_why;
} Cpu;

Z80 *gemu_z80(GemuDevice *d) { return &((Cpu *)d->priv)->cpu; }
GemuZ80End gemu_z80_ended(GemuDevice *d) { return ((Cpu *)d->priv)->ended; }
uint64_t gemu_z80_insns(GemuDevice *d) { return ((Cpu *)d->priv)->insns; }

void gemu_z80_set_hooks(GemuDevice *d, const GemuZ80Hooks *h) {
    Cpu *c = d->priv;
    if (h) c->h = *h;
    else memset(&c->h, 0, sizeof c->h);
}

void gemu_z80_end_run(GemuDevice *d, GemuZ80End why) {
    Cpu *c = d->priv;
    c->end_req = true;
    c->end_why = why;
}

static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    z80_init(&c->cpu);
    return z80_state_add(d, "", &c->cpu) && GEMU_STATE_MEMBER(d, "", c, insns);
}

static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Cpu *c = d->priv;
    z80_format_state(&c->cpu, buf, len);
    size_t n = strlen(buf);
    if (n < len)
        snprintf(buf + n, len - n, "%sinstruction %" PRIu64, n && buf[n - 1] != '\n' ? "\n" : "", c->insns);
}

const GemuDeviceType gemu_z80_type = {
    .name = "z80", .desc = "Zilog Z80 CPU", .size = sizeof(Cpu),
    .init = cpu_init, .describe = cpu_describe,
};

static uint64_t exec_run(void *ud, uint64_t budget) {
    GemuDevice *d = ud;
    Cpu *c = d->priv;
    const GemuZ80Hooks *h = &c->h;
    (void)budget;
    uint64_t start = c->run_start = c->cpu.cycles;
    c->ended = GEMU_Z80_BUDGET;
    c->end_req = false;
    if (!h->stop_before && !h->insn && !h->stop_after && !h->done) {
        /* no hooks: the plain loop (the budget is read each time: a timer
         * an instruction arms can bring the slice's end closer) */
        while (c->cpu.cycles - start < gemu_executor_budget(c->exec) && !c->end_req) {
            z80_step(&c->cpu);
            c->insns++;
        }
    } else {
        while (c->cpu.cycles - start < gemu_executor_budget(c->exec) && !c->end_req) {
            if (h->stop_before && h->stop_before(h->ud)) { c->ended = GEMU_Z80_STOPPED; break; }
            if (h->insn) h->insn(h->ud);
            else z80_step(&c->cpu);
            c->insns++;
            if (h->stop_after && h->stop_after(h->ud)) { c->ended = GEMU_Z80_STOPPED; break; }
            if (h->done && h->done(h->ud)) { c->ended = GEMU_Z80_DONE; break; }
        }
    }
    if (c->end_req) c->ended = c->end_why;
    if (c->ended != GEMU_Z80_BUDGET)
        gemu_machine_stop(d->machine);
    return c->cpu.cycles - start;
}

static uint64_t exec_elapsed(void *ud) {
    Cpu *c = ((GemuDevice *)ud)->priv;
    return c->cpu.cycles - c->run_start;
}

GemuExecutor *gemu_z80_exec(GemuDevice *d, uint64_t hz, const GemuZ80Hooks *h) {
    Cpu *c = d->priv;
    static const GemuExecutorOps ops = { .run = exec_run, .elapsed = exec_elapsed };
    gemu_z80_set_hooks(d, h);
    if (!c->exec) c->exec = gemu_executor_new(d, hz, &ops, d);
    return c->exec;
}
