#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8253 Programmable Interval Timer - 3 channels, ports 0x40-0x43.
 * Channel 0 drives IRQ0 (the system timer tick BIOS/DOS rely on for
 * delay loops and the time-of-day count), channel 1 is DRAM refresh
 * (not modeled - nothing reads it), channel 2 gates the PC speaker.
 *
 * Simplification: every mode is treated as a periodic reload-on-underflow
 * counter (real mode 2/3 behavior). Modes 0/1/4/5's one-shot/retrigger
 * distinctions aren't modeled - BIOS POST and DOS-era timing code only
 * ever programs channel 0 in mode 2 or 3 in practice, so this covers the
 * cases that matter without the full mode state machine. */

typedef struct {
    uint16_t reload;
    uint16_t counter;
    uint8_t  mode;      /* 0-5, from the command byte - stored but not
                          * behaviorally distinguished beyond periodic reload */
    uint8_t  rw_mode;   /* 1=LSB only, 2=MSB only, 3=LSB then MSB */
    bool     write_msb_next;
    bool     read_msb_next;
    uint16_t latched_value;
    bool     latched;
    bool     out_fired; /* set on underflow, cleared by the caller after reading */
} I8253Channel;

typedef struct {
    I8253Channel ch[3];
} I8253;

void    i8253_reset(I8253 *p);
uint8_t i8253_io_read(I8253 *p, uint16_t port);
void    i8253_io_write(I8253 *p, uint16_t port, uint8_t val);

/* Advances every channel's counter by `ticks` PIT clocks (the caller's
 * frame loop converts elapsed instructions/wall time into PIT ticks at
 * ~1.193182 MHz). Returns true if channel 0 underflowed this call - the
 * caller should raise IRQ0 on the PIC when it does. */
bool i8253_tick(I8253 *p, uint32_t ticks);

/* Current channel 2 output level - gates the PC speaker. */
bool i8253_ch2_out(const I8253 *p);
