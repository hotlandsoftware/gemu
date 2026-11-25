/* Lines (core/src/dev/line.c).  Run: ninja check-line */
#include "gemu/dev/line.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* Input callbacks log "<name><idx>=<level>". */
static char log_buf[1024];
static void log_cb(void *ud, int idx, bool level) {
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s%s%d=%d", n ? " " : "", (const char *)ud, idx, level);
}
static void log_clear(void) { log_buf[0] = 0; }

/* An input whose callback releases the output that drove it (nested set). */
static GemuLine *self_out;
static void self_release(void *ud, int idx, bool level) {
    log_cb(ud, idx, level);
    if (level)
        gemu_line_set(self_out, false);
}

static const GemuDeviceType chip = { .name = "chip", .stateless = true };

int main(void) {
    GemuMachine *m = gemu_machine_new("lines");
    GemuDevice *board = gemu_device_add(m, &chip, "board", NULL, NULL);
    GemuDevice *pit = gemu_device_add(m, &chip, "pit", board, NULL);
    GemuDevice *pic = gemu_device_add(m, &chip, "pic", board, NULL);
    GemuDevice *cpu = gemu_device_add(m, &chip, "cpu", NULL, NULL);
    GemuDevice *isa = gemu_device_add(m, &chip, "isa", NULL, NULL);

    /* ── Names ── */
    GemuLine *out0 = gemu_line_out(pit, "out0");
    GemuLine *out1 = gemu_line_out(pit, "out1");
    CHECK(out0 && out1 && !gemu_line_out(pit, "out0") && !gemu_line_out(pit, ""));
    CHECK(gemu_line_out(pic, "out0"));                         /* other device: fine */
    GemuLineIn *ir0 = gemu_line_in(pic, "ir0", log_cb, "ir", 0);
    GemuLineIn *ir1 = gemu_line_in(pic, "ir1", log_cb, "ir", 1);
    CHECK(ir0 && ir1 && !gemu_line_in(pic, "ir0", log_cb, "x", 9));
    CHECK(gemu_line_in(pit, "out0", log_cb, "x", 0));          /* an input may share an output's name */
    CHECK(gemu_line_find_out(pit, "out1") == out1 && gemu_line_find_in(pic, "ir1") == ir1);
    CHECK(!gemu_line_find_out(pic, "ir0") && !gemu_line_find_in(pic, "out0"));
    CHECK(!strcmp(gemu_line_name(out0), "out0") && !strcmp(gemu_line_in_name(ir1), "ir1"));
    char longname[40];
    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(!gemu_line_out(pit, longname));

    /* ── Before start: levels kept, no callbacks ── */
    CHECK(gemu_line_connect(out0, pic, "ir0", 0));
    CHECK(!gemu_line_connect(out0, pic, "ir7", 0));
    gemu_line_set(out0, true);
    CHECK(gemu_line_get(out0) && gemu_line_in_level(ir0) && log_buf[0] == 0);
    CHECK(gemu_machine_start(m));
    CHECK(log_buf[0] == 0 && gemu_line_in_level(ir0));         /* still none */

    /* ── Running: callbacks on changes only ── */
    gemu_line_set(out0, true);
    CHECK(log_buf[0] == 0);
    gemu_line_set(out0, false);
    CHECK(!strcmp(log_buf, "ir0=0"));
    log_clear();
    gemu_line_pulse(out0);
    CHECK(!strcmp(log_buf, "ir0=1 ir0=0"));

    /* ── Fan-out: one output, several inputs ── */
    GemuLineIn *nmi = gemu_line_in(cpu, "nmi", log_cb, "nmi", 0);
    GemuLineIn *mon = gemu_line_in(isa, "watch", log_cb, "w", 5);
    CHECK(gemu_line_connect_in(out0, nmi, 0) && gemu_line_connect_in(out0, mon, 0));
    log_clear();
    gemu_line_set(out0, true);
    /* newest connection first */
    CHECK(strstr(log_buf, "ir0=1") && strstr(log_buf, "nmi0=1") && strstr(log_buf, "w5=1"));
    gemu_line_set(out0, false);
    CHECK(gemu_line_disconnect(out0, nmi) == 1 && gemu_line_disconnect(out0, nmi) == 0);
    log_clear();
    gemu_line_set(out0, true);
    CHECK(!strstr(log_buf, "nmi"));
    gemu_line_set(out0, false);

    /* ── Inversion: an undriven-high (active-low) pin ── */
    log_clear();
    CHECK(gemu_line_connect_in(out1, ir1, GEMU_LINE_INVERT));   /* out1 low: ir1 asserted */
    CHECK(!strcmp(log_buf, "ir1=1") && gemu_line_in_level(ir1));
    log_clear();
    gemu_line_set(out1, true);
    CHECK(!strcmp(log_buf, "ir1=0"));
    gemu_line_set(out1, false);
    gemu_line_disconnect(out1, ir1);

    /* ── Wired-OR: two drivers on one input (a shared ISA or PCI INTx line) ── */
    GemuLine *a = gemu_line_out(isa, "irq-a"), *b = gemu_line_out(isa, "irq-b");
    GemuLineIn *intx = gemu_line_in(pic, "intx", log_cb, "x", 0);
    gemu_line_connect_in(a, intx, 0);
    gemu_line_connect_in(b, intx, 0);
    CHECK(gemu_line_in_drivers(intx) == 2);
    log_clear();
    gemu_line_set(a, true);
    gemu_line_set(b, true);
    gemu_line_set(a, false);
    CHECK(!strcmp(log_buf, "x0=1"));                           /* still held by b */
    gemu_line_set(b, false);
    CHECK(!strcmp(log_buf, "x0=1 x0=0"));

    /* ── Wired-AND ── */
    gemu_line_in_set_mode(intx, GEMU_LINE_WIRED_AND);
    log_clear();
    gemu_line_set(a, true);
    CHECK(log_buf[0] == 0);
    gemu_line_set(b, true);
    CHECK(!strcmp(log_buf, "x0=1"));
    gemu_line_set(a, false);
    CHECK(!strcmp(log_buf, "x0=1 x0=0"));
    gemu_line_set(b, false);
    gemu_line_in_set_mode(intx, GEMU_LINE_WIRED_OR);

    /* ── Default level of an undriven input ── */
    GemuLineIn *rdy = gemu_line_in(cpu, "ready", log_cb, "rdy", 0);
    log_clear();
    gemu_line_in_set_default(rdy, true);
    CHECK(!strcmp(log_buf, "rdy0=1") && gemu_line_in_level(rdy));
    GemuLine *wait = gemu_line_out(isa, "ready");
    log_clear();
    gemu_line_connect_in(wait, rdy, 0);                        /* driven, low */
    CHECK(!strcmp(log_buf, "rdy0=0"));
    log_clear();
    gemu_line_disconnect(wait, rdy);                           /* back to its default */
    CHECK(!strcmp(log_buf, "rdy0=1"));

    /* ── By path ── */
    gemu_line_out(pit, "out2");
    gemu_line_in(pic, "ir2", log_cb, "ir", 2);
    CHECK(gemu_lines_connect(m, "board/pit:out2", "board/pic:ir2", 0));
    log_clear();
    gemu_line_set(gemu_line_find_out(pit, "out2"), true);
    CHECK(!strcmp(log_buf, "ir2=1"));
    CHECK(!gemu_lines_connect(m, "board/pit", "board/pic:ir2", 0));
    CHECK(strstr(gemu_machine_error(m), "not \"device:line\""));
    CHECK(!gemu_lines_connect(m, "board/pit:", "board/pic:ir2", 0));
    CHECK(!gemu_lines_connect(m, "board/fdc:drq", "board/pic:ir2", 0));
    CHECK(strstr(gemu_machine_error(m), "no device \"board/fdc\""));
    CHECK(!gemu_lines_connect(m, "board/pit:out9", "board/pic:ir2", 0));
    CHECK(strstr(gemu_machine_error(m), "no output \"out9\""));
    CHECK(!gemu_lines_connect(m, "board/pit:out2", "board/pic:ir9", 0));
    CHECK(strstr(gemu_machine_error(m), "no input \"ir9\""));
    CHECK(gemu_lines_connect(m, "isa:irq-a", "cpu:nmi", GEMU_LINE_INVERT));   /* top level */
    gemu_line_disconnect(a, nmi);

    /* ── Nested: a callback releasing the line that asserted it ── */
    self_out = gemu_line_out(cpu, "self");
    GemuLineIn *s1 = gemu_line_in(isa, "s1", self_release, "s", 1);
    GemuLineIn *s2 = gemu_line_in(isa, "s2", log_cb, "s", 2);
    gemu_line_connect_in(self_out, s2, 0);
    gemu_line_connect_in(self_out, s1, 0);                     /* s1 first in the list */
    log_clear();
    gemu_line_set(self_out, true);
    CHECK(!gemu_line_get(self_out) && !gemu_line_in_level(s1) && !gemu_line_in_level(s2));
    CHECK(!strcmp(log_buf, "s1=1 s1=0"));                      /* s2 never saw it high */
    log_clear();
    gemu_line_set(self_out, true);                             /* the counts still add up */
    CHECK(!strcmp(log_buf, "s1=1 s1=0"));

    /* ── Destroying a device ── */
    /* a driver going away releases what it held */
    GemuDevice *card = gemu_device_add(m, &chip, "card", isa, NULL);
    GemuLine *cirq = gemu_line_out(card, "irq");
    gemu_line_connect_in(cirq, intx, 0);
    gemu_line_set(cirq, true);
    CHECK(gemu_line_in_level(intx) && gemu_line_in_drivers(intx) == 3);
    log_clear();
    gemu_device_destroy(card);
    CHECK(!strcmp(log_buf, "x0=0") && gemu_line_in_drivers(intx) == 2);
    /* an input going away leaves its drivers working */
    GemuDevice *spy = gemu_device_add(m, &chip, "spy", NULL, NULL);
    GemuLineIn *spin = gemu_line_in(spy, "in", log_cb, "spy", 0);
    gemu_line_connect_in(out0, spin, 0);
    gemu_device_destroy(spy);
    log_clear();
    gemu_line_set(out0, true);
    CHECK(!strcmp(log_buf, "w5=1 ir0=1"));
    /* the board (and pit, pic under it) */
    gemu_device_destroy(board);
    CHECK(!gemu_line_find_in(cpu, "nothing"));

    /* ── Not across machines ── */
    GemuMachine *m2 = gemu_machine_new("other");
    GemuDevice *o = gemu_device_add(m2, &chip, "o", NULL, NULL);
    GemuLine *ol = gemu_line_out(o, "x");
    CHECK(!gemu_line_connect_in(ol, rdy, 0));
    CHECK(!gemu_line_connect_in(NULL, rdy, 0) && !gemu_line_connect_in(ol, NULL, 0));
    gemu_machine_free(m2);

    gemu_machine_free(m);
    printf("line: %u checks passed\n", checks);
    return 0;
}
