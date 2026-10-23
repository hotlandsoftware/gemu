#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8237A DMA controller as used on the IBM PC/XT - ports 0x00-0x0F,
 * plus the 74LS670 page registers at 0x81-0x83 (channel 2, 3, 1; the 5150
 * has no channel 0 page register).
 *
 * Channel 0 performs DRAM refresh, triggered by PIT channel 1: each
 * request is a dummy transfer that steps the address and count exactly as
 * the real chip does, so software reading them back sees them move.
 * Channels 1-3 serve expansion cards; i8237_transfer() runs one cycle of
 * them for a device that has data to move. */

typedef struct {
    uint16_t base_addr, base_count;
    uint16_t cur_addr, cur_count;
    uint8_t  mode;
    uint8_t  page;
} I8237Channel;

typedef struct {
    I8237Channel ch[4];
    uint8_t command;
    uint8_t status;     /* bits 0-3: terminal count reached, 4-7: request */
    uint8_t mask;       /* bits 0-3: channel masked */
    uint8_t request;    /* software requests */
    uint8_t temp;
    bool    flipflop;   /* false = next byte is the low byte */
} I8237;

void    i8237_reset(I8237 *d);
uint8_t i8237_io_read(I8237 *d, uint16_t port);
void    i8237_io_write(I8237 *d, uint16_t port, uint8_t val);
uint8_t i8237_page_read(I8237 *d, uint16_t port);    /* 0x80-0x83 */
void    i8237_page_write(I8237 *d, uint16_t port, uint8_t val);

/* One DRAM refresh request on channel 0. */
void    i8237_refresh(I8237 *d);

/* One transfer cycle on channel ch (1-3): returns false if the channel is
 * masked or the controller disabled, otherwise sets *phys to the 20-bit
 * memory address to move a byte to/from (the mode register says which way)
 * and *tc when this was the final byte. */
bool    i8237_transfer(I8237 *d, unsigned ch, uint32_t *phys, bool *tc);
