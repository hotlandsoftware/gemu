/* The 8253 PIT as a core device: see i8253_dev.h. */
#include "timer/i8253_dev.h"
#include "timer/i8253_state.h"
#include "gemu/dev/line.h"
#include <stdio.h>

typedef struct {
    I8253       chip;
    GemuLineIn *gate[3];
    GemuLine   *out[3];
} Pit;

I8253 *gemu_i8253(GemuDevice *d) { return &((Pit *)d->priv)->chip; }
void gemu_i8253_tick(GemuDevice *d, uint32_t ticks) { i8253_tick(&((Pit *)d->priv)->chip, ticks); }

static void out_changed(void *ud, unsigned ch, bool level) {
    gemu_line_set(((Pit *)ud)->out[ch], level);
}
static void gate_changed(void *ud, int idx, bool level) {
    i8253_set_gate(&((Pit *)ud)->chip, (unsigned)idx, level);
}

static uint8_t io_r8(void *ud, uint64_t off) {
    return i8253_io_read(&((Pit *)((GemuDevice *)ud)->priv)->chip, (uint16_t)off);
}
static void io_w8(void *ud, uint64_t off, uint8_t v) {
    i8253_io_write(&((Pit *)((GemuDevice *)ud)->priv)->chip, (uint16_t)off, v);
}
/* No peek: reading a counter moves its LSB/MSB flip-flop and its latch. */
const GemuHandlers gemu_i8253_io = { .r8 = io_r8, .w8 = io_w8 };

static bool pit_init(GemuDevice *d) {
    Pit *p = d->priv;
    p->chip.out_changed = out_changed;
    p->chip.ud = p;
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof name, "gate%d", i);
        if (!(p->gate[i] = gemu_line_in(d, name, gate_changed, p, i)))
            return false;
        gemu_line_in_set_default(p->gate[i], true);
        snprintf(name, sizeof name, "out%d", i);
        if (!(p->out[i] = gemu_line_out(d, name)))
            return false;
    }
    return i8253_state_add(d, "", &p->chip);
}

static void pit_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Pit *p = d->priv;
    i8253_reset(&p->chip);
    for (int i = 0; i < 3; i++) {
        i8253_set_gate(&p->chip, (unsigned)i, gemu_line_in_level(p->gate[i]));
        gemu_line_set(p->out[i], p->chip.ch[i].out);
    }
}

static void pit_describe(GemuDevice *d, char *buf, size_t len) {
    const I8253 *c = &((Pit *)d->priv)->chip;
    static const char *const rw[] = { "latch", "LSB", "MSB", "LSB+MSB" };
    size_t n = 0;
    for (int i = 0; i < 3 && n < len; i++) {
        const I8253Channel *ch = &c->ch[i];
        int k = snprintf(buf + n, len - n,
                         "%schannel %d: mode %u, %s, count %04X reload %04X, gate %s, out %s%s%s",
                         i ? "\n" : "", i, ch->mode, rw[ch->rw_mode & 3], ch->count, ch->reload,
                         ch->gate ? "high" : "low", ch->out ? "high" : "low",
                         ch->counting ? "" : ", no count loaded",
                         ch->latched ? ", latched" : "");
        if (k < 0)
            break;
        n += (size_t)k;
    }
}

const GemuDeviceType gemu_i8253_type = {
    .name = "i8253", .desc = "Intel 8253 programmable interval timer",
    .size = sizeof(Pit), .init = pit_init, .reset = pit_reset, .describe = pit_describe,
};
