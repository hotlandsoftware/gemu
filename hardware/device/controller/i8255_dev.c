/* The 5150's 8255 PPI as a core device: see i8255_dev.h. */
#include "controller/i8255_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    I8255     chip;
    GemuLine *pb[8];
} Ppi;

I8255 *gemu_i8255(GemuDevice *d) { return &((Ppi *)d->priv)->chip; }
void gemu_i8255_set_kbd(GemuDevice *d, uint8_t scan) { ((Ppi *)d->priv)->chip.kb_data = scan; }

static void drive_pb(Ppi *p) {
    for (int i = 0; i < 8; i++)
        gemu_line_set(p->pb[i], (p->chip.port_b >> i) & 1);
}

static void pc_in(void *ud, int idx, bool level) {
    Ppi *p = ud;
    if (idx == 4) p->chip.cassette_in = level;
    else          p->chip.timer2_out = level;
}

static I8255 *chip_of(void *ud) { return &((Ppi *)((GemuDevice *)ud)->priv)->chip; }
static uint8_t io_r8(void *ud, uint64_t off) { return i8255_io_read(chip_of(ud), (uint16_t)off); }
static void io_w8(void *ud, uint64_t off, uint8_t v) {
    Ppi *p = ((GemuDevice *)ud)->priv;
    i8255_io_write(&p->chip, (uint16_t)off, v);
    drive_pb(p);
}
static bool io_peek(void *ud, uint64_t off, uint8_t *v) { *v = io_r8(ud, off); return true; }
const GemuHandlers gemu_i8255_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static const GemuOptionDef ppi_opts[] = {
    { "sw1", "DIP switch block 1 (bit 0 = switch 1)", GEMU_OPT_INT, "0", 0, 255, NULL },
    { "sw2", "DIP switch block 2 (bit 0 = switch 1)", GEMU_OPT_INT, "0", 0, 255, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool ppi_init(GemuDevice *d) {
    Ppi *p = d->priv;
    I8255 *c = &p->chip;
    for (int i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof name, "pb%d", i);
        if (!(p->pb[i] = gemu_line_out(d, name)))
            return false;
    }
    if (!gemu_line_in(d, "pc4", pc_in, p, 4) || !gemu_line_in(d, "pc5", pc_in, p, 5))
        return false;
    GEMU_STATE_VAR(d, "port_b", c->port_b);
    GEMU_STATE_VAR(d, "kb_data", c->kb_data);
    GEMU_STATE_VAR(d, "timer2_out", c->timer2_out);
    GEMU_STATE_VAR(d, "cassette_in", c->cassette_in);
    return true;   /* sw1/sw2 are the options: configuration, not state */
}

static void ppi_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Ppi *p = d->priv;
    p->chip.sw1 = (uint8_t)gemu_opt_int(d, "sw1");
    p->chip.sw2 = (uint8_t)gemu_opt_int(d, "sw2");
    i8255_reset(&p->chip);
    p->chip.timer2_out = gemu_line_in_level(gemu_line_find_in(d, "pc5"));
    p->chip.cassette_in = gemu_line_in_level(gemu_line_find_in(d, "pc4"));
    drive_pb(p);
}

static void ppi_describe(GemuDevice *d, char *buf, size_t len) {
    I8255 *c = &((Ppi *)d->priv)->chip;
    snprintf(buf, len,
             "port A %02X (%s)  port B %02X  port C %02X\n"
             "PB: timer 2 gate %d, speaker %d, %s, cassette motor %s, keyboard clock %s%s\n"
             "SW1 %02X  SW2 %02X",
             i8255_io_read(c, 0), (c->port_b & 0x80) ? "SW1" : "keyboard", c->port_b,
             i8255_io_read(c, 2), c->port_b & 1, (c->port_b >> 1) & 1,
             (c->port_b & 4) ? "SW2 1-4 on PC0-3" : "SW2 5 on PC0",
             (c->port_b & 8) ? "off" : "on", (c->port_b & 0x40) ? "running" : "held low",
             (c->port_b & 0x80) ? ", keyboard clear" : "", c->sw1, c->sw2);
}

const GemuDeviceType gemu_i8255_type = {
    .name = "i8255-pc", .desc = "Intel 8255 PPI on the IBM 5150 motherboard",
    .size = sizeof(Ppi), .options = ppi_opts,
    .init = ppi_init, .reset = ppi_reset, .describe = ppi_describe,
};
