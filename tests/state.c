/* Save state (core/src/dev/state.c).  Run: ninja check-state */
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static unsigned post_loads, pre_saves;

/* ── counter: a clock timer ticks a count and toggles a line ── */
static const GemuOptionDef counter_opts[] = {
    { "hz", "tick rate", GEMU_OPT_INT, "1000", 1, 1000000000, NULL },
    { "timers", "how many timers", GEMU_OPT_INT, "1", 1, 2, NULL },
    { "regs", "registers", GEMU_OPT_INT, "8", 1, 8, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
typedef struct {
    uint32_t  count;
    uint16_t  regs[8];
    uint8_t   mode;
    uint32_t  derived;          /* worked out from count in post_load */
    GemuTimer *t, *t2;
    GemuLine  *out;
} Counter;

static void counter_tick(void *ud, int param) {
    GemuDevice *d = ud;
    Counter *c = d->priv;
    c->count += (uint32_t)param;
    c->regs[c->count % 8] ^= (uint16_t)c->count;
    gemu_line_set(c->out, c->count & 1);
}
static bool counter_init(GemuDevice *d) {
    Counter *c = d->priv;
    GEMU_STATE_VAR(d, "count", c->count);
    gemu_state_add(d, "regs", c->regs, sizeof c->regs[0], (size_t)gemu_opt_int(d, "regs"));
    GEMU_STATE_VAR(d, "mode", c->mode);
    c->t = gemu_timer_new(d, counter_tick, d);
    if (gemu_opt_int(d, "timers") == 2)
        c->t2 = gemu_timer_new(d, counter_tick, d);
    c->out = gemu_line_out(d, "out");
    return c->t && c->out;
}
static void counter_reset(GemuDevice *d, GemuResetKind k) {
    (void)k;
    Counter *c = d->priv;
    gemu_timer_start_clocks(c->t, (uint64_t)gemu_opt_int(d, "hz"), 1, 1, 1);
}
static void counter_pre_save(GemuDevice *d) { ((Counter *)d->priv)->mode = 0x5A; pre_saves++; }
static void counter_post_load(GemuDevice *d) {
    Counter *c = d->priv;
    c->derived = c->count * 3;
    post_loads++;
}
static const GemuDeviceType counter_type = {
    .name = "counter", .size = sizeof(Counter), .options = counter_opts,
    .init = counter_init, .reset = counter_reset,
    .pre_save = counter_pre_save, .post_load = counter_post_load,
};

/* ── sink: counts rising edges on an input ── */
typedef struct { uint32_t edges; unsigned calls; } Sink;
static void sink_in(void *ud, int idx, bool level) {
    (void)idx;
    Sink *s = ((GemuDevice *)ud)->priv;
    s->calls++;
    if (level)
        s->edges++;
}
static bool sink_init(GemuDevice *d) {
    Sink *s = d->priv;
    GEMU_STATE_VAR(d, "edges", s->edges);
    return gemu_line_in(d, "in", sink_in, d, 0) != NULL;
}
static const GemuDeviceType sink_type = {
    .name = "sink", .size = sizeof(Sink), .init = sink_init,
};

/* ── cpu: an executor that sums its cycles into a register ── */
typedef struct { uint64_t acc; GemuExecutor *e; } Cpu;
static uint64_t cpu_run(void *ud, uint64_t budget) {
    Cpu *c = ud;
    uint64_t done = 0;
    while (done < gemu_executor_budget(c->e)) {
        done += 3;
        c->acc = c->acc * 31 + done;
    }
    (void)budget;
    return done;
}
static const GemuExecutorOps cpu_ops = { cpu_run, NULL };
static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    GEMU_STATE_VAR(d, "acc", c->acc);
    c->e = gemu_executor_new(d, 3579545, &cpu_ops, c);
    return c->e != NULL;
}
static const GemuDeviceType cpu_type = { .name = "cpu", .size = sizeof(Cpu), .init = cpu_init };

/* ── cart: variable-size data through a custom item ── */
static const GemuOptionDef cart_opts[] = {
    { "size", "RAM size", GEMU_OPT_SIZE, "1K", 1, 1 << 20, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
typedef struct { uint8_t *ram; uint32_t size; } Cart;
static void cart_save(void *ud, GemuStateWriter *w) {
    Cart *c = ud;
    gemu_state_write_u32(w, c->size);
    gemu_state_write(w, c->ram, c->size);
}
static bool cart_check(void *ud, GemuStateReader *r) {
    uint32_t n;
    return gemu_state_read_u32(r, &n) && n == ((Cart *)ud)->size && gemu_state_left(r) == n;
}
static bool cart_load(void *ud, GemuStateReader *r) {
    Cart *c = ud;
    uint32_t n;
    return gemu_state_read_u32(r, &n) && n == c->size && gemu_state_read(r, c->ram, n);
}
static bool cart_init(GemuDevice *d) {
    Cart *c = d->priv;
    c->size = (uint32_t)gemu_opt_size(d, "size");
    c->ram = calloc(1, c->size);
    return c->ram && gemu_state_add_custom(d, "ram", cart_save, cart_check, cart_load, c);
}
static void cart_destroy(GemuDevice *d) { free(((Cart *)d->priv)->ram); }
static const GemuDeviceType cart_type = {
    .name = "cart", .size = sizeof(Cart), .options = cart_opts,
    .init = cart_init, .destroy = cart_destroy,
};

static const GemuDeviceType bus_type = { .name = "bus", .stateless = true };

/* Types that get save state wrong */
static bool nostate_init(GemuDevice *d) { (void)d; return true; }
static const GemuDeviceType nostate_type = { .name = "nostate", .size = 4, .init = nostate_init };
static bool dup_init(GemuDevice *d) {
    gemu_state_add(d, "x", d->priv, 4, 1);
    gemu_state_add(d, "x", d->priv, 4, 1);
    return true;
}
static const GemuDeviceType dup_type = { .name = "dup", .size = 4, .init = dup_init };
static bool zero_init(GemuDevice *d) { gemu_state_add(d, "x", d->priv, 0, 1); return true; }
static const GemuDeviceType zero_type = { .name = "zero", .size = 4, .init = zero_init };

/* ── The test machine ── */
typedef struct {
    GemuMachine *m;
    GemuDevice *pit, *pic, *cpu, *cart;
} Rig;

static Rig build(const char *name, const char *pit_opts, const char *cart_opts_s, bool extra) {
    Rig r = { gemu_machine_new(name), 0, 0, 0, 0 };
    GemuDevice *board = gemu_device_add(r.m, &bus_type, "board", NULL, NULL);
    r.pit = gemu_device_add(r.m, &counter_type, "pit", board, pit_opts);
    r.pic = gemu_device_add(r.m, &sink_type, "pic", board, NULL);
    r.cpu = gemu_device_add(r.m, &cpu_type, "cpu", NULL, NULL);
    r.cart = gemu_device_add(r.m, &cart_type, "cart", NULL, cart_opts_s);
    if (extra)
        gemu_device_add(r.m, &sink_type, "extra", board, NULL);
    CHECK(r.pit && r.pic && r.cpu && r.cart);
    CHECK(gemu_machine_init(r.m));                    /* the devices make their lines */
    CHECK(gemu_lines_connect(r.m, "board/pit:out", "board/pic:in", 0));
    CHECK(gemu_machine_start(r.m));
    return r;
}

typedef struct {
    uint32_t count, edges, derived;
    uint16_t regs[8];
    uint64_t acc, now, cycles;
    uint8_t  cart[16];
    bool     level;
} Obs;

static Obs observe(Rig *r) {
    Obs o;
    memset(&o, 0, sizeof o);
    Counter *c = r->pit->priv;
    o.count = c->count;
    o.derived = c->derived;
    memcpy(o.regs, c->regs, sizeof o.regs);
    o.edges = ((Sink *)r->pic->priv)->edges;
    Cpu *cpu = r->cpu->priv;
    o.acc = cpu->acc;
    o.cycles = cpu->e->cycles;
    o.now = gemu_machine_time(r->m);
    memcpy(o.cart, ((Cart *)r->cart->priv)->ram, sizeof o.cart);
    o.level = gemu_line_in_level(gemu_line_find_in(r->pic, "in"));
    return o;
}
static bool same(const Obs *a, const Obs *b) {
    return a->count == b->count && a->edges == b->edges && a->acc == b->acc &&
           a->now == b->now && a->cycles == b->cycles && a->level == b->level &&
           !memcmp(a->regs, b->regs, sizeof a->regs) && !memcmp(a->cart, b->cart, sizeof a->cart);
}

/* A load that must fail and leave the machine exactly as it was. */
static void refused(Rig *r, const uint8_t *buf, size_t len, const char *msg) {
    uint8_t *before, *after;
    size_t nb, na;
    CHECK(gemu_machine_save_state(r->m, &before, &nb));
    unsigned pl = post_loads;
    CHECK(!gemu_machine_load_state(r->m, buf, len));
    if (msg && !strstr(gemu_machine_error(r->m), msg))
        fprintf(stderr, "error was: %s (wanted %s)\n", gemu_machine_error(r->m), msg);
    CHECK(!msg || strstr(gemu_machine_error(r->m), msg));
    CHECK(post_loads == pl);
    CHECK(gemu_machine_save_state(r->m, &after, &na));
    CHECK(na == nb && !memcmp(before, after, nb));
    free(before);
    free(after);
}

static GemuMachine *cb_machine;
static bool cb_saved;
static void save_in_callback(void *ud, int param) {
    (void)ud; (void)param;
    uint8_t *b;
    size_t n;
    cb_saved = gemu_machine_save_state(cb_machine, &b, &n);
}

int main(void) {
    /* ── Round trip: save at t1, run to t2; load, run to t2 again: the same ── */
    Rig r = build("pc", NULL, NULL, false);
    ((Cart *)r.cart->priv)->ram[3] = 0xC3;
    CHECK(gemu_machine_run_until(r.m, GEMU_MSEC(12) + 345));
    Obs t1 = observe(&r);
    CHECK(t1.count == 12 && t1.edges == 6 && t1.acc != 0);
    uint8_t *s1;
    size_t n1;
    unsigned ps = pre_saves;
    CHECK(gemu_machine_save_state(r.m, &s1, &n1));
    CHECK(pre_saves == ps + 1 && ((Counter *)r.pit->priv)->mode == 0x5A);
    CHECK(gemu_machine_save_file(r.m, "pc.state"));
    CHECK(gemu_machine_run_until(r.m, GEMU_MSEC(50)));
    Obs t2 = observe(&r);
    CHECK(t2.count == 50 && t2.edges == 25);
    ((Cart *)r.cart->priv)->ram[5] = 0x55;            /* changes the load must undo */
    ((Counter *)r.pit->priv)->mode = 1;

    unsigned pl = post_loads;
    CHECK(gemu_machine_load_state(r.m, s1, n1));
    CHECK(post_loads == pl + 1);
    Obs back = observe(&r);
    CHECK(same(&back, &t1));
    CHECK(((Counter *)r.pit->priv)->derived == t1.count * 3);   /* post_load ran */
    CHECK(((Counter *)r.pit->priv)->mode == 0x5A);
    CHECK(((Cart *)r.cart->priv)->ram[5] == 0 && ((Cart *)r.cart->priv)->ram[3] == 0xC3);
    CHECK(gemu_now(r.m) == t1.now);
    CHECK(gemu_timer_expires(((Counter *)r.pit->priv)->t) == GEMU_MSEC(13));
    /* the input was restored quietly: no callback during the load */
    unsigned calls = ((Sink *)r.pic->priv)->calls;
    CHECK(gemu_machine_run_until(r.m, GEMU_MSEC(50)));
    Obs again = observe(&r);
    CHECK(same(&again, &t2));
    CHECK(((Sink *)r.pic->priv)->calls > calls);

    /* from the file, into a fresh machine built the same way */
    Rig r2 = build("pc", NULL, NULL, false);
    CHECK(gemu_machine_load_file(r2.m, "pc.state"));
    Obs f = observe(&r2);
    CHECK(same(&f, &t1));
    CHECK(gemu_machine_run_until(r2.m, GEMU_MSEC(50)));
    Obs f2 = observe(&r2);
    CHECK(same(&f2, &t2));
    /* and saving that again gives the bytes of a save at the same point */
    CHECK(gemu_machine_load_state(r2.m, s1, n1));
    uint8_t *s1b;
    size_t n1b;
    CHECK(gemu_machine_save_state(r2.m, &s1b, &n1b));
    CHECK(n1b == n1 && !memcmp(s1, s1b, n1));
    free(s1b);
    CHECK(!gemu_machine_load_file(r2.m, "no-such.state"));
    CHECK(strstr(gemu_machine_error(r2.m), "can't open"));

    /* ── Refused, untouched ── */
    Rig other = build("xt", NULL, NULL, false);
    refused(&other, s1, n1, "saved from pc, this is xt");
    Rig extra = build("pc", NULL, NULL, true);
    refused(&extra, s1, n1, "devices saved");
    Rig fewer = build("pc", NULL, NULL, false);
    gemu_device_destroy(gemu_device_find(fewer.m, "cart"));
    refused(&fewer, s1, n1, "state: cart (cart) isn't in this machine");
    Rig regs = build("pc", "regs=4", NULL, false);
    refused(&regs, s1, n1, "state: board/pit item regs: 8 x 2 bytes saved, 4 x 2 here");
    Rig timers = build("pc", "timers=2", NULL, false);
    refused(&timers, s1, n1, "timers saved");
    Rig bigcart = build("pc", NULL, "size=2K", false);
    refused(&bigcart, s1, n1, "state: cart item ram: refused by the device");
    /* every truncation, and damage */
    for (size_t len = 0; len < n1; len++)
        refused(&r2, s1, len, NULL);                     /* the same machine, cut short */
    uint8_t *bad = malloc(n1);
    for (size_t i = 0; i < 40; i++) {
        memcpy(bad, s1, n1);
        bad[(i * 7919) % n1] ^= 0x41;
        Rig t = build("pc", NULL, NULL, false);
        uint8_t *b0;
        size_t nb0;
        CHECK(gemu_machine_save_state(t.m, &b0, &nb0));
        if (!gemu_machine_load_state(t.m, bad, n1)) {
            uint8_t *b1;
            size_t nb1;
            CHECK(gemu_machine_save_state(t.m, &b1, &nb1));
            CHECK(nb1 == nb0 && !memcmp(b0, b1, nb0));   /* a refusal changes nothing */
            free(b1);
        }
        free(b0);
        gemu_machine_free(t.m);
    }
    memcpy(bad, s1, n1);
    bad[0] = 'X';
    refused(&fewer, bad, n1, "not a GEMU machine state");
    memcpy(bad, s1, n1);
    bad[8] = 99;
    refused(&fewer, bad, n1, "format 99");
    free(bad);

    /* ── When saving isn't possible ── */
    GemuMachine *ns = gemu_machine_new("ns");
    uint8_t *b;
    size_t nb;
    CHECK(!gemu_machine_save_state(ns, &b, &nb) && strstr(gemu_machine_error(ns), "isn't started"));
    CHECK(!gemu_machine_load_state(ns, s1, n1));
    CHECK(gemu_machine_start(ns));
    cb_machine = ns;
    GemuTimer *t = gemu_timer_new(gemu_machine_root(ns), save_in_callback, NULL);
    gemu_timer_start(t, 10, 0, 0);
    cb_saved = true;
    gemu_machine_run_until(ns, 100);
    CHECK(!cb_saved && strstr(gemu_machine_error(ns), "not while the machine is running"));
    CHECK(gemu_machine_save_state(ns, &b, &nb));         /* an empty machine saves fine */
    CHECK(gemu_machine_load_state(ns, b, nb));
    free(b);
    gemu_machine_free(ns);

    /* ── Every device must register its state ── */
    GemuMachine *q = gemu_machine_new("q");
    gemu_device_add(q, &nostate_type, "lazy", NULL, NULL);
    CHECK(!gemu_machine_start(q));
    CHECK(strstr(gemu_machine_error(q), "lazy: nostate registers no save state"));
    gemu_machine_free(q);
    q = gemu_machine_new("q");
    gemu_device_add(q, &dup_type, "d", NULL, NULL);
    CHECK(!gemu_machine_start(q) && strstr(gemu_machine_error(q), "\"x\" registered twice"));
    gemu_machine_free(q);
    q = gemu_machine_new("q");
    gemu_device_add(q, &zero_type, "z", NULL, NULL);
    CHECK(!gemu_machine_start(q) && strstr(gemu_machine_error(q), "no memory or no size"));
    gemu_machine_free(q);
    /* hot-plugged too */
    q = gemu_machine_new("q");
    CHECK(gemu_machine_start(q));
    CHECK(!gemu_device_add(q, &nostate_type, "lazy", NULL, NULL));
    CHECK(strstr(gemu_machine_error(q), "registers no save state") && !gemu_device_find(q, "lazy"));
    gemu_machine_free(q);

    free(s1);
    Rig *rigs[] = { &r, &r2, &other, &extra, &fewer, &regs, &timers, &bigcart };
    for (unsigned i = 0; i < sizeof rigs / sizeof rigs[0]; i++)
        gemu_machine_free(rigs[i]->m);
    printf("state: %u checks passed (state %zu bytes)\n", checks, n1);
    return 0;
}
