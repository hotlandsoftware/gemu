/* The CDP1802 as a core device: see cdp1802_dev.h. */
#include "cdp1802_dev.h"
#include "cdp1802_state.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    Cdp1802          cpu;
    void           (*dma_cb)(uint8_t *byte, void *ud);
    void            *dma_ud;
    GemuExecutor    *exec;
    GemuCdp1802Hooks h;
    uint64_t         run_start;
    GemuCdp1802End   ended;
} Cpu;

Cdp1802 *gemu_cdp1802(GemuDevice *d) { return &((Cpu *)d->priv)->cpu; }
GemuCdp1802End gemu_cdp1802_ended(GemuDevice *d) { return ((Cpu *)d->priv)->ended; }

void gemu_cdp1802_set_dma_target(GemuDevice *d, void (*cb)(uint8_t *byte, void *ud), void *ud) {
    Cpu *c = d->priv;
    c->dma_cb = cb;
    c->dma_ud = ud;
}

/* After a load: the queued DMA-out entries, and the request waiting, all
 * go to the target (the only place a machine's DMA goes). */
static void cpu_post_load(GemuDevice *d) {
    Cpu *c = d->priv;
    c->cpu.next_dma_out.cb = c->dma_cb;
    c->cpu.next_dma_out.ud = c->dma_ud;
    for (unsigned i = 0; i < c->cpu.dma_count; i++) {
        Cdp1802DmaEntry *e = &c->cpu.dma_queue[(c->cpu.dma_head + i) & CDP1802_DMA_QUEUE_MASK];
        e->cb = c->dma_cb;
        e->ud = c->dma_ud;
    }
}

static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    cdp1802_init(&c->cpu, NULL, 0x10000u);
    return cdp1802_state_add(d, "", &c->cpu);
}

static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Cdp1802 *c = &((Cpu *)d->priv)->cpu;
    snprintf(buf, len, "P %X (PC %04X)  X %X  D %02X  DF %d  Q %d  IE %d  EF %d%d%d%d\n"
             "machine cycle %" PRIu64 ", instruction %" PRIu64 "%s%s%s",
             c->P, c->R[c->P], c->X, c->D, c->DF, c->Q, c->IE,
             c->EF[0], c->EF[1], c->EF[2], c->EF[3], c->cycle_count, c->insn_count,
             c->idle ? ", idle" : "", c->dma_count ? ", DMA queued" : "",
             c->irq_pending ? ", interrupt requested" : "");
}

const GemuDeviceType gemu_cdp1802_type = {
    .name = "cdp1802", .desc = "RCA CDP1802 COSMAC CPU", .size = sizeof(Cpu),
    .init = cpu_init, .describe = cpu_describe, .post_load = cpu_post_load,
};

static uint64_t exec_run(void *ud, uint64_t budget) {
    GemuDevice *d = ud;
    Cpu *c = d->priv;
    const GemuCdp1802Hooks *h = &c->h;
    (void)budget;
    uint64_t start = c->run_start = c->cpu.cycle_count;
    c->ended = GEMU_CDP1802_BUDGET;
    while (c->cpu.cycle_count - start < gemu_executor_budget(c->exec)) {
        if (c->cpu.state == CDP1802_S_FETCH && h->stop_before && h->stop_before(h->ud)) {
            c->ended = GEMU_CDP1802_STOPPED;
            break;
        }
        if (h->cycle) h->cycle(h->ud);
        else cdp1802_step(&c->cpu);
        if (c->cpu.state == CDP1802_S_FETCH && h->stop_after && h->stop_after(h->ud)) {
            c->ended = GEMU_CDP1802_STOPPED;
            break;
        }
        if (h->done && h->done(h->ud)) { c->ended = GEMU_CDP1802_DONE; break; }
    }
    if (c->ended != GEMU_CDP1802_BUDGET)
        gemu_machine_stop(d->machine);
    return c->cpu.cycle_count - start;
}

static uint64_t exec_elapsed(void *ud) {
    Cpu *c = ((GemuDevice *)ud)->priv;
    return c->cpu.cycle_count - c->run_start;
}

GemuExecutor *gemu_cdp1802_exec(GemuDevice *d, uint64_t hz, const GemuCdp1802Hooks *h) {
    Cpu *c = d->priv;
    static const GemuExecutorOps ops = { .run = exec_run, .elapsed = exec_elapsed };
    if (h) c->h = *h;
    if (!c->exec) c->exec = gemu_executor_new(d, hz, &ops, d);
    return c->exec;
}
