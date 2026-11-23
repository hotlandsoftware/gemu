/* Port 61h/80h and the clock divider.  Run: ninja check-pc_sysctl */
#include "controller/pc_sysctl.h"
#include "timer/clock_div.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void test_clock_div(void) {
    ClockDiv d;
    clock_div_init(&d, 733084000, 1193182, 1000);
    CHECK(clock_div_advance(&d, 1000) == 0);
    /* One second of the source is exactly one second of the target, in
     * any number of steps, with nothing lost to rounding. */
    uint64_t ticks = 0, now = 1000;
    for (unsigned i = 0; i < 1000; i++) {
        now += 733084;
        ticks += clock_div_advance(&d, now);
    }
    CHECK(ticks == 1193182);
    now += 733084000;
    CHECK(clock_div_advance(&d, now) == 1193182);
    /* Backwards: restart there, no ticks; rebase skips time. */
    CHECK(clock_div_advance(&d, now - 5000000) == 0);
    CHECK(clock_div_advance(&d, now - 5000000 + 614) == 0);   /* 0.999 ticks */
    clock_div_rebase(&d, now + 733084000);
    CHECK(clock_div_advance(&d, now + 733084000 + 1) == 1);   /* the fraction kept */
}

static void test_sysctl(void) {
    I8253 pit = {0};
    i8253_reset(&pit);
    PcSysctl s;
    pc_sysctl_init(&s, &pit);
    uint8_t v = 0xEE;
    CHECK(pc_sysctl_io_read(&s, 0x61, 0, &v) && (v & 0x1F) == 0);   /* bit 5: an unprogrammed channel */
    CHECK(!pc_sysctl_io_read(&s, 0x80, 0, &v) && !pc_sysctl_io_read(&s, 0x62, 0, &v));
    CHECK(!pc_sysctl_io_write(&s, 0x62, 0));

    /* The latch keeps bits 0-3; bit 4 is the refresh toggle. */
    CHECK(pc_sysctl_io_write(&s, 0x61, 0xFE));
    CHECK(pc_sysctl_io_read(&s, 0x61, 0, &v) && (v & 0x1F) == 0x0E);
    CHECK(pc_sysctl_io_read(&s, 0x61, 18, &v) && (v & 0x1F) == 0x1E);
    CHECK(pc_sysctl_io_read(&s, 0x61, 36, &v) && (v & 0x1F) == 0x0E);

    /* Bit 0 gates channel 2; bit 5 reads its output (mode 0 counts down). */
    i8253_io_write(&pit, 0x43, 0xB0);                 /* ch2, lo/hi, mode 0 */
    i8253_io_write(&pit, 0x42, 10);
    i8253_io_write(&pit, 0x42, 0);
    i8253_tick(&pit, 50);
    CHECK(pc_sysctl_io_read(&s, 0x61, 0, &v) && !(v & 0x20));   /* not gated */
    pc_sysctl_io_write(&s, 0x61, 0x01);
    i8253_tick(&pit, 50);
    CHECK(pc_sysctl_io_read(&s, 0x61, 0, &v) && (v & 0x21) == 0x21);

    /* POST: write-only, kept; reset clears it, the latch and the gate. */
    CHECK(pc_sysctl_io_write(&s, 0x80, 0xA5) && s.post == 0xA5);
    pc_sysctl_reset(&s);
    CHECK(s.post == 0 && pc_sysctl_io_read(&s, 0x61, 0, &v) && (v & 0x0F) == 0);
}

int main(void) {
    test_clock_div();
    test_sysctl();
    printf("pc_sysctl: %u checks passed\n", checks);
    return 0;
}
