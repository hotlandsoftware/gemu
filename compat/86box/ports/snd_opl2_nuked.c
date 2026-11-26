/* 86Box's Nuked OPL2 core (third_party/86box/v6.0, unchanged) as GEMU device
 * type "ym3812_nuked_opl2": the AdLib's chip, added by fm_driver_get().
 *
 * Its state: the registers the driver keeps (timers register themselves),
 * and the chip.  The chip's pointers all point into the chip itself (slots,
 * channels, the modulator inputs, tremolo), so it is saved whole with its
 * address and the pointers are moved on load - into this chip, in this
 * process or another.  What the driver has generated but not handed over
 * (pos, buffer) isn't state: before a save the layer has it generate up to
 * now and hand it over (pre_save), and after a load it starts empty. */
#include "../gemu_86box.h"
#include "gemu/dev/state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../../third_party/86box/v6.0/src/sound/snd_opl2_nuked.c"
#pragma GCC diagnostic pop

#define CHIP_BYTES (8 + sizeof(opl2_chip))

/* p moved from a chip at old to c; false if it wasn't inside that chip */
static bool move(void **p, uint64_t old, opl2_chip *c) {
    if (!*p)
        return true;
    uint64_t a = (uintptr_t)*p;
    if (a < old || a - old >= sizeof *c)
        return false;
    *p = (char *)c + (a - old);
    return true;
}

static bool relocate(opl2_chip *c, uint64_t old) {
    bool ok = true;
    for (int i = 0; i < 18; i++) {
        opl2_slot *s = &c->slot[i];
        ok &= move((void **)&s->channel, old, c) && move((void **)&s->chip, old, c) &&
              move((void **)&s->mod, old, c) && move((void **)&s->trem, old, c);
    }
    for (int i = 0; i < 9; i++) {
        opl2_channel *ch = &c->channel[i];
        ok &= move((void **)&ch->slotz[0], old, c) && move((void **)&ch->slotz[1], old, c) &&
              move((void **)&ch->chip, old, c);
    }
    return ok;
}

static void chip_save(void *ud, GemuStateWriter *w) {
    uint64_t base = (uintptr_t)ud;
    gemu_state_write(w, &base, 8);
    gemu_state_write(w, ud, sizeof(opl2_chip));
}

static bool chip_check(void *ud, GemuStateReader *r) {
    (void)ud;
    if (gemu_state_left(r) != CHIP_BYTES) {
        gemu_state_refuse(r, "the OPL2's state is not this version's");
        return false;
    }
    uint64_t old;
    opl2_chip *tmp = malloc(sizeof *tmp);
    bool ok = tmp && gemu_state_read(r, &old, 8) && gemu_state_read(r, tmp, sizeof *tmp) &&
              relocate(tmp, old);
    free(tmp);
    if (!ok)
        gemu_state_refuse(r, "the OPL2's state is damaged");
    return ok;
}

static bool chip_load(void *ud, GemuStateReader *r) {
    uint64_t old;
    return gemu_state_read(r, &old, 8) && gemu_state_read(r, ud, sizeof(opl2_chip)) &&
           relocate(ud, old);
}

static bool opl2_state(GemuDevice *d, void *priv) {
    nuked_opl2_drv_t *dev = priv;
    dev->flags &= ~FLAG_CYCLES;        /* GEMU's CPU times its own bus cycles (cpu.h) */
    return gemu_state_add_custom(d, "chip", chip_save, chip_check, chip_load, &dev->opl) &&
           GEMU_STATE_VAR(d, "flags", dev->flags) && GEMU_STATE_VAR(d, "port", dev->port) &&
           GEMU_STATE_VAR(d, "status", dev->status) &&
           GEMU_STATE_VAR(d, "timer_ctrl", dev->timer_ctrl) &&
           GEMU_STATE_ARRAY(d, "timer_count", dev->timer_count) &&
           GEMU_STATE_ARRAY(d, "timer_cur_count", dev->timer_cur_count);
}

GEMU_86BOX_PORT(gemu_86box_ym3812_nuked_opl2_type, ym3812_nuked_opl2_device, "ym3812_nuked_opl2",
                opl2_state, NULL, false);
