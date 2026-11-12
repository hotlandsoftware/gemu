#pragma once
/*
 * National ADC0808/ADC0809 8-bit, 8-channel successive-approximation A/D.
 *
 * ALE latches the channel from ADD A-C and START begins a conversion
 * (boards usually tie both to one decoded write strobe); 8 bits of
 * successive approximation take 64 clock periods (a few more until EOC,
 * which the conversion's first clocks hold low).  The result goes to the
 * tri-state output latch at the end of the conversion, so reading early
 * returns the previous result.  Inputs are ratiometric to REF+/REF-: a
 * channel's value here is already in counts (0-255 = REF- .. REF+).
 */
#include <stdbool.h>
#include <stdint.h>

#define ADC0809_CONVERT_CLOCKS 72   /* datasheet maximum: 64 + 8 clocks to EOC */

typedef struct {
    double   in[8];        /* channel inputs, in counts (0.0 - 255.0) */
    uint8_t  result;       /* output latch */
    uint8_t  channel;
    bool     busy;
    uint64_t done_at;      /* bus cycle the conversion completes */
    uint8_t  pending;      /* the value it will latch */
} Adc0809;

void    adc0809_init(Adc0809 *a);
/* ALE + START at bus cycle `now`; the ADC clock is clock_hz, the bus bus_hz. */
void    adc0809_start(Adc0809 *a, uint8_t channel, uint64_t now, double clock_hz, double bus_hz);
uint8_t adc0809_read(Adc0809 *a, uint64_t now);     /* OE */
bool    adc0809_eoc(Adc0809 *a, uint64_t now);
