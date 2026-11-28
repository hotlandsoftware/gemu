/* The mos family's standalone core devices (stage M1): mos6502, pia6821,
 * m6530.  Run: ninja check-mos_devices */
#include "mos6502_dev.h"
#include "controller/pia6821_dev.h"
#include "controller/m6530_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static uint8_t ram[0x10000];
static GemuSpace *bus;
static uint8_t rd(uint16_t a, void *ud) { (void)ud; return gemu_space_read8(bus, a); }
static void wr(uint16_t a, uint8_t v, void *ud) { (void)ud; gemu_space_write8(bus, a, v); }

static Mos6502 *cpu;
static unsigned insns, stop_at;
static uint64_t cycles(void *ud) { (void)ud; return cpu->cycle_count; }
static void insn(void *ud) { (void)ud; mos6502_step(cpu); insns++; }
static bool done(void *ud) { (void)ud; return stop_at && insns >= stop_at; }

int main(void) {
    GemuMachine *m = gemu_machine_new("mos-devices");
    GemuDevice *c = gemu_device_add(m, &gemu_mos6502_type, "cpu", NULL, NULL);
    GemuDevice *c7 = gemu_device_add(m, &gemu_mos6502_type, "cpu7", NULL, "variant=6507");
    GemuDevice *pia = gemu_device_add(m, &gemu_pia6821_type, "pia", NULL, NULL);
    GemuDevice *riot = gemu_device_add(m, &gemu_m6530_type, "riot", NULL, NULL);
    CHECK(c && c7 && pia && riot);
    CHECK(!gemu_device_add(m, &gemu_mos6502_type, "bad", NULL, "variant=z80"));
    CHECK(gemu_machine_init(m));
    cpu = gemu_mos6502(c);
    CHECK(cpu->addr_mask == 0xFFFF && gemu_mos6502(c7)->addr_mask == 0x1FFF &&
          gemu_mos6502(c7)->no_int_pins);

    /* the bus: RAM, the PIA at $D010, the 6530 at $1700 */
    CHECK((bus = gemu_space_new(m, "program", 16, 8, GEMU_LITTLE_ENDIAN)));
    CHECK(gemu_space_map_ram(bus, 0, 0x10000, ram, 0, 0));
    CHECK(gemu_space_map(bus, 0xD010, 4, &gemu_pia6821_bus, pia, 1, 0));
    CHECK(gemu_space_map(bus, 0x1700, 16, &gemu_m6530_io, riot, 1, 0));
    gemu_m6530_set_cycles(riot, cycles, NULL);
    cpu->mem_read = rd;
    cpu->mem_write = wr;

    /* the PIA's and the 6530's IRQs, wire-ORed onto the CPU's irq; a
     * strobe line onto the PIA's CA1 */
    GemuDevice *root = gemu_machine_root(m);
    GemuLine *strobe = gemu_line_out(root, "strobe");
    CHECK(strobe && gemu_line_connect(strobe, pia, "ca1", 0));
    CHECK(gemu_line_connect(gemu_line_find_out(pia, "irqa"), c, "irq", 0));
    CHECK(gemu_line_connect(gemu_line_find_out(riot, "irq"), c, "irq", 0));
    static const GemuMos6502Hooks hooks = { .insn = insn, .done = done };
    CHECK(gemu_mos6502_exec(c, 1000000, &hooks));
    CHECK(gemu_machine_start(m));

    /* a program: INC $10; JMP $0200 (5 + 3 cycles a loop) */
    static const uint8_t prog[] = { 0xE6, 0x10, 0x4C, 0x00, 0x02 };
    memcpy(ram + 0x200, prog, sizeof prog);
    ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
    ram[0xFFFE] = 0x00; ram[0xFFFF] = 0x03;     /* IRQ: $0300 (RTI, never taken: I set) */
    ram[0x300] = 0x40;
    mos6502_reset(cpu);
    uint64_t c0 = cpu->cycle_count;

    /* the executor: 8,000 cycles of machine time run 1,000 loops */
    GemuTime t0 = gemu_machine_time(m);
    gemu_machine_run_until(m, t0 + gemu_clocks(1000000, 8000));
    CHECK(gemu_mos6502_ended(c) == GEMU_MOS6502_BUDGET);
    CHECK(cpu->cycle_count - c0 >= 8000 && cpu->cycle_count - c0 < 8010);
    CHECK(ram[0x10] == (uint8_t)(insns / 2));
    /* done stops the run, and the machine, after that instruction */
    stop_at = insns + 7;
    GemuTime t1 = gemu_machine_time(m);
    gemu_machine_run_until(m, t1 + gemu_clocks(1000000, 100000));
    CHECK(gemu_mos6502_ended(c) == GEMU_MOS6502_DONE && insns == stop_at);
    CHECK(gemu_machine_time(m) < t1 + gemu_clocks(1000000, 100));
    stop_at = 0;

    /* the PIA: CA1's rising edge with CRA = $07 (IRQ enabled, rising edge,
     * ORA selected) raises irqa, so the CPU's irq; reading ORA clears it */
    gemu_space_write8(bus, 0xD011, 0x07);
    CHECK(!cpu->irq);
    gemu_line_set(strobe, true);
    CHECK(gemu_line_get(gemu_line_find_out(pia, "irqa")) && cpu->irq);
    CHECK(gemu_space_read8(bus, 0xD011) & 0x80);
    (void)gemu_space_read8(bus, 0xD010);
    CHECK(!cpu->irq);
    gemu_line_set(strobe, false);

    /* the 6530: 10 at /1 through $170C (IRQ enabled) underflows 11 cycles
     * later; a tick after that raises its irq */
    gemu_space_write8(bus, 0x170C, 10);
    CHECK(!cpu->irq);
    cpu->cycle_count += 20;
    gemu_m6530_tick(riot);
    CHECK(gemu_line_get(gemu_line_find_out(riot, "irq")) && cpu->irq);
    CHECK(gemu_space_read8(bus, 0x170E) & 0x80);    /* reading the flag clears it */
    CHECK(!cpu->irq);

    /* state: save, change all three, load: back; save again: the same bytes */
    gemu_space_write8(bus, 0xD012, 0x55);
    gemu_m6530(riot)->ram[5] = 0x66;
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(m, &a, &na));
    uint16_t pc = cpu->PC;
    uint64_t cyc = cpu->cycle_count;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(1000000, 500));
    gemu_space_write8(bus, 0xD012, 0xAA);
    gemu_m6530(riot)->ram[5] = 0;
    CHECK(gemu_machine_load_state(m, a, na));
    CHECK(cpu->PC == pc && cpu->cycle_count == cyc && gemu_m6530(riot)->ram[5] == 0x66 &&
          gemu_pia6821(pia)->ddrb == 0x55);   /* CRB 0: $D012 is DDRB */
    CHECK(gemu_machine_save_state(m, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    free(a);
    free(b);

    /* info */
    char buf[256];
    gemu_device_describe(pia, buf, sizeof buf);
    CHECK(strstr(buf, "DDR 55"));
    gemu_device_describe(c, buf, sizeof buf);
    CHECK(strstr(buf, "PC "));
    gemu_machine_free(m);
    printf("mos_devices: %u checks passed\n", checks);
    return 0;
}
