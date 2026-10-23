#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8253 Programmable Interval Timer - 3 channels, ports 0x40-0x43,
 * clocked at 1.193182 MHz on the PC (the 4.77 MHz CPU clock / 4).
 * Channel 0 drives IRQ0 (the system timer tick), channel 1 requests DRAM
 * refresh DMA, channel 2 drives the speaker and is gated by PPI port B.
 *
 * All six counting modes are modelled at PIT-clock granularity, including
 * gate inputs, the one-clock delay before a newly written count takes
 * effect, and counter latching.  BCD counting is accepted but counts in
 * binary (nothing on the PC uses it). */

typedef struct {
    uint16_t count;          /* counting element (0 = 65536) */
    uint16_t reload;         /* count register, as last written */
    uint8_t  mode;           /* 0-5 */
    uint8_t  rw_mode;        /* 1=LSB only, 2=MSB only, 3=LSB then MSB */
    bool     out;            /* output pin level */
    bool     gate;           /* gate input level */
    bool     counting;       /* a count has been loaded into the element */
    bool     load_pending;   /* reload -> count on the next clock */
    bool     trigger;        /* gate rising edge seen, act on the next clock */
    bool     strobe;         /* modes 4/5: output is low for this one clock */
    bool     write_msb_next;
    bool     read_msb_next;
    bool     latched;
    uint16_t latched_value;
} I8253Channel;

typedef struct {
    I8253Channel ch[3];
    /* Called whenever a channel's output pin changes level. */
    void (*out_changed)(void *ud, unsigned ch, bool level);
    void  *ud;
} I8253;

void    i8253_reset(I8253 *p);
uint8_t i8253_io_read(I8253 *p, uint16_t port);
void    i8253_io_write(I8253 *p, uint16_t port, uint8_t val);

/* Advance every channel by `ticks` PIT clocks. */
void    i8253_tick(I8253 *p, uint32_t ticks);

void    i8253_set_gate(I8253 *p, unsigned ch, bool level);
bool    i8253_out(const I8253 *p, unsigned ch);
