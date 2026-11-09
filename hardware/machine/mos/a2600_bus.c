/* Atari 2600 bus with a lazily-run TIA - see a2600_bus.h. */
#include "a2600_bus.h"
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

static uint8_t a26_read(uint16_t addr, void *ud) {
    Atari2600State *s = ud;
    uint8_t v;
    if (s->bus_fast && (addr & 0x1000u)) {
        uint16_t off = addr & 0x0FFFu;
        if (s->bus_fast == FAST_PLAIN) v = s->cart.rom[off & s->bus_fast_mask];
        else if (off < 0x0FF4u)        v = s->cart.rom[s->cart.bank[0] * 4096u + off];
        else                           v = a26_cart_read(&s->cart, addr, s->bus, s->cpu.cycle_count);
    } else {
        bus_access(s, addr);
        if (addr & 0x1000u)        v = a26_cart_read(&s->cart, addr, s->bus, s->cpu.cycle_count);
        else if (!(addr & 0x80u))  { a26_bus_sync(s); v = tia_read(&s->tia, addr, s->bus); }
        else if (!(addr & 0x200u)) v = s->riot.ram[addr & 0x7Fu];
        else                       v = m6532_read(&s->riot, addr, s->cpu.cycle_count);
        if (!s->bus_fast) a26_cart_snoop(&s->cart, addr, v, false);
    }
    s->bus = v;
    gemu_monitor_check_read(s->monitor, addr);
    return v;
}

static void a26_write(uint16_t addr, uint8_t val, void *ud) {
    Atari2600State *s = ud;
    if (!s->bus_fast) bus_access(s, addr);
    if (addr & 0x1000u)        a26_cart_write(&s->cart, addr, val, s->cpu.cycle_count);
    else if (!(addr & 0x80u))  { a26_bus_sync(s); s->wsync |= tia_write(&s->tia, addr, val); }
    else if (!(addr & 0x200u)) s->riot.ram[addr & 0x7Fu] = val;
    else                       m6532_write(&s->riot, addr, val, s->cpu.cycle_count);
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
    a26_bus_sync(s);
    tia_reset(&s->tia);
    m6532_reset(&s->riot, s->cpu.cycle_count);
    a26_cart_reset(&s->cart);
    s->wsync = false;
    mos6502_reset(&s->cpu);
}
