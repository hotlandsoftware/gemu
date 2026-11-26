/* 86Box's generic ISA ROM boards (third_party/86box/v6.0, unchanged) as GEMU
 * device types "isarom", "isarom_dual" and "isarom_quad": one, two or four
 * ROM sockets, each with an image file (bios_fn=), an address (bios_addr=,
 * C0000h-EE000h) and a size (bios_size=, 4K-64K), mapped into the machine's
 * ISA memory - where a BIOS finds an option ROM and runs it.  A socket can
 * take writes (rom_writes_enabled=on: a flash part), kept in GEMU's settings
 * directory (nvr.h) between runs.  (The LBA Enhancer, for AT BIOSes, isn't
 * offered.)
 *
 * The state: the board's structure but for its pointers and mappings (which
 * don't change after init), and a writable socket's contents. */
#include "../gemu_86box.h"
#include "gemu/dev/state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wempty-body"
#include "../../../third_party/86box/v6.0/src/device/isarom.c"
#pragma GCC diagnostic pop

static const Gemu86BoxKeep isarom_keep[] = {
#define SOCKET(i) GEMU_86BOX_KEEP(isarom_t, socket[i].rom), GEMU_86BOX_KEEP(isarom_t, socket[i].fn)
    SOCKET(0), SOCKET(1), SOCKET(2), SOCKET(3),
#undef SOCKET
    { 0, 0 }
};

static bool isarom_state(GemuDevice *d, void *priv) {
    isarom_t *dev = priv;
    if (!gemu_86box_state_blob(d, "isarom", dev, sizeof *dev, isarom_keep))
        return false;
    for (unsigned i = 0; i < get_limit(dev->type); i++)
        if (dev->socket[i].writable && dev->socket[i].rom.rom) {
            char n[16];
            snprintf(n, sizeof n, "rom%u", i);
            if (!gemu_state_add(d, n, dev->socket[i].rom.rom, 1, dev->socket[i].size))
                return false;
        }
    return true;
}

GEMU_86BOX_PORT(gemu_86box_isarom_type, isarom_device, "isarom", isarom_state, NULL, false);
GEMU_86BOX_PORT(gemu_86box_isarom_dual_type, isarom_dual_device, "isarom_dual", isarom_state,
                NULL, false);
GEMU_86BOX_PORT(gemu_86box_isarom_quad_type, isarom_quad_device, "isarom_quad", isarom_state,
                NULL, false);
