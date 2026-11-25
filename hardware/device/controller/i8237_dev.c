/* The 8237A DMA controller as a core device: see i8237_dev.h. */
#include "controller/i8237_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    I8237      chip;
    GemuSpace *mem;          /* found at first use: spaces may come after init */
} Dma;

I8237 *gemu_i8237(GemuDevice *d) { return &((Dma *)d->priv)->chip; }

static GemuSpace *memory(GemuDevice *d) {
    Dma *p = d->priv;
    if (!p->mem)
        p->mem = gemu_space_find(d->machine, gemu_opt_str(d, "space"));
    return p->mem;
}

bool gemu_i8237_to_mem(GemuDevice *d, unsigned ch, uint8_t val, bool *tc) {
    uint32_t phys;
    GemuSpace *s = memory(d);
    if (!s || !i8237_transfer(&((Dma *)d->priv)->chip, ch, &phys, tc))
        return false;
    gemu_space_write8(s, phys, val);
    return true;
}

bool gemu_i8237_from_mem(GemuDevice *d, unsigned ch, uint8_t *val, bool *tc) {
    uint32_t phys;
    GemuSpace *s = memory(d);
    if (!s || !i8237_transfer(&((Dma *)d->priv)->chip, ch, &phys, tc))
        return false;
    *val = gemu_space_read8(s, phys);
    return true;
}

static void dreq0(void *ud, int idx, bool level) {
    (void)idx;
    if (level)
        i8237_refresh(&((Dma *)ud)->chip);
}

static I8237 *chip_of(void *ud) { return &((Dma *)((GemuDevice *)ud)->priv)->chip; }
static uint8_t io_r8(void *ud, uint64_t off) { return i8237_io_read(chip_of(ud), (uint16_t)off); }
static void io_w8(void *ud, uint64_t off, uint8_t v) { i8237_io_write(chip_of(ud), (uint16_t)off, v); }
static uint8_t page_r8(void *ud, uint64_t off) { return i8237_page_read(chip_of(ud), (uint16_t)(0x80 + off)); }
static void page_w8(void *ud, uint64_t off, uint8_t v) { i8237_page_write(chip_of(ud), (uint16_t)(0x80 + off), v); }
static bool page_peek(void *ud, uint64_t off, uint8_t *v) { *v = page_r8(ud, off); return true; }
/* No peek on the controller: address/count reads move the byte flip-flop,
 * the status read clears the terminal-count bits. */
const GemuHandlers gemu_i8237_io = { .r8 = io_r8, .w8 = io_w8 };
const GemuHandlers gemu_i8237_page_io = { .r8 = page_r8, .w8 = page_w8, .peek8 = page_peek };

static const GemuOptionDef dma_opts[] = {
    { "space", "the address space transfers use", GEMU_OPT_STRING, "program", 0, 0, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool dma_init(GemuDevice *d) {
    Dma *p = d->priv;
    I8237 *c = &p->chip;
    if (!gemu_line_in(d, "dreq0", dreq0, p, 0))
        return false;
    for (int i = 0; i < 4; i++) {
        I8237Channel *ch = &c->ch[i];
        char n[32];
#define CH(field) snprintf(n, sizeof n, "ch%d.%s", i, #field); GEMU_STATE_VAR(d, n, ch->field)
        CH(base_addr); CH(base_count); CH(cur_addr); CH(cur_count); CH(mode); CH(page);
#undef CH
    }
    GEMU_STATE_VAR(d, "command", c->command);
    GEMU_STATE_VAR(d, "status", c->status);
    GEMU_STATE_VAR(d, "mask", c->mask);
    GEMU_STATE_VAR(d, "request", c->request);
    GEMU_STATE_VAR(d, "temp", c->temp);
    GEMU_STATE_VAR(d, "flipflop", c->flipflop);
    return true;
}

static void dma_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    i8237_reset(&((Dma *)d->priv)->chip);
}

static void dma_describe(GemuDevice *d, char *buf, size_t len) {
    const I8237 *c = &((Dma *)d->priv)->chip;
    static const char *const dir[] = { "verify", "write", "read", "?" };
    int n = snprintf(buf, len, "command %02X  status %02X  mask %X  request %X  flip-flop %s",
                     c->command, c->status, c->mask, c->request, c->flipflop ? "high" : "low");
    for (int i = 0; i < 4 && n >= 0 && (size_t)n < len; i++) {
        const I8237Channel *ch = &c->ch[i];
        n += snprintf(buf + n, len - (size_t)n,
                      "\nchannel %d: address %02X:%04X (base %04X), count %04X (base %04X), "
                      "mode %02X %s%s%s",
                      i, ch->page, ch->cur_addr, ch->base_addr, ch->cur_count, ch->base_count,
                      ch->mode, dir[(ch->mode >> 2) & 3], (ch->mode & 0x10) ? " auto-init" : "",
                      (c->mask >> i) & 1 ? ", masked" : "");
    }
}

const GemuDeviceType gemu_i8237_type = {
    .name = "i8237", .desc = "Intel 8237A DMA controller (with PC page registers)",
    .size = sizeof(Dma), .options = dma_opts,
    .init = dma_init, .reset = dma_reset, .describe = dma_describe,
};
