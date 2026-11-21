/* The 8250/16450 UART.  Run: ninja check-ins8250 */
#include "serial/ins8250.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* A terminal on the port: what it was sent, and bytes for it to send. */
static uint8_t sent[64], to_send[64];
static unsigned n_sent, n_to_send, to_send_pos;
static bool irq;

static void dev_tx(void *dev, uint8_t b) { (void)dev; sent[n_sent++] = b; }
static bool dev_rx(void *dev, uint8_t *b) {
    (void)dev;
    if (to_send_pos == n_to_send) return false;
    *b = to_send[to_send_pos++];
    return true;
}
static void set_irq(void *ud, bool level) { (void)ud; irq = level; }

static void wire(Ins8250 *u, Ins8250Model model, uint32_t clock_hz) {
    memset(u, 0, sizeof *u);
    u->model = model;
    u->clock_hz = clock_hz;
    u->set_irq = set_irq;
    u->dev_tx = dev_tx;
    u->dev_rx = dev_rx;
    n_sent = n_to_send = to_send_pos = 0;
    irq = false;
    ins8250_reset(u);
}

static void test_scratch(void) {
    Ins8250 u;
    wire(&u, INS8250_8250, 1843200);
    ins8250_write(&u, 7, 0x55);
    CHECK(ins8250_read(&u, 7) == 0xFF);          /* no register on the 8250 */
    wire(&u, INS8250_16450, 1843200);
    ins8250_write(&u, 7, 0x55);
    CHECK(ins8250_read(&u, 7) == 0x55);
    ins8250_write(&u, 7, 0xAA);
    CHECK(ins8250_read(&u, 7) == 0xAA);
}

static void test_modem_inputs(void) {
    Ins8250 u;
    memset(&u, 0, sizeof u);
    u.clock_hz = 1843200;
    ins8250_reset(&u);
    CHECK(ins8250_read(&u, 6) == 0x00);          /* nothing driven */
    u.modem_in = 0xB0;                            /* CTS, DSR, DCD */
    ins8250_reset(&u);
    CHECK(ins8250_read(&u, 6) == 0xB0);          /* no deltas after reset */
    ins8250_write(&u, 4, 0x10);                  /* loopback: from MCR */
    CHECK((ins8250_read(&u, 6) & 0xF0) == 0x00);
    ins8250_write(&u, 4, 0x13);                  /* + DTR, RTS */
    CHECK((ins8250_read(&u, 6) & 0xF0) == 0x30);
    ins8250_write(&u, 4, 0x00);                  /* back to the device */
    CHECK((ins8250_read(&u, 6) & 0xF0) == 0xB0);
}

/* Real timing: 9600 baud 8N1 is 1920 clocks of the 1.8432 MHz crystal. */
static void test_timing(void) {
    Ins8250 u;
    wire(&u, INS8250_8250, 1843200);
    ins8250_write(&u, 3, 0x83);                  /* DLAB, 8N1 */
    ins8250_write(&u, 0, 12);                    /* 9600 */
    ins8250_write(&u, 1, 0);
    CHECK(ins8250_read(&u, 0) == 12 && ins8250_read(&u, 1) == 0);
    ins8250_write(&u, 3, 0x03);
    ins8250_write(&u, 0, 'A');
    CHECK((ins8250_read(&u, 5) & 0x60) == 0x20);  /* THR empty, TSR busy */
    ins8250_write(&u, 0, 'B');
    CHECK(!(ins8250_read(&u, 5) & 0x20));
    ins8250_tick(&u, 1919);
    CHECK(n_sent == 0);
    ins8250_tick(&u, 1);
    CHECK(n_sent == 1 && sent[0] == 'A');
    ins8250_tick(&u, 1920);
    CHECK(n_sent == 2 && sent[1] == 'B');
    CHECK((ins8250_read(&u, 5) & 0x60) == 0x60);

    /* Received bytes arrive a character apart; one not read is overrun. */
    to_send[n_to_send++] = 'x';
    to_send[n_to_send++] = 'y';
    ins8250_tick(&u, 1);                          /* the receiver takes 'x' */
    CHECK(!(ins8250_read(&u, 5) & 0x01));
    ins8250_tick(&u, 1920);
    CHECK(ins8250_read(&u, 5) & 0x01);
    ins8250_tick(&u, 1920);                       /* 'y' lands on 'x' */
    CHECK((ins8250_read(&u, 5) & 0x03) == 0x03);
    CHECK(ins8250_read(&u, 0) == 'y');

    /* Received-data interrupt through OUT2. */
    ins8250_write(&u, 1, 0x01);
    to_send[n_to_send++] = 'z';
    ins8250_tick(&u, 1);
    ins8250_tick(&u, 1920);
    CHECK(!irq && ins8250_read(&u, 2) == 0x04);
    ins8250_write(&u, 4, 0x08);
    CHECK(irq);
    CHECK(ins8250_read(&u, 0) == 'z' && !irq && ins8250_read(&u, 2) == 0x01);
}

/* clock_hz 0: every character takes one tick, whatever the divisor. */
static void test_tick_per_char(void) {
    Ins8250 u;
    wire(&u, INS8250_16450, 0);
    ins8250_write(&u, 3, 0x03);
    ins8250_write(&u, 0, 'Q');
    CHECK(n_sent == 0);
    ins8250_tick(&u, 1);
    CHECK(n_sent == 1 && sent[0] == 'Q');
    to_send[n_to_send++] = 'r';
    ins8250_tick(&u, 1);
    ins8250_tick(&u, 1);
    CHECK((ins8250_read(&u, 5) & 0x01) && ins8250_read(&u, 0) == 'r');
}

int main(void) {
    test_scratch();
    test_modem_inputs();
    test_timing();
    test_tick_per_char();
    printf("ins8250: %u checks passed\n", checks);
    return 0;
}
