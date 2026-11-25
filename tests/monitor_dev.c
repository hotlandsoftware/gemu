/* The monitor on a core machine (core/src/monitor_dev.c): info qtree /
 * mtree / <path>, "space:addr", device commands.  Run: ninja check-monitor_dev */
#include "../core/src/monitor_priv.h"
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static GemuMonitor *mon;
static char out[65536];

/* Run a line; its output (captured as for QMP) in out. */
static const char *run(const char *line) {
    free(mon->capture_buf);
    mon->capture_buf = NULL;
    mon->capture_len = mon->capture_size = 0;
    mon->capturing = true;
    mon_dispatch(mon, line);
    mon->capturing = false;
    snprintf(out, sizeof out, "%s", mon->capture_buf ? mon->capture_buf : "");
    return out;
}
#define HAS(line, text) do { run(line); \
    if (!strstr(out, text)) fprintf(stderr, "'%s' printed:\n%s(wanted '%s')\n", line, out, text); \
    CHECK(strstr(out, text)); } while (0)
#define LACKS(line, text) do { run(line); \
    if (strstr(out, text)) fprintf(stderr, "'%s' printed:\n%s(did not want '%s')\n", line, out, text); \
    CHECK(!strstr(out, text)); } while (0)

/* ── Device types ── */
static unsigned regs_calls;
static GemuDevice *last_dev;
static char last_args[64];
static void pit_regs(GemuDevice *d, GemuMonitor *m, const char *args) {
    regs_calls++;
    last_dev = d;
    snprintf(last_args, sizeof last_args, "%s", args);
    gemu_monitor_printf(m, "pit regs of %s\n", d->path);
}
static void pit_x(GemuDevice *d, GemuMonitor *m, const char *args) {
    (void)d; (void)args;
    gemu_monitor_printf(m, "THE PIT'S OWN X\n");
}
static const GemuDeviceCmd pit_cmds[] = {
    { "pitregs", "[channel]", "show the PIT's channels", pit_regs },
    { "x", "", "shadows the monitor's x: ignored", pit_x },
    { NULL, NULL, NULL, NULL },
};
static const GemuOptionDef pit_opts[] = {
    { "channels", "how many", GEMU_OPT_INT, "3", 1, 3, NULL },
    { "gate", "gate 2", GEMU_OPT_BOOL, NULL, 0, 0, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static void pit_describe(GemuDevice *d, char *buf, size_t len) {
    (void)d;
    snprintf(buf, len, "channel 0: mode 3, count 0000\nchannel 1: mode 2, count 0012");
}
static uint8_t pit_state[4];
static bool pit_init(GemuDevice *d) { return GEMU_STATE_ARRAY(d, "regs", pit_state); }
static const GemuDeviceType pit_type = {
    .name = "i8253", .desc = "Intel 8253 PIT", .options = pit_opts, .commands = pit_cmds,
    .init = pit_init, .describe = pit_describe,
};

static void uart_regs(GemuDevice *d, GemuMonitor *m, const char *args) {
    last_dev = d;
    snprintf(last_args, sizeof last_args, "%s", args);
    gemu_monitor_printf(m, "uart %s\n", d->path);
}
static const GemuDeviceCmd uart_cmds[] = {
    { "uartregs", "", "show the UART's registers", uart_regs }, { NULL, NULL, NULL, NULL } };
static const GemuDeviceType uart_type = {
    .name = "ns8250", .desc = "8250 UART", .commands = uart_cmds, .stateless = true,
};
static const GemuDeviceType bus_type = { .name = "isa", .stateless = true };

/* ── Handlers ── */
typedef struct { unsigned reads, writes; uint64_t off, v; } Port;
static uint8_t port_r8(void *ud, uint64_t off) { ((Port *)ud)->reads++; return (uint8_t)(0xA0 + off); }
static void port_w8(void *ud, uint64_t off, uint8_t v) { Port *p = ud; p->writes++; p->off = off; p->v = v; }
static bool port_peek(void *ud, uint64_t off, uint8_t *v) { (void)ud; *v = (uint8_t)(0xA0 + off); return true; }
static const GemuHandlers vga_h = { .r8 = port_r8, .w8 = port_w8, .peek8 = port_peek };
static const GemuHandlers fifo_h = { .r8 = port_r8, .w8 = port_w8 };   /* no peek */

/* A machine-provided view (as machines have today) */
static bool old_read(void *ud, uint64_t a, uint8_t *v) { (void)ud; *v = (uint8_t)(a ^ 0xFF); return true; }

int main(void) {
    gemu_monitor_set_default("none");
    mon = gemu_monitor_create();
    CHECK(mon);

    GemuMachine *m = gemu_machine_new("testpc");
    GemuDevice *board = gemu_device_add(m, &bus_type, "board", NULL, NULL);
    GemuDevice *pit = gemu_device_add(m, &pit_type, "pit", board, "gate=on");
    gemu_device_set_clock(board, 1193182);
    GemuDevice *com1 = gemu_device_add(m, &uart_type, "com1", board, NULL);
    GemuDevice *com2 = gemu_device_add(m, &uart_type, "com2", board, NULL);
    CHECK(pit && com1 && com2 && gemu_machine_start(m));

    static uint8_t ram[0x8000], rom[0x1000];
    for (unsigned i = 0; i < sizeof ram; i++)
        ram[i] = (uint8_t)i;
    memset(rom, 0xEA, sizeof rom);
    GemuSpace *prog = gemu_space_new(m, "program", 16, 8, GEMU_LITTLE_ENDIAN);
    GemuSpace *io = gemu_space_new(m, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    gemu_mapping_set_name(gemu_space_map_ram(prog, 0, sizeof ram, ram, 0, 0), "ram");
    gemu_mapping_set_name(gemu_space_map_ram(prog, 0xF000, sizeof rom, rom, GEMU_MAP_READONLY, 1), "bios");
    Port vga = { 0 }, fifo = { 0 };
    gemu_mapping_set_name(gemu_space_map(io, 0x3C0, 0x20, &vga_h, &vga, 0, 0), "vga");
    gemu_space_map(io, 0x60, 1, &fifo_h, &fifo, 0, GEMU_MAP_SHARED);
    gemu_space_mirror(io, 0, 0x10000, 0x3FF);

    /* ── Not attached: nothing new ── */
    HAS("x 10", "not supported on this machine");
    HAS("info qtree", "unknown subcommand 'qtree'");
    HAS("pitregs", "unknown command 'pitregs'");

    /* ── Attached ── */
    gemu_monitor_attach_machine(mon, m, "program");
    HAS("info qtree", "machine testpc\n  board  (isa)  1.193182 MHz\n    pit  (i8253)  gate=on\n"
                      "    com1  (ns8250)\n    com2  (ns8250)\n");
    run("info mtree");
    CHECK(strstr(out, "program: 16-bit addresses, 8-bit data, little-endian\n"));
    CHECK(strstr(out, "  0000-7fff  ram              prio 0  ram\n"));
    CHECK(strstr(out, "  f000-ffff  rom              prio 1  bios\n"));
    CHECK(strstr(out, "  0060-0060  i/o              prio 0 shared\n"));
    CHECK(strstr(out, "  03c0-03df  i/o              prio 0  vga\n"));
    CHECK(strstr(out, "  0000-ffff  mirror (address & 03ff)\n"));
    run("info board/pit");
    CHECK(strstr(out, "board/pit: i8253 - Intel 8253 PIT\n"));
    CHECK(strstr(out, "  clock  1.193182 MHz (inherited)\n"));
    CHECK(strstr(out, "  channels=3 (default)\n  gate=on\n"));
    CHECK(strstr(out, "  channel 0: mode 3, count 0000\n  channel 1: mode 2, count 0012\n"));
    HAS("info board/com1", "board/com1: ns8250 - 8250 UART\n");
    HAS("info board/nothing", "unknown subcommand 'board/nothing'");
    HAS("info status", "");                                  /* built-ins still first */
    LACKS("info status", "unknown");

    /* ── Memory: the default space, and space:addr ── */
    HAS("x /4xb 10", "0010: 10 11 12 13\n");
    HAS("x /2xh 7ffe", "7ffe: fffe");                         /* RAM, then the hole */
    HAS("peek f000 2", "f000: ea ea\n");
    HAS("x /4xb io:3c0", "03c0: a0 a1 a2 a3\n");
    HAS("x /2xh io:3c2", "03c2: a3a2 a5a4\n");
    HAS("x /4xb io:7c0", "07c0: a0 a1 a2 a3\n");              /* through the mirror */
    CHECK(vga.reads == 0);                                    /* peeks, not reads */
    HAS("peek io:60 1", "0060: --\n");                        /* no peek hook */
    CHECK(fifo.reads == 0);
    HAS("poke io:3c5 55", "wrote 1 byte at 0x03c5\n");
    CHECK(vga.writes == 1 && vga.off == 5 && vga.v == 0x55);
    HAS("poke 7000 aa bb", "wrote 2 bytes at 0x7000\n");
    CHECK(ram[0x7000] == 0xAA && ram[0x7001] == 0xBB);
    run("poke f000 11");                                      /* ROM: the write goes nowhere */
    CHECK(rom[0] == 0xEA);
    HAS("x nope:10", "x: no address space 'nope' (spaces: program, io)\n");
    HAS("x /4xb program:10", "0010: 10 11 12 13\n");
    HAS("memsave io:3c0 4 io.bin", "saved 4 bytes from 0x03c0 to io.bin");
    FILE *f = fopen("io.bin", "rb");
    uint8_t b[8] = { 0 };
    CHECK(f && fread(b, 1, 8, f) == 4 && b[0] == 0xA0 && b[3] == 0xA3);
    fclose(f);
    remove("io.bin");
    HAS("x", "usage: x");                                     /* no address: usage */
    /* the machine's own view wins over the default space */
    GemuMonMemory old = { old_read, NULL, NULL, 16, false };
    gemu_monitor_set_memory(mon, &old);
    HAS("x /2xb 10", "0010: ef ee\n");
    HAS("poke 10 1", "poke: this memory can't be written");
    HAS("x /2xb program:10", "0010: 10 11\n");                /* a prefix still picks the space */
    gemu_monitor_set_memory(mon, NULL);
    /* no default space: the prefix is needed */
    gemu_monitor_attach_machine(mon, m, NULL);
    HAS("x 10", "x: no default memory; give a space, e.g. program:10 (spaces: program, io)");
    gemu_monitor_attach_machine(mon, m, "program");

    /* ── Device commands ── */
    HAS("pitregs 2", "pit regs of board/pit\n");
    CHECK(regs_calls == 1 && last_dev == pit && !strcmp(last_args, "2"));
    HAS("uartregs", "uartregs: 2 devices have it; pick one with @path: board/com1 board/com2\n");
    HAS("uartregs @board/com2 dlab", "uart board/com2\n");
    CHECK(last_dev == com2 && !strcmp(last_args, "dlab"));
    HAS("uartregs @board/pit", "uartregs: board/pit has no such command\n");
    HAS("uartregs @board/nope", "uartregs: board/nope is not a device here\n");
    HAS("x /1xb 10", "0010: 10\n");                           /* not the PIT's "x" */
    LACKS("x /1xb 10", "THE PIT'S OWN X");
    run("help");
    CHECK(strstr(out, "Device commands"));
    CHECK(strstr(out, "pitregs [channel]") && strstr(out, "show the PIT's channels (i8253)"));
    CHECK(strstr(out, "uartregs") && !strstr(out, "shadows the monitor's x"));
    HAS("help pitregs", "pitregs [channel]\n    show the PIT's channels\n    devices: board/pit (i8253)\n");
    HAS("help info", "info qtree");
    HAS("help info", "info <device path>");

    /* ── Detached again ── */
    gemu_monitor_attach_machine(mon, NULL, "program");
    HAS("pitregs", "unknown command 'pitregs'");
    HAS("info mtree", "unknown subcommand 'mtree'");
    HAS("x 10", "not supported on this machine");
    LACKS("help", "Device commands");

    gemu_machine_free(m);
    gemu_monitor_destroy(mon);
    printf("monitor_dev: %u checks passed\n", checks);
    return 0;
}
