/* GEMU's 86Box layer (compat/86box).  Run: ninja check-86box
 *
 * A small machine - an "io" space, GEMU's i8259, a probe counting IRQ edges -
 * with 86Box v6.0 drivers on it as core devices: the Novell key card from
 * third_party unchanged, and a test driver below written against the v6.0
 * API as 86Box drivers are (options, stacked I/O handlers, timers short,
 * periodic and over a second, edge and level IRQs, reset, close), checked
 * against the core: exact timer moments, save and load. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/pic.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include "gemu_86box.h"
#include "controller/i8259_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <math.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static GemuMachine *m;
#define BUS_HZ 4772727u

/* ── A driver, as 86Box's are ─────────────────────────────────────────────── */

typedef struct tdev_t {
    uint16_t   base;
    int        irq, period;
    char       label[16];
    uint8_t    reg, irq_state;
    pc_timer_t tick, once, big;
    unsigned   ticks, onces, bigs;
    GemuTime   tick_at[64], last_at, big_at;   /* test only: when they fired */
    uint64_t   tick_tsc[64], big_t0;
} tdev_t;

static unsigned closed;

static uint8_t tdev_in(uint16_t port, void *priv) {
    tdev_t *dev = (tdev_t *) priv;
    switch (port - dev->base) {
        case 0:  return dev->reg;
        case 1:  return (uint8_t) dev->ticks;
        case 2:  return (uint8_t) timer_get_remaining_us(&dev->once);
        default: return dev->irq_state;
    }
}

static void tdev_out(uint16_t port, uint8_t val, void *priv);

static void tdev_outw(uint16_t port, uint16_t val, void *priv) {
    tdev_t *dev = (tdev_t *) priv;
    if (port == dev->base)
        dev->reg = (uint8_t) (val ^ (val >> 8));
}

static void tdev_out(uint16_t port, uint8_t val, void *priv) {
    tdev_t *dev = (tdev_t *) priv;
    switch (port - dev->base) {
        case 0:
            dev->reg = val;
            break;
        case 1:                                          /* IRQ requests */
            if (val == 1)      picint(1 << dev->irq);
            else if (val == 0) picintc(1 << dev->irq);
            else if (val == 3) picintlevel(1 << dev->irq, &dev->irq_state);
            else if (val == 2) picintclevel(1 << dev->irq, &dev->irq_state);
            break;
        case 2:                                          /* the one-shot */
            if (val == 0xFF) {                           /* 2.5 s */
                dev->big_t0 = tsc;
                timer_on_auto(&dev->big, 2500000.0);
            } else if (val)
                timer_on_auto(&dev->once, (double) val);
            else
                timer_disable(&dev->once);
            break;
        case 3:                                          /* take port base off */
            io_removehandler(dev->base, 1, tdev_in, NULL, NULL, tdev_out, tdev_outw, NULL, dev);
            break;
    }
}

static void tdev_tick(void *priv) {
    tdev_t *dev = (tdev_t *) priv;
    if (dev->ticks < 64) {
        dev->tick_at[dev->ticks]  = gemu_now(m);
        dev->tick_tsc[dev->ticks] = tsc;
    }
    dev->last_at = gemu_now(m);
    dev->ticks++;
    timer_advance_u64(&dev->tick, dev->period * TIMER_USEC);
}

static void tdev_once(void *priv) { ((tdev_t *) priv)->onces++; }

static void tdev_big(void *priv) {
    tdev_t *dev = (tdev_t *) priv;
    dev->bigs++;
    dev->big_at = gemu_now(m);
}

static void *tdev_init(UNUSED(const device_t *info)) {
    tdev_t *dev = calloc(1, sizeof(tdev_t));
    dev->base   = (uint16_t) device_get_config_hex16("base");
    dev->irq    = device_get_config_int("irq");
    dev->period = device_get_config_int("period");
    strncpy(dev->label, device_get_config_string("label"), sizeof(dev->label) - 1);
    io_sethandler(dev->base, 4, tdev_in, NULL, NULL, tdev_out, tdev_outw, NULL, dev);
    timer_add(&dev->tick, tdev_tick, dev, 0);
    timer_add(&dev->once, tdev_once, dev, 0);
    timer_add(&dev->big, tdev_big, dev, 0);
    if (device_get_config_int("ticking"))
        timer_set_delay_u64(&dev->tick, dev->period * TIMER_USEC);
    pclog("tdev: %s at %03X, IRQ %d\n", dev->label, dev->base, dev->irq);
    return dev;
}

static void tdev_close(void *priv) {
    closed++;
    free(priv);
}

static void tdev_reset(void *priv) { ((tdev_t *) priv)->reg = 0x55; }

static const device_config_t tdev_config[] = {
    { .name = "base", .description = "Address", .type = CONFIG_HEX16, .default_int = 0x310,
      .selection = { { .description = "300H", .value = 0x300 },
                     { .description = "310H", .value = 0x310 },
                     { .description = "320H", .value = 0x320 },
                     { .description = "" } } },
    { .name = "irq", .description = "IRQ", .type = CONFIG_SELECTION, .default_int = 5,
      .selection = { { .description = "IRQ 3", .value = 3 },
                     { .description = "IRQ 5", .value = 5 },
                     { .description = "IRQ 7", .value = 7 },
                     { .description = "" } } },
    { .name = "period", .description = "Tick (us)", .type = CONFIG_SPINNER, .default_int = 100,
      .spinner = { .min = 10, .max = 1000, .step = 10 } },
    { .name = "ticking", .description = "Tick", .type = CONFIG_BINARY, .default_int = 1 },
    { .name = "label", .description = "Label", .type = CONFIG_STRING, .default_string = "hello" },
    { .name = "", .description = "", .type = CONFIG_END }
};

static const device_t tdev_device = {
    .name = "Test card", .internal_name = "tdev", .flags = DEVICE_ISA, .local = 0,
    .init = tdev_init, .close = tdev_close, .reset = tdev_reset, .available = NULL,
    .speed_changed = NULL, .force_redraw = NULL, .config = tdev_config
};

/* The port: what the driver keeps is its state (the timers register
 * themselves). */
static bool tdev_state(GemuDevice *d, void *priv) {
    tdev_t *dev = priv;
    return GEMU_STATE_VAR(d, "reg", dev->reg) && GEMU_STATE_VAR(d, "irq_state", dev->irq_state) &&
           GEMU_STATE_VAR(d, "ticks", dev->ticks) && GEMU_STATE_VAR(d, "onces", dev->onces) &&
           GEMU_STATE_VAR(d, "bigs", dev->bigs);
}
GEMU_86BOX_PORT(tdev_type, tdev_device, "tdev", tdev_state, NULL, false);

/* ── The machine ──────────────────────────────────────────────────────────── */

/* Counts rising edges on in0..in7 */
typedef struct { unsigned rises[8]; } Probe;
static void probe_cb(void *ud, int idx, bool level) {
    if (level) ((Probe *)ud)->rises[idx]++;
}
static bool probe_init(GemuDevice *d) {
    Probe *p = d->priv;
    for (int i = 0; i < 8; i++) {
        char n[8];
        snprintf(n, sizeof n, "in%d", i);
        if (!gemu_line_in(d, n, probe_cb, p, i)) return false;
    }
    return GEMU_STATE_ARRAY(d, "rises", p->rises);
}
static const GemuDeviceType probe_type = { .name = "probe", .size = sizeof(Probe), .init = probe_init };
static const GemuDeviceType isa_type = { .name = "isa", .stateless = true };

static GemuSpace *io;
static GemuDevice *isa, *pic;
static Probe *probe;

static void build(void) {
    if (m) gemu_machine_free(m);
    m = gemu_machine_new("box");
    io = gemu_space_new(m, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    pic = gemu_device_add_type(m, "i8259", "pic", NULL, NULL);
    GemuDevice *pr = gemu_device_add(m, &probe_type, "probe", NULL, NULL);
    isa = gemu_device_add(m, &isa_type, "isa", NULL, NULL);
    CHECK(io && pic && pr && isa);
    gemu_device_set_clock(isa, BUS_HZ);
    probe = pr->priv;
}

static void start(void) {
    CHECK(gemu_machine_init(m));
    CHECK(gemu_space_map(io, 0x20, 2, &gemu_i8259_io, pic, 0, 0));
    for (GemuDevice *d = isa->first_child; d; d = d->next_sibling) {
        CHECK(gemu_86box_connect_irqs(d, "pic:ir%u", 8));
        CHECK(gemu_86box_connect_irqs(d, "probe:in%u", 8));
    }
    CHECK(gemu_machine_start(m));
    gemu_space_write8(io, 0x20, 0x13);        /* edge, single, ICW4 */
    gemu_space_write8(io, 0x21, 0x08);
    gemu_space_write8(io, 0x21, 0x09);
    gemu_space_write8(io, 0x21, 0x00);        /* all unmasked */
}

static uint8_t irr(void) {
    gemu_space_write8(io, 0x20, 0x0A);        /* OCW3: read IRR */
    return gemu_space_read8(io, 0x20);
}

/* When the k-th tick (from 1) is due: the timestamp, rounded up */
static GemuTime tick_due(uint64_t start_ts, unsigned k, unsigned period_us) {
    unsigned __int128 ts = ((unsigned __int128)start_ts << 32) +
                           (unsigned __int128)k * period_us * TIMER_USEC;
    unsigned __int128 den = (unsigned __int128)BUS_HZ << 32;
    return (GemuTime)((ts * GEMU_PS_PER_SEC + den - 1) / den);
}

/* ── The AdLib ──────────────────────────────────────────────────────────── */

static float got[48000 * 4];
static size_t n_got;
static void sink(void *ud, const float *smp, size_t frames) {
    (void)ud;
    for (size_t i = 0; i < frames && n_got < sizeof got / sizeof got[0]; i++)
        got[n_got++] = smp[i];
}

static void opl(uint8_t reg, uint8_t val) {
    gemu_space_write8(io, 0x388, reg);
    gemu_space_write8(io, 0x389, val);
}

/* Run to t, mixing every 10 ms as a machine's frames would */
static void run_mixing(GemuTime t) {
    while (gemu_machine_time(m) < t) {
        GemuTime next = gemu_machine_time(m) + GEMU_MSEC(10);
        gemu_machine_run_until(m, next < t ? next : t);
        gemu_mixer_run(m);
    }
}

/* The frequency of what the sink got from sample a on: rising zero
 * crossings over the span */
static double freq_of(size_t a, size_t b) {
    double first = -1, last = -1;
    unsigned n = 0;
    for (size_t i = a + 1; i < b; i++)
        if (got[i - 1] < 0 && got[i] >= 0) {
            double x = (double)(i - 1) + got[i - 1] / (got[i - 1] - got[i]);
            if (first < 0) first = x;
            last = x;
            n++;
        }
    return n > 1 ? (n - 1) * 48000.0 / (last - first) : 0;
}

/* An AdLib machine, the note A4 on channel 0 from t = 1 ms */
static GemuDevice *adlib_machine(void) {
    build();
    GemuDevice *ad = gemu_device_add_type(m, "adlib", "adlib", isa, NULL);
    CHECK(ad);
    start();
    n_got = 0;
    gemu_mixer_set_output(m, 48000, 1, sink, NULL);
    gemu_mixer_run(m);
    return ad;
}
static void adlib_note(void) {
    static const uint8_t inst[][2] = {
        { 0x01, 0x20 }, { 0x20, 0x01 }, { 0x40, 0x10 }, { 0x60, 0xF0 }, { 0x80, 0x77 },
        { 0x23, 0x01 }, { 0x43, 0x00 }, { 0x63, 0xF0 }, { 0x83, 0x77 }, { 0xC0, 0x01 },
    };
    for (unsigned i = 0; i < sizeof inst / sizeof inst[0]; i++)
        opl(inst[i][0], inst[i][1]);
    opl(0xA0, 0x44);                          /* A4: block 4, F-number 0x244 */
    opl(0xB0, 0x20 | (4 << 2) | 0x02);        /* key on */
}

int main(void) {
    /* ── The key card, unchanged from 86Box ── */
    build();
    GemuDevice *key = gemu_device_add_type(m, "novellkeycard", "key", isa,
                                           "serial_number=123456780ABC");
    CHECK(key);
    start();
    static const uint8_t want[6] = { 0x4F, 0x2F, 0x9B, 0xE7, 0xD8, 0xCA };
    for (int i = 0; i < 6; i++)
        CHECK(gemu_space_read8(io, 0x23A + i) == want[i]);
    char buf[256];
    gemu_device_describe(key, buf, sizeof buf);
    CHECK(!strcmp(buf, "Novell NetWare 2.x Key Card (86Box v6.0: novellkeycard)"));
    gemu_device_destroy(key);                 /* its handlers go with it */
    CHECK(gemu_space_read8(io, 0x23A) == 0xFF);
    build();
    CHECK(gemu_device_add_type(m, "novellkeycard", "key", isa, NULL));
    start();
    CHECK(gemu_space_read8(io, 0x23C) == 0xFF);           /* serial 000000000000 */
    /* its options are the config table's */
    CHECK(!gemu_device_add_type(m, "novellkeycard", "k2", isa, "serial=1"));
    CHECK(strstr(gemu_machine_error(m), "serial"));

    /* ── Options ── */
    build();
    GemuDevice *a = gemu_device_add(m, &tdev_type, "a", isa, "label=first,period=250");
    GemuDevice *b = gemu_device_add(m, &tdev_type, "b", isa, "base=0x310,irq=3,ticking=off");
    CHECK(a && b);
    CHECK(!gemu_device_add(m, &tdev_type, "c", isa, "base=0x330"));       /* not a choice */
    CHECK(!gemu_device_add(m, &tdev_type, "c", isa, "period=5"));         /* below the spinner */
    CHECK(!gemu_device_add(m, &tdev_type, "c", isa, "irq=4"));
    start();
    tdev_t *ta = gemu_86box_priv(a), *tb = gemu_86box_priv(b);
    CHECK(ta->base == 0x310 && ta->irq == 5 && ta->period == 250 && !strcmp(ta->label, "first"));
    CHECK(tb->base == 0x310 && tb->irq == 3 && tb->period == 100 && !strcmp(tb->label, "hello"));
    CHECK(ta->reg == 0x55 && tb->reg == 0x55);            /* reset at power-on */

    /* ── I/O: stacked on one port, ANDed reads, widths, removal ── */
    gemu_space_write8(io, 0x310, 0xF0);                   /* both take it */
    CHECK(ta->reg == 0xF0 && tb->reg == 0xF0);
    ta->reg = 0xF0, tb->reg = 0x3C;
    CHECK(gemu_space_read8(io, 0x310) == 0x30);
    gemu_space_write16(io, 0x310, 0x1234);                /* their outw */
    CHECK(ta->reg == 0x26 && tb->reg == 0x26);
    CHECK(gemu_space_read16(io, 0x310) == (uint16_t)(0x26 | (0x00 << 8)));   /* two inb: reg, ticks */
    gemu_space_write8(io, 0x313, 0);                      /* both take their port 310 off */
    CHECK(gemu_space_read8(io, 0x310) == 0xFF);
    gemu_space_write8(io, 0x310, 0x11);
    CHECK(ta->reg == 0x26);
    CHECK(gemu_space_read8(io, 0x311) == 0);              /* the rest stay */

    /* ── IRQs: edges (each picint a request), level wired-OR ── */
    gemu_space_write8(io, 0x311, 1);                      /* both: picint (5 and 3) */
    CHECK(probe->rises[5] == 1 && probe->rises[3] == 1 && (irr() & 0x28) == 0x28);
    gemu_space_write8(io, 0x311, 1);                      /* again: another edge */
    CHECK(probe->rises[5] == 2 && probe->rises[3] == 2);
    gemu_space_write8(io, 0x311, 0);                      /* picintc */
    CHECK(!gemu_line_get(gemu_line_find_out(a, "irq5")));
    /* level: a and b both on IRQ 7 */
    ta->irq = tb->irq = 7;
    gemu_space_write8(io, 0x311, 3);                      /* both raise */
    CHECK(ta->irq_state == 1 && tb->irq_state == 1 && probe->rises[7] == 1);
    CHECK(gemu_space_read8(io, 0x313) == 1);
    ta->irq = 0;                                          /* a lowers IRQ 0 (low anyway), */
    gemu_space_write8(io, 0x311, 2);                      /* b lowers its 7 */
    CHECK(ta->irq_state == 0 && tb->irq_state == 0);
    CHECK(gemu_line_get(gemu_line_find_out(a, "irq7")));  /* a still holds 7 */
    CHECK(!gemu_line_get(gemu_line_find_out(b, "irq7")));
    CHECK(probe->rises[7] == 1);                          /* wired-OR: no new edge */

    /* ── Timers: periodic to the exact moment, one-shot, over a second ── */
    build();
    a = gemu_device_add(m, &tdev_type, "a", isa, "period=100");
    CHECK(a);
    start();
    ta = gemu_86box_priv(a);
    gemu_machine_run_until(m, GEMU_MSEC(5));
    CHECK(ta->ticks == 50);
    for (unsigned k = 1; k <= 50; k++) {
        CHECK(ta->tick_at[k - 1] == tick_due(0, k, 100));
        CHECK(ta->tick_tsc[k - 1] == gemu_cycles_in(BUS_HZ, ta->tick_at[k - 1]));
    }
    /* 32.32: 100 us is 477.2727 clocks; no drift after 50 */
    CHECK(ta->tick_at[49] >= GEMU_MSEC(5) - 1000 && ta->tick_at[49] <= GEMU_MSEC(5) + 1000);
    gemu_space_write8(io, 0x312, 200);                    /* once in 200 us */
    CHECK(timer_is_enabled(&ta->once));
    gemu_machine_run_until(m, GEMU_MSEC(5) + GEMU_USEC(50));
    uint8_t left = gemu_space_read8(io, 0x312);           /* timer_get_remaining_us */
    CHECK(left == 149 || left == 150);
    gemu_machine_run_until(m, GEMU_MSEC(6));
    CHECK(ta->onces == 1 && !timer_is_enabled(&ta->once));
    gemu_space_write8(io, 0x312, 100);
    gemu_machine_run_until(m, GEMU_MSEC(6) + GEMU_USEC(50));
    gemu_space_write8(io, 0x312, 0);                      /* timer_disable before it's due */
    gemu_machine_run_until(m, GEMU_MSEC(8));
    CHECK(ta->onces == 1);
    /* 2.5 s: three core timer runs (1 s, 1 s, 0.5 s), one callback */
    {
        gemu_space_write8(io, 0x312, 0xFF);
        uint64_t t0 = ta->big_t0;
        CHECK(t0 == gemu_cycles_in(BUS_HZ, GEMU_MSEC(8)));
        gemu_machine_run_until(m, GEMU_SEC(4));
        CHECK(ta->bigs == 1);
        /* 1 s + 1 s + 0.5 s of TIMER_USEC steps from t0 */
        unsigned __int128 ts = ((unsigned __int128)t0 << 32) +
                               (unsigned __int128)2000000u * TIMER_USEC +
                               (unsigned __int128)(uint64_t)(500000.0 * (double)TIMER_USEC);
        unsigned __int128 den = (unsigned __int128)BUS_HZ << 32;
        CHECK(ta->big_at == (GemuTime)((ts * GEMU_PS_PER_SEC + den - 1) / den));
    }

    /* ── Save state: the timers mid-period, the driver's fields ── */
    gemu_machine_run_until(m, GEMU_SEC(4) + GEMU_USEC(37));
    gemu_space_write8(io, 0x312, 150);
    gemu_space_write8(io, 0x310, 0x9A);
    uint8_t *st;
    size_t stn;
    CHECK(gemu_machine_save_state(m, &st, &stn));
    unsigned t_before = ta->ticks;
    gemu_machine_run_until(m, GEMU_SEC(4) + GEMU_MSEC(2));
    unsigned t_first = ta->ticks, o_first = ta->onces;
    GemuTime last_first = ta->last_at;
    gemu_space_write8(io, 0x310, 0x00);
    CHECK(gemu_machine_load_state(m, st, stn));
    CHECK(ta->ticks == t_before && ta->reg == 0x9A && timer_is_enabled(&ta->once));
    gemu_machine_run_until(m, GEMU_SEC(4) + GEMU_MSEC(2));
    CHECK(ta->ticks == t_first && ta->onces == o_first && o_first == 2);
    CHECK(ta->last_at == last_first);
    CHECK(t_first - t_before == 20);                      /* 2 ms of 100 us */
    free(st);

    /* ── Close, with the device ── */
    closed = 0;
    gemu_device_destroy(a);
    CHECK(closed == 1 && gemu_space_read8(io, 0x311) == 0xFF);

    /* ── The AdLib: its OPL2 a sub-device, detected by its timers, a note
     *    at its pitch, sound that carries on exactly across a load ── */
    GemuDevice *ad = adlib_machine();
    GemuDevice *chip = gemu_device_find(m, "isa/adlib/ym3812_nuked_opl2");
    CHECK(chip);
    gemu_device_describe(chip, buf, sizeof buf);
    CHECK(strstr(buf, "YM3812"));
    /* AdLib's documented detection: timer 1 overflows after 80 us */
    opl(0x04, 0x60);
    opl(0x04, 0x80);
    CHECK((gemu_space_read8(io, 0x388) & 0xE0) == 0x00);
    opl(0x02, 0xFF);
    opl(0x04, 0x21);
    gemu_machine_run_until(m, gemu_machine_time(m) + GEMU_USEC(70));
    CHECK((gemu_space_read8(io, 0x388) & 0xE0) == 0x00);
    gemu_machine_run_until(m, gemu_machine_time(m) + GEMU_USEC(20));
    CHECK((gemu_space_read8(io, 0x388) & 0xE0) == 0xC0);
    opl(0x04, 0x60);
    opl(0x04, 0x80);
    CHECK((gemu_space_read8(io, 0x388) & 0xE0) == 0x00);
    /* silence, then A4 */
    run_mixing(GEMU_MSEC(1));
    size_t quiet = n_got;
    for (size_t i = 0; i < quiet; i++)
        CHECK(got[i] == 0.0f);
    adlib_note();
    run_mixing(GEMU_MSEC(150));
    double f = freq_of(quiet + 2400, n_got);
    float peak = 0;
    for (size_t i = quiet; i < n_got; i++)
        peak = fabsf(got[i]) > peak ? fabsf(got[i]) : peak;
    if (fabs(f - 440.0) > 1.0 || peak < 0.05f)
        fprintf(stderr, "A4: %.2f Hz, peak %.3f\n", f, peak);
    CHECK(fabs(f - 440.0) <= 1.0 && peak >= 0.05f);
    printf("adlib: A4 at %.2f Hz, peak %.3f\n", f, peak);
    /* save mid-note, 5 ms after the last mix: the chip has made its sound
     * only that far (pre_save brings it up to the save); the sound after
     * a load - from the load on - is the sound that was */
    size_t from = n_got;                      /* 150 ms */
    gemu_machine_run_until(m, GEMU_MSEC(155));
    uint8_t *as;
    size_t asn;
    CHECK(gemu_machine_save_state(m, &as, &asn));
    const size_t skip = 48 * 5;               /* 150-155 ms: not heard after a load */
    opl(0xA0, 0x59);                          /* C5 at 160 ms */
    gemu_machine_run_until(m, GEMU_MSEC(160));
    opl(0xB0, 0x20 | (5 << 2) | 0x01);
    run_mixing(GEMU_MSEC(250));
    size_t len = n_got - from;
    static float first[48000];
    CHECK(len < 48000);
    memcpy(first, got + from, len * sizeof *first);
    float loud = 0;
    for (size_t i = 0; i < len; i++)
        loud = fabsf(first[i]) > loud ? fabsf(first[i]) : loud;
    CHECK(loud >= 0.05f);                     /* what's compared isn't silence */
    CHECK(fabs(freq_of(from + 48000 * 15 / 1000, n_got) - 523.25) < 2.0);   /* C5 */
    CHECK(gemu_machine_load_state(m, as, asn));
    n_got = from;
    opl(0xA0, 0x59);
    gemu_machine_run_until(m, GEMU_MSEC(160));
    opl(0xB0, 0x20 | (5 << 2) | 0x01);
    run_mixing(GEMU_MSEC(250));
    CHECK(n_got - from == len - skip &&
          !memcmp(first + skip, got + from, (len - skip) * sizeof *first));
    /* and into a new machine */
    adlib_machine();
    CHECK(gemu_machine_load_state(m, as, asn));
    n_got = from;
    gemu_mixer_run(m);
    opl(0xA0, 0x59);
    gemu_machine_run_until(m, GEMU_MSEC(160));
    opl(0xB0, 0x20 | (5 << 2) | 0x01);
    run_mixing(GEMU_MSEC(250));
    CHECK(n_got - from == len - skip &&
          !memcmp(first + skip, got + from, (len - skip) * sizeof *first));
    free(as);
    (void)ad;
    ad = gemu_device_find(m, "isa/adlib");
    gemu_device_destroy(ad);                  /* the chip goes with it */
    CHECK(!gemu_device_find(m, "isa/adlib/ym3812_nuked_opl2"));
    CHECK(gemu_space_read8(io, 0x388) == 0xFF);

    gemu_machine_free(m);
    m = NULL;
    printf("86box: %u checks passed\n", checks);
    return 0;
}
