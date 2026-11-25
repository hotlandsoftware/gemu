/* The INS8250 UART as a core device: see ins8250_dev.h. */
#include "serial/ins8250_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    Ins8250   chip;
    GemuLine *intr;
} Uart;

Ins8250 *gemu_ins8250(GemuDevice *d) { return &((Uart *)d->priv)->chip; }
void gemu_ins8250_tick(GemuDevice *d, uint32_t c) { ins8250_tick(&((Uart *)d->priv)->chip, c); }

static void set_irq(void *ud, bool level) { gemu_line_set(((Uart *)ud)->intr, level); }

static Ins8250 *chip_of(void *ud) { return &((Uart *)((GemuDevice *)ud)->priv)->chip; }
static uint8_t io_r8(void *ud, uint64_t off) { return ins8250_read(chip_of(ud), (unsigned)off & 7); }
static void io_w8(void *ud, uint64_t off, uint8_t v) { ins8250_write(chip_of(ud), (unsigned)off & 7, v); }
/* The registers as they stand, without the reads' side effects (RBR's
 * data-ready, LSR's errors, MSR's deltas); not IIR, which is worked out. */
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    const Ins8250 *u = chip_of(ud);
    bool dlab = (u->lcr & 0x80) != 0;
    switch (off & 7) {
    case 0: *v = dlab ? (uint8_t)u->divisor : u->rbr; return true;
    case 1: *v = dlab ? (uint8_t)(u->divisor >> 8) : u->ier; return true;
    case 3: *v = u->lcr; return true;
    case 4: *v = u->mcr; return true;
    case 5: *v = u->lsr; return true;
    case 6: *v = u->msr; return true;
    case 7: *v = u->model == INS8250_16450 ? u->scr : 0xFF; return true;
    default: return false;
    }
}
const GemuHandlers gemu_ins8250_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static const char *const models[] = { "8250", "16450", NULL };
static const GemuOptionDef uart_opts[] = {
    { "model", "the chip", GEMU_OPT_CHOICE, "8250", 0, 0, models },
    { "clock", "the rate it is ticked at, Hz (0: per character)", GEMU_OPT_INT, "0", 0, 1000000000, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool uart_init(GemuDevice *d) {
    Uart *p = d->priv;
    Ins8250 *u = &p->chip;
    if (!(p->intr = gemu_line_out(d, "int")))
        return false;
    u->model = gemu_opt_choice(d, "model") ? INS8250_16450 : INS8250_8250;
    u->clock_hz = (uint32_t)gemu_opt_int(d, "clock");
    u->set_irq = set_irq;
    u->ud = p;
    GEMU_STATE_VAR(d, "modem_in", u->modem_in);
    GEMU_STATE_VAR(d, "rbr", u->rbr);
    GEMU_STATE_VAR(d, "thr", u->thr);
    GEMU_STATE_VAR(d, "ier", u->ier);
    GEMU_STATE_VAR(d, "lcr", u->lcr);
    GEMU_STATE_VAR(d, "mcr", u->mcr);
    GEMU_STATE_VAR(d, "lsr", u->lsr);
    GEMU_STATE_VAR(d, "msr", u->msr);
    GEMU_STATE_VAR(d, "scr", u->scr);
    GEMU_STATE_VAR(d, "divisor", u->divisor);
    GEMU_STATE_VAR(d, "thre_int", u->thre_int);
    GEMU_STATE_VAR(d, "tx_busy", u->tx_busy);
    GEMU_STATE_VAR(d, "tsr", u->tsr);
    GEMU_STATE_VAR(d, "tx_wait", u->tx_wait);
    GEMU_STATE_VAR(d, "rx_busy", u->rx_busy);
    GEMU_STATE_VAR(d, "rsr", u->rsr);
    GEMU_STATE_VAR(d, "rx_wait", u->rx_wait);
    GEMU_STATE_VAR(d, "loop_pending", u->loop_pending);
    GEMU_STATE_VAR(d, "loop_byte", u->loop_byte);
    return true;
}

static void uart_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    ins8250_reset(&((Uart *)d->priv)->chip);
}

static void uart_describe(GemuDevice *d, char *buf, size_t len) {
    const Ins8250 *u = &((Uart *)d->priv)->chip;
    static const char *const parity[] = { "N", "O", "N", "E", "N", "M", "N", "S" };
    unsigned bits = 5 + (u->lcr & 3), stop = (u->lcr & 4) ? 2 : 1;
    snprintf(buf, len,
             "%s, divisor %u (%u baud), %u%s%u%s\n"
             "IER %02X  LCR %02X  MCR %02X  LSR %02X  MSR %02X%s\n"
             "transmitter %s, receiver %s%s",
             u->model == INS8250_16450 ? "16450" : "8250", u->divisor,
             u->divisor ? 115200u / u->divisor : 0, bits, parity[(u->lcr >> 3) & 7], stop,
             (u->lcr & 0x80) ? ", DLAB" : "", u->ier, u->lcr, u->mcr, u->lsr, u->msr,
             (u->mcr & 0x10) ? ", loopback" : "",
             u->tx_busy ? "sending" : "idle", u->rx_busy ? "receiving" : "idle",
             u->dev_tx || u->dev_rx ? ", a device attached" : "");
}

const GemuDeviceType gemu_ins8250_type = {
    .name = "ins8250", .desc = "National INS8250 UART (IBM asynchronous communications adapter)",
    .size = sizeof(Uart), .options = uart_opts,
    .init = uart_init, .reset = uart_reset, .describe = uart_describe,
};
