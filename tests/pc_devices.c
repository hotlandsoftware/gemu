/* The 5150's chips as core devices (stage D1): i8259, i8253, i8237,
 * i8255-pc, upd765, ibm-fixed-disk, ins8250.  Run: ninja check-pc_devices
 *
 * They are wired into a small PC - ports in an "io" space, RAM in
 * "program", IRQs and the refresh/speaker lines connected - and driven
 * through the ports as software would; then a stretch of activity is
 * replayed from a save state. */
#include "controller/i8237_dev.h"
#include "controller/i8255_dev.h"
#include "controller/i8259_dev.h"
#include "serial/ins8250_dev.h"
#include "storage/ibm_hdc_dev.h"
#include "storage/upd765_dev.h"
#include "timer/i8253_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include "gemu/drive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* drive.c (the disk wrappers name their drives) registers drives with the
 * monitor, which a device test doesn't link. */
#include "gemu/monitor.h"
bool gemu_monitor_register_media(GemuMonitor *mon, const GemuMediaDevice *dev) {
    (void)mon; (void)dev;
    return false;
}
GemuMediaDevice *gemu_monitor_find_media(GemuMonitor *mon, const char *name) {
    (void)mon; (void)name;
    return NULL;
}

static const GemuDeviceType board_type = { .name = "board", .stateless = true };

/* The CPU's INTR pin: count rising edges */
typedef struct { unsigned rises; bool level; } Cpu;
static void intr_cb(void *ud, int idx, bool level) {
    (void)idx;
    Cpu *c = ud;
    if (level && !c->level)
        c->rises++;
    c->level = level;
}
static bool cpu_init(GemuDevice *d) {
    Cpu *c = d->priv;
    return GEMU_STATE_VAR(d, "rises", c->rises) && GEMU_STATE_VAR(d, "level", c->level) &&
           gemu_line_in(d, "intr", intr_cb, c, 0);
}
static const GemuDeviceType cpu_type = { .name = "test-cpu", .size = sizeof(Cpu), .init = cpu_init };

typedef struct {
    GemuMachine *m;
    GemuSpace *mem, *io;
    GemuDevice *pic, *pit, *dma, *ppi, *fdc, *hdc, *com, *cpu;
    Cpu *cpu_pins;
} Pc;

static uint8_t ram[0x10000];

static void build(Pc *pc) {
    memset(pc, 0, sizeof *pc);
    GemuMachine *m = pc->m = gemu_machine_new("ibm5150");
    pc->mem = gemu_space_new(m, "program", 20, 8, GEMU_LITTLE_ENDIAN);
    pc->io = gemu_space_new(m, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    memset(ram, 0, sizeof ram);
    gemu_space_map_ram(pc->mem, 0, sizeof ram, ram, 0, 0);

    GemuDevice *b = gemu_device_add(m, &board_type, "board", NULL, NULL);
    pc->pic = gemu_device_add_type(m, "i8259", "pic", b, NULL);
    pc->pit = gemu_device_add_type(m, "i8253", "pit", b, NULL);
    pc->dma = gemu_device_add_type(m, "i8237", "dma", b, NULL);
    pc->ppi = gemu_device_add_type(m, "i8255-pc", "ppi", b, "sw1=0x41,sw2=0x15");
    pc->fdc = gemu_device_add_type(m, "upd765", "fdc", b, NULL);
    pc->hdc = gemu_device_add_type(m, "ibm-fixed-disk", "hdc", b, "interleave=3,irq=5");
    pc->com = gemu_device_add_type(m, "ins8250", "com1", b, "model=16450");
    pc->cpu = gemu_device_add(m, &cpu_type, "cpu", NULL, NULL);
    CHECK(pc->pic && pc->pit && pc->dma && pc->ppi && pc->fdc && pc->hdc && pc->com && pc->cpu);
    gemu_device_set_clock(pc->pit, 1193182);

    CHECK(gemu_machine_init(m));
    struct { uint64_t base, size; const GemuHandlers *h; GemuDevice *d; } ports[] = {
        { 0x000, 16, &gemu_i8237_io, pc->dma },      { 0x020, 2, &gemu_i8259_io, pc->pic },
        { 0x040, 4, &gemu_i8253_io, pc->pit },       { 0x060, 4, &gemu_i8255_io, pc->ppi },
        { 0x080, 4, &gemu_i8237_page_io, pc->dma },  { 0x320, 4, &gemu_ibm_hdc_io, pc->hdc },
        { 0x3F0, 8, &gemu_upd765_io, pc->fdc },      { 0x3F8, 8, &gemu_ins8250_io, pc->com },
    };
    for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; i++)
        CHECK(gemu_space_map(pc->io, ports[i].base, ports[i].size, ports[i].h, ports[i].d, 0, 0));
    static const char *const wires[][2] = {
        { "board/pit:out0", "board/pic:ir0" },  { "board/com1:int", "board/pic:ir4" },
        { "board/hdc:int", "board/pic:ir5" },   { "board/fdc:int", "board/pic:ir6" },
        { "board/pit:out1", "board/dma:dreq0" },{ "board/ppi:pb0", "board/pit:gate2" },
        { "board/pit:out2", "board/ppi:pc5" },
    };
    for (unsigned i = 0; i < sizeof wires / sizeof wires[0]; i++)
        CHECK(gemu_lines_connect(m, wires[i][0], wires[i][1], 0));
    pc->cpu_pins = pc->cpu->priv;
    CHECK(gemu_lines_connect(m, "board/pic:int", "cpu:intr", 0));
    gemu_upd765_set_dma(pc->fdc, pc->dma, 2);
    gemu_ibm_hdc_set_dma(pc->hdc, pc->dma, 3);
    CHECK(gemu_machine_start(m));
}

static void out(Pc *pc, uint16_t port, uint8_t v) { gemu_space_write8(pc->io, port, v); }
static uint8_t in(Pc *pc, uint16_t port) { return gemu_space_read8(pc->io, port); }
static bool line(GemuDevice *d, const char *name) { return gemu_line_get(gemu_line_find_out(d, name)); }

static const char *describe(GemuDevice *d) {
    static char buf[2048];
    gemu_device_describe(d, buf, sizeof buf);
    return buf;
}
#define DESCRIBES(d, text) do { const char *s_ = describe(d); \
    if (!strstr(s_, text)) fprintf(stderr, "%s describes as:\n%s\n(wanted '%s')\n", (d)->path, s_, text); \
    CHECK(strstr(s_, text)); } while (0)

/* What the machine looks like: ports read without side effects, lines,
 * RAM - for comparing a replay with the original. */
static void snapshot_view(Pc *pc, char *buf, size_t len) {
    int n = 0;
    static const uint16_t peeks[] = { 0x21, 0x20, 0x60, 0x61, 0x62, 0x81, 0x82, 0x83,
                                      0x321, 0x322, 0x3F4, 0x3F9, 0x3FB, 0x3FC, 0x3FD, 0x3FF };
    for (unsigned i = 0; i < sizeof peeks / sizeof peeks[0]; i++) {
        uint8_t v = 0;
        bool ok = gemu_space_peek8(pc->io, peeks[i], &v);
        n += snprintf(buf + n, len - (size_t)n, "%03X=%s%02X ", peeks[i], ok ? "" : "?", v);
    }
    n += snprintf(buf + n, len - (size_t)n, "int=%d out=%d%d%d rises=%u ram=%02X%02X ch0=%04X",
                  line(pc->pic, "int"), line(pc->pit, "out0"), line(pc->pit, "out1"),
                  line(pc->pit, "out2"), pc->cpu_pins->rises, ram[0x1000], ram[0x1001],
                  gemu_i8237(pc->dma)->ch[0].cur_addr);
    for (int i = 0; i < 3; i++)
        n += snprintf(buf + n, len - (size_t)n, " c%d=%04X", i, gemu_i8253(pc->pit)->ch[i].count);
}

/* A stretch of activity: the timer ticking IRQ 0 and refresh, the CPU
 * acknowledging and ending interrupts, the speaker gate toggling. */
static void activity(Pc *pc, char *log, size_t len) {
    int n = 0;
    for (int step = 0; step < 40; step++) {
        gemu_i8253_tick(pc->pit, 3);
        if (line(pc->pic, "int")) {
            uint8_t vec = gemu_i8259_ack(pc->pic);
            out(pc, 0x20, 0x20);                       /* EOI */
            n += snprintf(log + n, len - (size_t)n, "v%02X ", vec);
        }
        if (step % 7 == 0)
            out(pc, 0x61, (uint8_t)(in(pc, 0x61) ^ 1));
        gemu_upd765_tick(pc->fdc, 1000);
        gemu_ibm_hdc_tick(pc->hdc, 1000);
        gemu_ins8250_tick(pc->com, 1);
    }
    char view[1024];
    snapshot_view(pc, view, sizeof view);
    snprintf(log + n, len - (size_t)n, "| %s", view);
}

int main(void) {
    /* ── The registry has them all ── */
    CHECK(gemu_device_type_find("i8259") == &gemu_i8259_type);
    CHECK(gemu_device_type_find("i8253") == &gemu_i8253_type);
    CHECK(gemu_device_type_find("i8237") == &gemu_i8237_type);
    CHECK(gemu_device_type_find("i8255-pc") == &gemu_i8255_type);
    CHECK(gemu_device_type_find("upd765") == &gemu_upd765_type);
    CHECK(gemu_device_type_find("ibm-fixed-disk") == &gemu_ibm_hdc_type);
    CHECK(gemu_device_type_find("ins8250") == &gemu_ins8250_type);
    CHECK(gemu_device_type_count() == 7);

    Pc pc;
    build(&pc);

    /* ── PIC: the BIOS's initialisation; IRQ 0 from the PIT ── */
    out(&pc, 0x20, 0x13);                           /* ICW1: edge, single, ICW4 */
    out(&pc, 0x21, 0x08);                           /* ICW2: vectors 08h-0Fh */
    out(&pc, 0x21, 0x09);                           /* ICW4: 8086, buffered */
    out(&pc, 0x21, 0xAE);                           /* unmask IRQ 0, 4, 6 */
    CHECK(in(&pc, 0x21) == 0xAE);
    uint8_t v;
    CHECK(gemu_space_peek8(pc.io, 0x21, &v) && v == 0xAE);
    DESCRIBES(pc.pic, "IMR AE");
    DESCRIBES(pc.pic, "vector base 08, edge-triggered");

    /* ── PIT: channel 0 mode 3, count 4: out0 toggles every 2 clocks ── */
    out(&pc, 0x43, 0x36);
    out(&pc, 0x40, 4);
    out(&pc, 0x40, 0);
    CHECK(!gemu_space_peek8(pc.io, 0x40, &v));      /* counters can't be peeked */
    CHECK(line(pc.pit, "out0"));
    gemu_i8253_tick(pc.pit, 1);                     /* the count loads */
    gemu_i8253_tick(pc.pit, 2);
    CHECK(!line(pc.pit, "out0"));
    unsigned r0 = pc.cpu_pins->rises;
    gemu_i8253_tick(pc.pit, 2);
    CHECK(line(pc.pit, "out0"));                    /* rising: IRQ 0 */
    CHECK(gemu_line_in_level(gemu_line_find_in(pc.pic, "ir0")));
    CHECK(line(pc.pic, "int") && pc.cpu_pins->rises == r0 + 1 && pc.cpu_pins->level);
    CHECK(gemu_i8259_ack(pc.pic) == 0x08);
    CHECK(!line(pc.pic, "int") && !pc.cpu_pins->level);
    DESCRIBES(pc.pic, "ISR 01");
    out(&pc, 0x20, 0x20);                           /* EOI */
    DESCRIBES(pc.pic, "ISR 00");
    DESCRIBES(pc.pit, "channel 0: mode 3, LSB+MSB, count");

    /* ── Refresh: PIT channel 1 -> DMA channel 0 ── */
    out(&pc, 0x0B, 0x58);                           /* as the BIOS: ch 0 single, read, auto-init */
    out(&pc, 0x01, 0xFF);
    out(&pc, 0x01, 0xFF);
    out(&pc, 0x0A, 0x00);                           /* unmask channel 0 */
    uint16_t a0 = gemu_i8237(pc.dma)->ch[0].cur_addr;
    out(&pc, 0x43, 0x54);                           /* channel 1, LSB, mode 2 */
    out(&pc, 0x41, 3);
    gemu_i8253_tick(pc.pit, 10);
    CHECK(gemu_i8237(pc.dma)->ch[0].cur_addr != a0);

    /* ── PPI: switches, port B lines, timer 2 back on PC5 ── */
    out(&pc, 0x61, 0x80);                           /* PB7: port A shows SW1 */
    CHECK(in(&pc, 0x60) == 0x41);
    gemu_i8255_set_kbd(pc.ppi, 0x1E);
    out(&pc, 0x61, 0x00);
    CHECK(in(&pc, 0x60) == 0x1E && !gemu_i8253(pc.pit)->ch[2].gate);
    out(&pc, 0x61, 0x05);                           /* PB0 gate 2 high, PB2: SW2 1-4 */
    CHECK(line(pc.ppi, "pb0") && line(pc.ppi, "pb2") && gemu_i8253(pc.pit)->ch[2].gate);
    CHECK((in(&pc, 0x62) & 0x0F) == 0x05);
    out(&pc, 0x43, 0xB0);                           /* channel 2, mode 0 */
    out(&pc, 0x42, 2);
    out(&pc, 0x42, 0);
    CHECK(!line(pc.pit, "out2") && !(in(&pc, 0x62) & 0x20));
    gemu_i8253_tick(pc.pit, 4);
    CHECK(line(pc.pit, "out2") && (in(&pc, 0x62) & 0x20));   /* PC5 = timer 2 */
    DESCRIBES(pc.ppi, "SW1 41  SW2 15");
    DESCRIBES(pc.ppi, "timer 2 gate 1");

    /* ── DMA: channel 2, two bytes to 01000h ── */
    out(&pc, 0x0C, 0);                              /* clear the flip-flop */
    out(&pc, 0x0B, 0x46);                           /* ch 2: single, increment, write to memory */
    out(&pc, 0x04, 0x00);
    out(&pc, 0x04, 0x10);
    out(&pc, 0x05, 0x01);                           /* count 1: two bytes */
    out(&pc, 0x05, 0x00);
    out(&pc, 0x81, 0x00);                           /* channel 2's page */
    bool tc = false;
    CHECK(!gemu_i8237_to_mem(pc.dma, 2, 0x5A, &tc)); /* still masked */
    out(&pc, 0x0A, 0x02);                           /* unmask channel 2 */
    CHECK(gemu_i8237_to_mem(pc.dma, 2, 0x5A, &tc) && !tc && ram[0x1000] == 0x5A);
    CHECK(gemu_i8237_to_mem(pc.dma, 2, 0xA5, &tc) && tc && ram[0x1001] == 0xA5);
    CHECK(gemu_space_peek8(pc.io, 0x81, &v) && v == 0x00);
    DESCRIBES(pc.dma, "channel 2: address 00:1002 (base 1000)");
    out(&pc, 0x83, 0x0C);
    CHECK(in(&pc, 0x83) == 0x0C);
    ram[0x2000] = 0x77;                             /* and from memory */
    out(&pc, 0x0C, 0);
    out(&pc, 0x0B, 0x4B);                           /* ch 3: single, increment, read from memory */
    out(&pc, 0x06, 0x00);
    out(&pc, 0x06, 0x20);
    out(&pc, 0x07, 0);
    out(&pc, 0x07, 0);
    out(&pc, 0x82, 0x00);                           /* channel 3's page */
    out(&pc, 0x0A, 0x03);
    uint8_t got = 0;
    CHECK(gemu_i8237_from_mem(pc.dma, 3, &got, &tc) && got == 0x77 && tc);

    /* ── FDC: out of reset, the controller interrupts (IRQ 6) ── */
    unsigned r1 = pc.cpu_pins->rises;
    out(&pc, 0x3F2, 0x00);
    out(&pc, 0x3F2, 0x1C);                          /* drive A, running, DMA/IRQ, motor A */
    gemu_upd765_tick(pc.fdc, 1000000);
    CHECK(line(pc.fdc, "int"));
    CHECK(gemu_line_in_level(gemu_line_find_in(pc.pic, "ir6")));
    CHECK(pc.cpu_pins->rises == r1 + 1 && gemu_i8259_ack(pc.pic) == 0x0E);
    out(&pc, 0x20, 0x20);
    CHECK(gemu_space_peek8(pc.io, 0x3F4, &v) && (v & 0x80));    /* RQM */
    CHECK(!gemu_space_peek8(pc.io, 0x3F5, &v));
    out(&pc, 0x3F5, 0x08);                          /* sense interrupt status */
    gemu_upd765_tick(pc.fdc, 1000000);
    CHECK(!line(pc.fdc, "int"));
    uint8_t st0 = in(&pc, 0x3F5), pcn = in(&pc, 0x3F5);
    CHECK((st0 & 0xC0) == 0xC0 && pcn == 0);        /* "ready changed" after reset */
    DESCRIBES(pc.fdc, "DOR 1C: drive A, running, DMA/IRQ on, motors A---");

    /* ── HDC: a command for a drive that isn't there fails with IRQ 5 ── */
    out(&pc, 0x323, 0x03);                          /* DMA and IRQ enabled */
    out(&pc, 0x322, 0);                             /* select */
    static const uint8_t tdr[6] = { 0x00, 0, 0, 0, 0, 0 };   /* test drive ready, drive 0 */
    for (int i = 0; i < 6; i++)
        out(&pc, 0x320, tdr[i]);
    for (int i = 0; i < 100 && !line(pc.hdc, "int"); i++)
        gemu_ibm_hdc_tick(pc.hdc, 10000);
    CHECK(line(pc.hdc, "int"));
    CHECK(gemu_line_in_level(gemu_line_find_in(pc.pic, "ir5")));   /* masked at the PIC */
    CHECK(gemu_space_peek8(pc.io, 0x321, &v) && (v & 0x20));
    CHECK(!gemu_space_peek8(pc.io, 0x320, &v));
    CHECK(in(&pc, 0x320) & 0x02);                   /* the status byte: error */
    CHECK(!line(pc.hdc, "int"));
    DESCRIBES(pc.hdc, "mask 03 (DMA on, IRQ on)");
    CHECK(!strcmp(gemu_opt_str(pc.hdc, "irq"), "5") && gemu_opt_int(pc.hdc, "interleave") == 3);
    CHECK(gemu_opt_bool(pc.hdc, "rom") && gemu_opt_int(pc.hdc, "type0") == -1);

    /* ── UART: 9600 8N1, THR-empty interrupt through OUT2 (IRQ 4) ── */
    out(&pc, 0x3FB, 0x80);
    out(&pc, 0x3F8, 12);
    out(&pc, 0x3F9, 0);
    out(&pc, 0x3FB, 0x03);
    out(&pc, 0x3FF, 0x99);                          /* the 16450's scratch register */
    CHECK(in(&pc, 0x3FF) == 0x99);
    unsigned r2 = pc.cpu_pins->rises;
    out(&pc, 0x3F9, 0x02);                          /* IER: THR empty */
    CHECK(!line(pc.com, "int"));                    /* OUT2 still off */
    out(&pc, 0x3FC, 0x08);                          /* MCR: OUT2 */
    CHECK(line(pc.com, "int") && pc.cpu_pins->rises == r2 + 1);
    CHECK(gemu_i8259_ack(pc.pic) == 0x0C);
    out(&pc, 0x20, 0x20);
    CHECK(gemu_space_peek8(pc.io, 0x3FD, &v) && (v & 0x60) == 0x60);
    CHECK(!gemu_space_peek8(pc.io, 0x3FA, &v));
    CHECK(in(&pc, 0x3FA) == 0x02);                  /* IIR: THR empty, now cleared */
    CHECK(!line(pc.com, "int"));
    DESCRIBES(pc.com, "16450, divisor 12 (9600 baud), 8N1");

    /* ── Reset: the chips start over, the pins keep their levels ── */
    gemu_line_set(gemu_line_find_out(pc.ppi, "pb0"), false);
    gemu_machine_reset(pc.m, GEMU_RESET_HARD);
    CHECK(in(&pc, 0x21) == 0xFF);                   /* PIC: everything masked */
    CHECK(gemu_i8253(pc.pit)->ch[0].gate && !gemu_i8253(pc.pit)->ch[2].gate);   /* gate 2 from PB0 */
    CHECK(line(pc.pit, "out0") && line(pc.pit, "out2"));
    CHECK(gemu_i8237(pc.dma)->mask == 0x0F && in(&pc, 0x61) == 0);
    CHECK(in(&pc, 0x3FB) == 0 && gemu_i8255(pc.ppi)->sw1 == 0x41);

    /* ── Save state: every type registered complete state; replay ── */
    out(&pc, 0x20, 0x13);
    out(&pc, 0x21, 0x08);
    out(&pc, 0x21, 0x09);
    out(&pc, 0x21, 0xFE);
    out(&pc, 0x43, 0x36);                           /* PIT 0: mode 3, count 10 */
    out(&pc, 0x40, 10);
    out(&pc, 0x40, 0);
    out(&pc, 0x43, 0x54);                           /* PIT 1: refresh every 5 */
    out(&pc, 0x41, 5);
    out(&pc, 0x3FB, 0x83);                          /* UART: DLAB set mid-programming */
    out(&pc, 0x3F8, 6);
    gemu_i8253_tick(pc.pit, 7);                     /* counting, mid-period */
    uint8_t *s1;
    size_t n1;
    CHECK(gemu_machine_save_state(pc.m, &s1, &n1));
    char first[4096], again[4096];
    activity(&pc, first, sizeof first);
    CHECK(strstr(first, "v08 "));                   /* IRQ 0 came round */
    CHECK(gemu_machine_load_state(pc.m, s1, n1));
    activity(&pc, again, sizeof again);
    if (strcmp(first, again))
        fprintf(stderr, "first: %s\nagain: %s\n", first, again);
    CHECK(!strcmp(first, again));
    /* a fresh machine built the same way, from the state */
    Pc pc2;
    GemuMachine *keep = pc.m;
    build(&pc2);
    CHECK(gemu_machine_load_state(pc2.m, s1, n1));
    uint8_t *s2;
    size_t n2;
    CHECK(gemu_machine_save_state(pc2.m, &s2, &n2));
    CHECK(n2 == n1 && !memcmp(s1, s2, n1));
    free(s1);
    free(s2);

    /* ── The disks' contents: a controller's drives are part of its state ── */
    static const char *const imgs[] = { "pcdev-a.img", "pcdev-b.img", "pcdev-c.img" };
    static const long sizes[] = { 163840, 327680, 163840 };
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(imgs[i], "wb");
        CHECK(f && fseek(f, sizes[i] - 1, SEEK_SET) == 0 && fputc(0, f) == 0 && !fclose(f));
    }
    CHECK(gemu_drive_add("file=pcdev-a.img,if=floppy,index=0,snapshot=on"));
    CHECK(gemu_drive_add("file=pcdev-b.img,if=floppy,index=1,snapshot=on"));
    CHECK(gemu_drive_add("file=pcdev-c.img,if=floppy,index=2"));
    GemuDrive *da = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, 0);
    GemuDrive *db = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, 1);
    GemuDrive *dc = gemu_drive_get(GEMU_DRIVE_IF_FLOPPY, 0, 2);
    CHECK(da && db && dc);
    Upd765 *fdc = gemu_upd765(pc2.fdc);
    fdc->drive[0] = da;
    uint8_t got4[4];
    CHECK(gemu_drive_write(da, 0, "AAAA", 4));
    uint8_t *ds;
    size_t dn;
    CHECK(gemu_machine_save_state(pc2.m, &ds, &dn));
    CHECK(gemu_drive_write(da, 0, "BBBB", 4) && gemu_drive_write(da, 70000, "BBBB", 4));
    CHECK(gemu_machine_load_state(pc2.m, ds, dn));     /* the writes since are gone */
    CHECK(gemu_drive_read(da, 0, got4, 4) && !memcmp(got4, "AAAA", 4));
    CHECK(gemu_drive_read(da, 70000, got4, 4) && !memcmp(got4, "\0\0\0\0", 4));
    /* another disk (by size), or none: refused, nothing changed */
    out(&pc2, 0x21, 0x5A);
    fdc->drive[0] = db;
    CHECK(gemu_drive_write(db, 0, "CCCC", 4));
    CHECK(!gemu_machine_load_state(pc2.m, ds, dn));
    CHECK(strstr(gemu_machine_error(pc2.m), "board/fdc: floppy1: 'pcdev-b.img' is 327680 bytes; "
                                            "the state's disk ('pcdev-a.img') was 163840"));
    CHECK(in(&pc2, 0x21) == 0x5A && fdc->drive[0] == db);
    CHECK(gemu_drive_read(db, 0, got4, 4) && !memcmp(got4, "CCCC", 4));
    fdc->drive[0] = NULL;
    CHECK(!gemu_machine_load_state(pc2.m, ds, dn));
    CHECK(strstr(gemu_machine_error(pc2.m), "the drive: the state has 'pcdev-a.img' (163840 "
                                            "bytes) in it; this drive is not here"));
    /* the same size, not in snapshot mode: it is from here on, the image untouched */
    fdc->drive[0] = dc;
    CHECK(gemu_machine_load_state(pc2.m, ds, dn));
    CHECK(gemu_drive_read(dc, 0, got4, 4) && !memcmp(got4, "AAAA", 4));
    CHECK(gemu_drive_write(dc, 4, "DDDD", 4));
    FILE *cf = fopen("pcdev-c.img", "rb");
    CHECK(cf && fread(got4, 1, 4, cf) == 4 && !fclose(cf) && !memcmp(got4, "\0\0\0\0", 4));
    free(ds);
    /* an empty drive in the state takes whatever is in the drive now */
    fdc->drive[0] = NULL;
    CHECK(gemu_machine_save_state(pc2.m, &ds, &dn));
    fdc->drive[0] = db;
    CHECK(gemu_machine_load_state(pc2.m, ds, dn));
    CHECK(gemu_drive_read(db, 0, got4, 4) && !memcmp(got4, "CCCC", 4));
    free(ds);
    fdc->drive[0] = NULL;
    /* the same through a FILE (the i2000's savevm) */
    FILE *sf = fopen("pcdev-state.bin", "w+b");
    CHECK(sf && gemu_drive_save_state(da, sf) && gemu_drive_save_state(NULL, sf));
    CHECK(gemu_drive_write(da, 0, "EEEE", 4));
    rewind(sf);
    CHECK(gemu_drive_load_state(da, sf) && gemu_drive_load_state(NULL, sf));
    CHECK(gemu_drive_read(da, 0, got4, 4) && !memcmp(got4, "AAAA", 4));
    CHECK(fgetc(sf) == EOF && !fclose(sf));
    remove("pcdev-state.bin");
    for (int i = 0; i < 3; i++) remove(imgs[i]);

    /* ── Option errors ── */
    GemuMachine *bad = gemu_machine_new("bad");
    CHECK(!gemu_device_add_type(bad, "ins8250", "com", NULL, "model=8251"));
    CHECK(strstr(gemu_machine_error(bad), "not one of 8250, 16450"));
    CHECK(!gemu_device_add_type(bad, "ibm-fixed-disk", "hdc", NULL, "irq=7"));
    CHECK(!gemu_device_add_type(bad, "ibm-fixed-disk", "hdc", NULL, "interleave=17"));
    CHECK(!gemu_device_add_type(bad, "i8255-pc", "ppi", NULL, "sw1=256"));
    gemu_machine_free(bad);

    gemu_machine_free(keep);
    gemu_machine_free(pc2.m);
    printf("pc_devices: %u checks passed\n", checks);
    return 0;
}
