/* The 6530 RRIOT's I/O, timer and RAM.  Run: ninja check-m6530 */
#include "controller/m6530.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef struct { uint8_t pa_in, pa_out, pb_out; unsigned irq_calls; } Board;
static uint8_t read_pa(void *ud) { return ((Board *)ud)->pa_in; }
static void write_pa(void *ud, uint8_t v) { ((Board *)ud)->pa_out = v; }
static void write_pb(void *ud, uint8_t v) { ((Board *)ud)->pb_out = v; }
static void irq_changed(void *ud) { ((Board *)ud)->irq_calls++; }

int main(void) {
    Board b = { .pa_in = 0x5A };
    M6530 r = { .read_pa = read_pa, .write_pa = write_pa, .write_pb = write_pb,
                .irq_changed = irq_changed, .ud = &b };
    r.ram[10] = 0x77;
    m6530_reset(&r);
    CHECK(r.ram[10] == 0x77 && r.read_pa == read_pa);   /* RAM and wiring kept */
    CHECK(r.prescale == 1 && !r.running && !m6530_irq(&r));

    /* Ports: outputs from the register, inputs from the pins (pulled up
     * without a callback); the write callbacks get the register. */
    m6530_write(&r, 1, 0xF0, 0);
    m6530_write(&r, 0, 0xC3, 0);
    CHECK(b.pa_out == 0xC3);
    CHECK(m6530_read(&r, 0, 0) == 0xCA && m6530_read(&r, 1, 0) == 0xF0);
    m6530_write(&r, 3, 0x0F, 0);
    m6530_write(&r, 2, 0x96, 0);
    CHECK(b.pb_out == 0x96 && m6530_read(&r, 2, 0) == 0xF6);   /* PB pins: 1 */
    CHECK(m6530_read(&r, 0x12, 0) == m6530_read(&r, 2, 0));    /* A3..A0 only */

    /* Timer /8 loaded with 3 at cycle 100: underflow at 124. */
    m6530_write(&r, 5, 3, 100);
    CHECK(r.running && r.next_fire == 124);
    CHECK(m6530_read(&r, 4, 100) == 2 && m6530_read(&r, 4, 116) == 0);
    m6530_tick(&r, 123);
    CHECK(!r.irq_flag);
    m6530_tick(&r, 124);
    CHECK(r.irq_flag && !m6530_irq(&r));                 /* loaded via 4-7: no IRQ */
    CHECK(m6530_peek(&r, 7, 124) == 0x80);               /* flag | count 0 */
    CHECK(m6530_read(&r, 5, 132) == 0xFF);               /* on down from $FF */
    CHECK(r.irq_flag);                                   /* the count read keeps it */
    CHECK(m6530_read(&r, 6, 140) == (0x80 | 0x7E));      /* flag | $FE & $7F */
    CHECK(!r.irq_flag);                                  /* the status read clears it */
    CHECK(m6530_read(&r, 7, 141) == 0x7E);

    /* Loaded via C-F: the underflow drives IRQ; 0 counts as 256. */
    unsigned calls = b.irq_calls;
    m6530_write(&r, 0x0C, 0, 1000);
    CHECK(r.prescale == 1 && r.next_fire == 1256 && b.irq_calls > calls);
    m6530_tick(&r, 1256);
    CHECK(m6530_irq(&r));
    m6530_read(&r, 0x0E, 1257);
    CHECK(!m6530_irq(&r));
    /* /64 and /1024. */
    m6530_write(&r, 6, 2, 0);
    CHECK(r.next_fire == 128);
    m6530_write(&r, 0x0F, 1, 0);
    CHECK(r.next_fire == 1024 && r.irq_en);
    /* Writes to 8-B do nothing. */
    m6530_write(&r, 9, 0x55, 5);
    CHECK(r.next_fire == 1024 && r.reload == 1);

    printf("m6530: %u checks passed\n", checks);
    return 0;
}
