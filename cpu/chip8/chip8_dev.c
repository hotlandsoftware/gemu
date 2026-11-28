/* The CHIP-8 interpreter as a core device: see chip8_dev.h. */
#include "chip8_dev.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    Chip8State    c8;
    GemuExecutor *exec;
    bool        (*stop_before)(void *ud);
    void         *stop_ud;
    uint64_t      run_start, ran;
    GemuChip8End  ended;
} Cpu;

Chip8State *gemu_chip8(GemuDevice *d) { return &((Cpu *)d->priv)->c8; }
GemuChip8End gemu_chip8_ended(GemuDevice *d) { return ((Cpu *)d->priv)->ended; }

void gemu_chip8_set_stop_before(GemuDevice *d, bool (*fn)(void *ud), void *ud) {
    Cpu *c = d->priv;
    c->stop_before = fn;
    c->stop_ud = ud;
}

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", s, member))
static bool cpu_init(GemuDevice *d) {
    Chip8State *s = gemu_chip8(d);
    gemu_tb_cache_init(&s->tb_cache, free);
    s->PC = CHIP8_ROM_BASE;
    s->rng = 0xDEADBEEFu;
    bool ok = true;
    M(V); M(I); M(PC); M(SP); M(stack); M(delay); M(sound); M(mem); M(vram);
    M(keys); M(keys_prev); M(wait_key); M(wait_reg); M(insn_count); M(rng);
    return ok;
}
#undef M

static void cpu_destroy(GemuDevice *d) { gemu_tb_cache_flush(&gemu_chip8(d)->tb_cache); }

/* After a load: blocks translated from the old memory go; the display is
 * drawn again. */
static void cpu_post_load(GemuDevice *d) {
    Chip8State *s = gemu_chip8(d);
    gemu_tb_cache_flush(&s->tb_cache);
    s->draw_flag = true;
}

static void cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Chip8State *s = gemu_chip8(d);
    int n = snprintf(buf, len, "PC %03X I %03X SP %X DT %02X ST %02X%s\n", s->PC, s->I, s->SP,
                     s->delay, s->sound, s->wait_key ? " (waiting for a key)" : "");
    for (int r = 0; r < CHIP8_NUM_REGS && n >= 0 && (size_t)n < len; r++)
        n += snprintf(buf + n, len - (size_t)n, "V%X %02X%s", r, s->V[r], r == CHIP8_NUM_REGS - 1 ? "" : " ");
    if (n >= 0 && (size_t)n < len)
        snprintf(buf + n, len - (size_t)n, "\n%" PRIu64 " instructions, %" PRIu64 " blocks translated",
                 s->insn_count, s->tb_misses);
}

const GemuDeviceType gemu_chip8_type = {
    .name = "chip8", .desc = "CHIP-8 interpreter", .size = sizeof(Cpu),
    .init = cpu_init, .destroy = cpu_destroy, .describe = cpu_describe, .post_load = cpu_post_load,
};

static uint64_t exec_run(void *ud, uint64_t budget) {
    GemuDevice *d = ud;
    Cpu *c = d->priv;
    Chip8State *s = &c->c8;
    (void)budget;
    uint64_t limit = gemu_executor_budget(c->exec), n = 0;
    c->ended = GEMU_CHIP8_BUDGET;
    c->ran = 0;
    if (c->stop_before) {
        while (n < limit && !s->wait_key) {
            if (c->stop_before(c->stop_ud)) { c->ended = GEMU_CHIP8_STOPPED; break; }
            if (!chip8_exec_one(s)) break;
            n++;
            c->ran = n;
        }
    } else {
        while (n < limit && !s->wait_key) {
            GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
            if (!tb) tb = chip8_translate_block(s, s->PC);
            if (!tb) break;
            chip8_execute_tb(s, tb);
            n += tb->n_insns;
            c->ran = n < limit ? n : limit;
        }
    }
    if (c->ended == GEMU_CHIP8_STOPPED) {
        gemu_machine_stop(d->machine);
        return n;
    }
    return limit;   /* a key wait idles the rest; a block's overshoot is free */
}

static uint64_t exec_elapsed(void *ud) { return ((Cpu *)((GemuDevice *)ud)->priv)->ran; }

GemuExecutor *gemu_chip8_exec(GemuDevice *d, uint64_t hz) {
    Cpu *c = d->priv;
    static const GemuExecutorOps ops = { .run = exec_run, .elapsed = exec_elapsed };
    if (!c->exec) c->exec = gemu_executor_new(d, hz, &ops, d);
    return c->exec;
}
