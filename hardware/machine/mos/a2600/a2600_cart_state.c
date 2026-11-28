/* The 2600 cartridge's save state: see a2600_cart_state.h. */
#include "a2600_cart_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))
#define P(part, member) (ok &= gemu_state_add_named(d, pp, #member, &x->part.member, \
                                                    sizeof x->part.member, 1))

/* ── "info": which cartridge this is ── the scheme, the Superchip and the
 * image's size (0 for an empty slot).  A state for another is refused (what
 * the old snapshot's load checked); the load changes nothing, the values
 * being the same. */
static void info_save(void *ud, GemuStateWriter *w) {
    const A2600Cart *c = ud;
    gemu_state_write_u32(w, (uint32_t)c->type);
    gemu_state_write_u32(w, c->sc);
    gemu_state_write_u32(w, c->rom ? c->rom_size : 0);
}
static bool info_check(void *ud, GemuStateReader *r) {
    const A2600Cart *c = ud;
    uint32_t type, sc, size;
    if (gemu_state_left(r) != 12 || !gemu_state_read_u32(r, &type) ||
        !gemu_state_read_u32(r, &sc) || !gemu_state_read_u32(r, &size)) {
        gemu_state_refuse(r, "2600 cartridge: bad info item");
        return false;
    }
    uint32_t have = c->rom ? c->rom_size : 0;
    if (type != (uint32_t)c->type || (sc != 0) != c->sc || size != have) {
        gemu_state_refuse(r, "2600 cartridge: the state is for a %u-byte %s%s cartridge; "
                          "the slot holds a %u-byte %s%s one", size,
                          type < A26_CART_COUNT ? a26_cart_type_name((A26CartType)type) : "?",
                          sc ? "SC" : "", have, a26_cart_type_name(c->type), c->sc ? "SC" : "");
        return false;
    }
    return true;
}
static bool info_load(void *ud, GemuStateReader *r) { return info_check(ud, r); }

/* A2600Cart, in order.  Excluded:
 *   type, sc, rom, rom_size   the cartridge itself ("info" checks them)
 *   banks, ram_size, cpu_hz   from the image and the console
 *   ar.bios                   built when the image is loaded
 * The RAM is saved whole (32 KB, the largest any scheme has). */
bool a26_cart_state_add(GemuDevice *d, const char *prefix, A2600Cart *x) {
    char name[64], pp[64];
    snprintf(name, sizeof name, "%sinfo", prefix);
    bool ok = gemu_state_add_custom(d, name, info_save, info_check, info_load, x);
    M(bank); M(fe_pending); M(ram); M(distinct);
    snprintf(pp, sizeof pp, "%sdpc.", prefix);
    P(dpc, tops); P(dpc, bottoms); P(dpc, flags); P(dpc, counters); P(dpc, music);
    P(dpc, random); P(dpc, music_cycle); P(dpc, music_frac);
    snprintf(pp, sizeof pp, "%sar.", prefix);
    P(ar, control); P(ar, page); P(ar, page_rom); P(ar, write_enabled); P(ar, data_hold);
    P(ar, data_cycle); P(ar, data_pending); P(ar, load_count);
    return ok;
}
