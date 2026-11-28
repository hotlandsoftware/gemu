/* Atari 2600 bus with a lazily-run TIA - see a2600_bus.h. */
#include "a2600_bus.h"
#include "a2600_core.h"
#include <string.h>
#include "gemu/monitor.h"

enum { FAST_NONE, FAST_PLAIN, FAST_BANKED };

void a26_bus_sync(Atari2600State *s) {
    tia_run(&s->tia, s->cpu.cycle_count * 3u);
}

/* Cartridge fast paths, with the same results as a26_cart_read: plain
 * 2K/4K ROMs, and F8/F6/F4 (no Superchip) outside the hotspot area.  These
 * schemes don't snoop the bus, and only the Supercharger counts distinct
 * addresses, so both are skipped for them. */
static void pick_fast_path(Atari2600State *s) {
    const A2600Cart *c = &s->cart;
    s->bus_fast = FAST_NONE;
    if (!c->rom) return;
    if ((c->type == A26_CART_2K || c->type == A26_CART_4K) &&
        (c->rom_size == 2048 || c->rom_size == 4096)) {
        s->bus_fast = FAST_PLAIN;
        s->bus_fast_mask = (uint16_t)(c->rom_size - 1u);
    } else if ((c->type == A26_CART_F8 || c->type == A26_CART_F6 || c->type == A26_CART_F4) &&
               !c->sc) {
        s->bus_fast = FAST_BANKED;
    }
}

static inline void bus_access(Atari2600State *s, uint16_t addr) {
    if (addr != s->last_addr) { s->cart.distinct++; s->last_addr = addr; }
}

/* ── The bus as a core space (stage T2) ──────────────────────────────────────
 * "program", 13 bits.  Below $1000 the chips decode A7 and A9 only:
 * mirror(0, $1000, $02FF) folds every address there onto $000-$2FF, where
 * the TIA sits at $000-$07F and $200-$27F (A7 = 0), the RIOT's RAM at
 * $080-$0FF and its ports and timer at $280-$2FF.  The cartridge has
 * $1000-$1FFF; for the fast schemes (as pick_fast_path() chose them) its ROM
 * is also plain read-only memory on top: all of it for 2K/4K, the current
 * bank but for the hotspot page for F8/F6/F4.  The bus-wide work stays in
 * the CPU's read and write below, as before: the data bus, the count of
 * distinct addresses (the Supercharger's timing), the schemes that watch
 * every access (a26_cart_snoop) and the monitor's watchpoints. */

static uint8_t bus_tia_r(void *ud, uint64_t off) {
    Atari2600State *s = ud;
    a26_bus_sync(s);
    return tia_read(&s->tia, (uint16_t)off, s->bus);
}
static void bus_tia_w(void *ud, uint64_t off, uint8_t v) {
    Atari2600State *s = ud;
    a26_bus_sync(s);
    s->wsync |= tia_write(&s->tia, (uint16_t)off, v);
}
static bool bus_tia_peek(void *ud, uint64_t off, uint8_t *v) {
    *v = tia_peek(&((Atari2600State *)ud)->tia, (uint16_t)off);
    return true;
}
static uint8_t bus_riot_r(void *ud, uint64_t off) {
    Atari2600State *s = ud;
    return m6532_read(&s->riot, (uint16_t)(0x280u + off), s->cpu.cycle_count);
}
static void bus_riot_w(void *ud, uint64_t off, uint8_t v) {
    Atari2600State *s = ud;
    m6532_write(&s->riot, (uint16_t)(0x280u + off), v, s->cpu.cycle_count);
}
static bool bus_riot_peek(void *ud, uint64_t off, uint8_t *v) {
    Atari2600State *s = ud;
    *v = m6532_peek(&s->riot, (uint16_t)(0x280u + off), s->cpu.cycle_count);
    return true;
}

/* An F8/F6/F4 bank switch: the ROM window follows. */
static inline void bank_moved(Atari2600State *s) {
    A26Core *c = s->core;
    if (s->bus_fast == FAST_BANKED && c->rom_map[0] && c->rom_bank != s->cart.bank[0]) {
        c->rom_bank = s->cart.bank[0];
        gemu_mapping_set_ptr(c->rom_map[0], s->cart.rom + (size_t)c->rom_bank * 4096u);
    }
}
static uint8_t bus_cart_r(void *ud, uint64_t off) {
    Atari2600State *s = ud;
    uint8_t v = a26_cart_read(&s->cart, (uint16_t)(0x1000u + off), s->bus, s->cpu.cycle_count);
    bank_moved(s);
    return v;
}
static void bus_cart_w(void *ud, uint64_t off, uint8_t v) {
    Atari2600State *s = ud;
    a26_cart_write(&s->cart, (uint16_t)(0x1000u + off), v, s->cpu.cycle_count);
    bank_moved(s);
}
static bool bus_cart_peek(void *ud, uint64_t off, uint8_t *v) {
    Atari2600State *s = ud;
    if (!s->cart.rom) return false;
    *v = a26_cart_peek(&s->cart, (uint16_t)(0x1000u + off));
    return true;
}

static const GemuHandlers bus_tia  = { .r8 = bus_tia_r,  .w8 = bus_tia_w,  .peek8 = bus_tia_peek };
static const GemuHandlers bus_riot = { .r8 = bus_riot_r, .w8 = bus_riot_w, .peek8 = bus_riot_peek };
static const GemuHandlers bus_cart = { .r8 = bus_cart_r, .w8 = bus_cart_w, .peek8 = bus_cart_peek };

bool a26_bus_build(Atari2600State *s) {
    A26Core *c = s->core;
    if (!(c->bus = gemu_space_new(c->machine, "program", 13, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    GemuMapping *ram;
    bool ok = gemu_space_mirror(c->bus, 0x0000, 0x1000, 0x02FF) &&
              gemu_space_map(c->bus, 0x000, 0x80, &bus_tia, s, 0, 0) &&
              gemu_space_map(c->bus, 0x200, 0x80, &bus_tia, s, 0, 0) &&
              (ram = gemu_space_map_ram(c->bus, 0x080, 0x80, s->riot.ram, 0, 0)) &&
              gemu_space_map(c->bus, 0x280, 0x80, &bus_riot, s, 0, 0) &&
              gemu_space_map(c->bus, 0x1000, 0x1000, &bus_cart, s, 0, 0);
    if (ok) gemu_mapping_set_name(ram, "RIOT RAM");
    return ok;
}

/* The fast schemes' ROM as plain memory over the cartridge's handler:
 * after power-on (pick_fast_path()) and after a load. */
void a26_maps_sync(Atari2600State *s) {
    A26Core *c = s->core;
    if (!c || !c->bus) return;
    for (int i = 0; i < 2; i++)
        if (c->rom_map[i]) { gemu_mapping_remove(c->rom_map[i]); c->rom_map[i] = NULL; }
    const A2600Cart *k = &s->cart;
    if (s->bus_fast == FAST_PLAIN && k->rom_size == 2048) {
        c->rom_map[0] = gemu_space_map_ram(c->bus, 0x1000, 0x800, k->rom, GEMU_MAP_READONLY, 1);
        c->rom_map[1] = gemu_space_map_ram(c->bus, 0x1800, 0x800, k->rom, GEMU_MAP_READONLY, 1);
    } else if (s->bus_fast == FAST_PLAIN) {
        c->rom_map[0] = gemu_space_map_ram(c->bus, 0x1000, 0x1000, k->rom, GEMU_MAP_READONLY, 1);
    } else if (s->bus_fast == FAST_BANKED) {
        /* all but the last 64 bytes, which hold the hotspots ($1FF4-$1FFB) */
        c->rom_bank = k->bank[0];
        c->rom_map[0] = gemu_space_map_ram(c->bus, 0x1000, 0xFC0, k->rom + (size_t)c->rom_bank * 4096u,
                                           GEMU_MAP_READONLY, 1);
    }
    for (int i = 0; i < 2; i++)
        if (c->rom_map[i]) gemu_mapping_set_name(c->rom_map[i], "cartridge ROM");
}

/* How the 64 bytes around an address are served - plain memory (the
 * RIOT's RAM: it shares its 256-byte page with the TIA, so the space has no
 * direct page for it; the ROM windows), one handler (the TIA, the RIOT's
 * ports, the cartridge), or neither - looked up in the space
 * (gemu_space_run()) and kept while its generation holds. */
enum { SLOT_NEW, SLOT_RAM, SLOT_MAP, SLOT_SPACE };

static __attribute__((noinline)) struct A26Slot *bus_slot_fill(A26Core *c, uint16_t addr, bool write) {
    uint64_t gen = gemu_space_generation(c->bus);
    if (c->slot_gen != gen) {
        memset(c->slot, 0, sizeof c->slot);
        c->slot_gen = gen;
    }
    struct A26Slot *sl = &c->slot[write][(addr >> 6) & 127u];
    if (sl->kind == SLOT_NEW) {
        GemuSpaceRun r;
        sl->kind = SLOT_SPACE;
        if (gemu_space_run(c->bus, addr & 0x1FC0u, write, &r) && r.avail >= 64) {
            if (r.ram) { sl->ram = r.ram; sl->kind = SLOT_RAM; }
            else       { sl->map = r.map; sl->off = (uint32_t)r.off; sl->kind = SLOT_MAP; }
        }
    }
    return sl;
}

static inline struct A26Slot *bus_slot(Atari2600State *s, uint16_t addr, bool write) {
    A26Core *c = s->core;
    struct A26Slot *sl = &c->slot[write][(addr >> 6) & 127u];
    if (__builtin_expect(sl->kind == SLOT_NEW || c->slot_gen != gemu_space_generation(c->bus), 0))
        sl = bus_slot_fill(c, addr, write);
    return sl;
}

/* As before T2: a fast scheme's ROM is neither counted nor snooped; every
 * other access is counted, and snooped unless the scheme is a fast one. */
static uint8_t a26_read(uint16_t addr, void *ud) {
    Atari2600State *s = ud;
    if (!(s->bus_fast && (addr & 0x1000u))) bus_access(s, addr);
    struct A26Slot *sl = bus_slot(s, addr, false);
    uint8_t v;
    if (sl->kind == SLOT_RAM)      v = sl->ram[addr & 63u];
    else if (sl->kind == SLOT_MAP) v = sl->map->h.r8(sl->map->ud, sl->off + (addr & 63u));
    else                           v = gemu_space_read8(s->core->bus, addr);
    if (!s->bus_fast) a26_cart_snoop(&s->cart, addr, v, false);
    s->bus = v;
    gemu_monitor_check_read(s->monitor, addr);
    return v;
}

static void a26_write(uint16_t addr, uint8_t val, void *ud) {
    Atari2600State *s = ud;
    if (!s->bus_fast) bus_access(s, addr);
    struct A26Slot *sl = bus_slot(s, addr, true);
    if (sl->kind == SLOT_RAM)      sl->ram[addr & 63u] = val;
    else if (sl->kind == SLOT_MAP) sl->map->h.w8(sl->map->ud, sl->off + (addr & 63u), val);
    else                           gemu_space_write8(s->core->bus, addr, val);
    if (!s->bus_fast) a26_cart_snoop(&s->cart, addr, val, true);
    s->bus = val;
    gemu_monitor_check_write(s->monitor, addr);
}

/* Supercharger BIOS hotspot access to the console's RAM. */
static uint8_t ram_peek(void *ud, uint8_t addr) { return ((Atari2600State *)ud)->riot.ram[addr & 0x7Fu]; }
static void    ram_poke(void *ud, uint8_t addr, uint8_t val) { ((Atari2600State *)ud)->riot.ram[addr & 0x7Fu] = val; }

void a26_bus_attach(Atari2600State *s) {
    s->cpu.mem_read  = a26_read;
    s->cpu.mem_write = a26_write;
    s->cpu.mem_ud    = s;
    a26_cart_set_ram_access(ram_peek, ram_poke, s);
}

/* One instruction, then the WSYNC hold: RDY stays low until the TIA
 * reaches the start of the next scanline (the WSYNC write synced it). */
void a26_bus_step(Atari2600State *s) {
    mos6502_step(&s->cpu);
    if (s->wsync) {
        s->wsync = false;
        int left = (TIA_LINE_CLOCKS - s->tia.hctr) % TIA_LINE_CLOCKS;
        s->cpu.cycle_count += (uint64_t)(left + 2) / 3u;
    }
}

/* Power cycle (the console's own Reset is a switch the program reads). */
void a26_bus_power_on(Atari2600State *s) {
    pick_fast_path(s);
    a26_maps_sync(s);
    a26_bus_sync(s);
    tia_reset(&s->tia);
    m6532_reset(&s->riot, s->cpu.cycle_count);
    a26_cart_reset(&s->cart);
    s->wsync = false;
    mos6502_reset(&s->cpu);
}
