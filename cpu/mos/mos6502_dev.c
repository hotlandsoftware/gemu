/* The 6502 as a core device: see mos6502_dev.h. */
#include "mos6502_dev.h"
#include "mos6502_state.h"
#include "mos6502cfg.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    Mos6502          cpu;
    GemuLineIn      *nmi, *irq;
    GemuExecutor    *exec;
    GemuMos6502Hooks h;
    uint64_t         run_start;
    GemuMos6502End   ended;
} Cpu;

Mos6502 *gemu_mos6502(GemuDevice *d) { return &((Cpu *)d->priv)->cpu; }
GemuExecutor *gemu_mos6502_executor(GemuDevice *d) { return ((Cpu *)d->priv)->exec; }
GemuMos6502End gemu_mos6502_ended(GemuDevice *d) { return ((Cpu *)d->priv)->ended; }

static void nmi_in(void *ud, int idx, bool level) {
    (void)idx;
    if (level) ((Cpu *)ud)->cpu.nmi = true;
}
static void irq_in(void *ud, int idx, bool level) {
    (void)idx;
    ((Cpu *)ud)->cpu.irq = level;
}

static const char *const variants[] = { "6502", "2a03", "2a07", "6501", "6507", NULL };
static const GemuOptionDef cpu_opts[] = {
    { "variant", "6502 variant (pins, decimal mode)", GEMU_OPT_CHOICE, "6502", 0, 0, variants },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    mos6502_init(&c->cpu);
    static const MosCpuType types[] = { MOS_CPU_6502, MOS_CPU_2A03, MOS_CPU_2A07, MOS_CPU_6501,
                                        MOS_CPU_6507 };
    mos_cpu_apply(&c->cpu, types[gemu_opt_choice(d, "variant")]);
    return (c->nmi = gemu_line_in(d, "nmi", nmi_in, c, 0)) &&
           (c->irq = gemu_line_in(d, "irq", irq_in, c, 0)) &&
           mos6502_state_add(d, "", &c->cpu);
}

static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Mos6502 *c = &((Cpu *)d->priv)->cpu;
    snprintf(buf, len, "PC %04X  A %02X  X %02X  Y %02X  SP %02X  P %02X\n"
             "cycle %" PRIu64 ", instruction %" PRIu64 "%s%s",
             c->PC, c->A, c->X, c->Y, c->SP, c->P, c->cycle_count, c->insn_count,
             c->nmi ? ", NMI pending" : "", c->irq ? ", /IRQ low" : "");
}

const GemuDeviceType gemu_mos6502_type = {
    .name = "mos6502", .desc = "MOS 6502 CPU", .size = sizeof(Cpu), .init = cpu_init,
    .describe = cpu_describe, .options = cpu_opts,
};

static uint64_t exec_run(void *ud, uint64_t budget) {
    GemuDevice *d = ud;
    Cpu *c = d->priv;
    const GemuMos6502Hooks *h = &c->h;
    (void)budget;
    uint64_t start = c->run_start = c->cpu.cycle_count;
    c->ended = GEMU_MOS6502_BUDGET;
    while (c->cpu.cycle_count - start < gemu_executor_budget(c->exec)) {
        if (h->stop_before && h->stop_before(h->ud)) { c->ended = GEMU_MOS6502_STOPPED; break; }
        if (h->insn) h->insn(h->ud);
        else mos6502_step(&c->cpu);
        if (h->stop_after && h->stop_after(h->ud)) { c->ended = GEMU_MOS6502_STOPPED; break; }
        if (h->done && h->done(h->ud)) { c->ended = GEMU_MOS6502_DONE; break; }
    }
    if (c->ended != GEMU_MOS6502_BUDGET)
        gemu_machine_stop(d->machine);
    return c->cpu.cycle_count - start;
}

static uint64_t exec_elapsed(void *ud) {
    Cpu *c = ((GemuDevice *)ud)->priv;
    return c->cpu.cycle_count - c->run_start;
}

GemuExecutor *gemu_mos6502_exec(GemuDevice *d, uint64_t hz, const GemuMos6502Hooks *h) {
    Cpu *c = d->priv;
    static const GemuExecutorOps ops = { .run = exec_run, .elapsed = exec_elapsed };
    if (h) c->h = *h;
    if (!c->exec) c->exec = gemu_executor_new(d, hz, &ops, d);
    return c->exec;
}
