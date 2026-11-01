/* INS8250 UART: see ins8250.h. */
#include "ins8250.h"

#define LSR_DR   0x01
#define LSR_OE   0x02
#define LSR_ERRS 0x1E       /* OE, PE, FE, BI */
#define LSR_THRE 0x20
#define LSR_TEMT 0x40

#define MCR_DTR  0x01
#define MCR_RTS  0x02
#define MCR_OUT1 0x04
#define MCR_OUT2 0x08
#define MCR_LOOP 0x10

#define UART_HZ  1843200u

/* One character's time: start bit, 5-8 data bits, parity, 1/1.5/2 stops. */
static uint32_t char_clocks(const Ins8250 *u) {
    unsigned data = 5 + (u->lcr & 3);
    unsigned half_bits = 2 * (1 + data + ((u->lcr & 0x08) ? 1 : 0));
    half_bits += (u->lcr & 0x04) ? (data == 5 ? 3 : 4) : 2;
    uint64_t div = u->divisor ? u->divisor : 0x10000u;
    uint64_t c = (uint64_t)half_bits * 8 * div * u->clock_hz / UART_HZ;
    return c ? (uint32_t)c : 1;
}

/* The highest-priority pending interrupt, as IIR's bits 1-2, or -1. */
static int pending(const Ins8250 *u) {
    if ((u->ier & 0x04) && (u->lsr & LSR_ERRS)) return 3;
    if ((u->ier & 0x01) && (u->lsr & LSR_DR))   return 2;
    if ((u->ier & 0x02) && u->thre_int)         return 1;
    if ((u->ier & 0x08) && (u->msr & 0x0F))     return 0;
    return -1;
}

static void update_irq(Ins8250 *u) {
    if (u->set_irq) u->set_irq(u->ud, pending(u) >= 0 && (u->mcr & MCR_OUT2));
}

/* The modem inputs: from the outputs in loopback, otherwise none (the
 * devices here don't drive CTS, DSR, RI or DCD). */
static void update_msr(Ins8250 *u) {
    uint8_t in = 0;
    if (u->mcr & MCR_LOOP) {
        if (u->mcr & MCR_RTS)  in |= 0x10;
        if (u->mcr & MCR_DTR)  in |= 0x20;
        if (u->mcr & MCR_OUT1) in |= 0x40;
        if (u->mcr & MCR_OUT2) in |= 0x80;
    }
    uint8_t old = u->msr & 0xF0, delta = 0;
    if ((old ^ in) & 0x10) delta |= 0x01;
    if ((old ^ in) & 0x20) delta |= 0x02;
    if ((old & 0x40) && !(in & 0x40)) delta |= 0x04;   /* trailing edge of RI */
    if ((old ^ in) & 0x80) delta |= 0x08;
    u->msr = (uint8_t)(in | (u->msr & 0x0F) | delta);
}

/* The device sees DTR and RTS; in loopback the pins are held inactive. */
static void lines_changed(Ins8250 *u) {
    if (!u->dev_lines) return;
    bool loop = (u->mcr & MCR_LOOP) != 0;
    u->dev_lines(u->dev, !loop && (u->mcr & MCR_DTR), !loop && (u->mcr & MCR_RTS));
}

void ins8250_reset(Ins8250 *u) {
    u->rbr = u->thr = 0;
    u->ier = u->lcr = u->mcr = 0;
    u->lsr = LSR_THRE | LSR_TEMT;
    u->msr = 0;
    u->divisor = 0;
    u->thre_int = false;
    u->tx_busy = u->rx_busy = u->loop_pending = false;
    u->tx_wait = u->rx_wait = 0;
    update_msr(u);
    u->msr &= 0xF0;
    lines_changed(u);
    update_irq(u);
}

/* Move THR into the transmitter if it's free. */
static void tx_load(Ins8250 *u) {
    if (u->tx_busy || (u->lsr & LSR_THRE)) return;
    u->tsr = u->thr;
    u->tx_busy = true;
    u->tx_wait = char_clocks(u);
    u->lsr = (uint8_t)((u->lsr | LSR_THRE) & ~LSR_TEMT);
    u->thre_int = true;
}

static void rx_deliver(Ins8250 *u, uint8_t b) {
    if (u->lsr & LSR_DR) u->lsr |= LSR_OE;
    u->rbr = b;
    u->lsr |= LSR_DR;
}

uint8_t ins8250_read(Ins8250 *u, unsigned reg) {
    uint8_t v = 0xFF;
    bool dlab = (u->lcr & 0x80) != 0;
    switch (reg & 7) {
    case 0:
        if (dlab) return (uint8_t)u->divisor;
        v = u->rbr;
        u->lsr &= (uint8_t)~LSR_DR;
        break;
    case 1:
        return dlab ? (uint8_t)(u->divisor >> 8) : u->ier;
    case 2: {
        int p = pending(u);
        v = p < 0 ? 0x01 : (uint8_t)(p << 1);
        if (p == 1) u->thre_int = false;    /* reading IIR clears THR empty */
        break;
    }
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5:
        v = u->lsr;
        u->lsr &= (uint8_t)~LSR_ERRS;
        break;
    case 6:
        v = u->msr;
        u->msr &= 0xF0;
        break;
    case 7:
        return 0xFF;
    }
    update_irq(u);
    return v;
}

void ins8250_write(Ins8250 *u, unsigned reg, uint8_t val) {
    bool dlab = (u->lcr & 0x80) != 0;
    switch (reg & 7) {
    case 0:
        if (dlab) { u->divisor = (uint16_t)((u->divisor & 0xFF00) | val); return; }
        u->thr = val;
        u->lsr &= (uint8_t)~LSR_THRE;
        u->thre_int = false;
        tx_load(u);
        break;
    case 1:
        if (dlab) { u->divisor = (uint16_t)((u->divisor & 0x00FF) | val << 8); return; }
        /* enabling the THR-empty interrupt with THR empty raises it */
        if ((val & 0x02) && !(u->ier & 0x02) && (u->lsr & LSR_THRE)) u->thre_int = true;
        u->ier = val & 0x0F;
        break;
    case 3:
        u->lcr = val;
        return;
    case 4: {
        uint8_t old = u->mcr;
        u->mcr = val & 0x1F;
        update_msr(u);
        if ((old ^ u->mcr) & (MCR_DTR | MCR_RTS | MCR_LOOP)) lines_changed(u);
        break;
    }
    default:
        return;     /* LSR, MSR (factory test) and +7 */
    }
    update_irq(u);
}

void ins8250_tick(Ins8250 *u, uint32_t clocks) {
    bool changed = false;
    if (u->tx_busy) {
        if (clocks < u->tx_wait) {
            u->tx_wait -= clocks;
        } else {
            u->tx_busy = false;
            if (u->mcr & MCR_LOOP) {
                u->loop_pending = true;     /* straight into the receiver */
                u->loop_byte = u->tsr;
            } else if (u->dev_tx) {
                u->dev_tx(u->dev, u->tsr);
            }
            u->lsr |= LSR_TEMT;
            tx_load(u);
            changed = true;
        }
    }
    if (u->loop_pending) {
        u->loop_pending = false;
        rx_deliver(u, u->loop_byte);
        changed = true;
    }
    if (u->rx_busy) {
        if (clocks < u->rx_wait) {
            u->rx_wait -= clocks;
        } else {
            u->rx_busy = false;
            rx_deliver(u, u->rsr);
            changed = true;
        }
    }
    if (!u->rx_busy && !(u->mcr & MCR_LOOP) && u->dev_rx && u->dev_rx(u->dev, &u->rsr)) {
        u->rx_busy = true;
        u->rx_wait = char_clocks(u);
    }
    if (changed) update_irq(u);
}
