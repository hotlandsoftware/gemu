/* Save state for the I2C devices: see i2c_state.h. */
#include "i2c/i2c_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Lm87, in order.  Excluded: id_reported, monitor_reported,
 * value_reported, reports - which debug reports were printed and how many
 * (host log limits, I2C_DEBUG only). */
bool lm87_state_add(GemuDevice *d, const char *prefix, Lm87 *x) {
    bool ok = true;
    M(reg); M(ptr); M(bytes_written);
    return ok;
}

/* At24: every member, in order. */
bool at24_state_add(GemuDevice *d, const char *prefix, At24 *x) {
    bool ok = true;
    M(mem); M(ptr); M(addr_next); M(write_protect);
    return ok;
}

/* ItaniumCartRom: every member, in order (writable is the SMWP strap, set
 * at init; saved as it is a plain flag of the chip's struct). */
bool itanium_cart_rom_state_add(GemuDevice *d, const char *prefix, ItaniumCartRom *x) {
    bool ok = true;
    M(mem); M(ptr); M(addr_next); M(writable); M(page_base);
    return ok;
}
