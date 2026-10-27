/*
 * ADC0809 - see adc0809.h.
 */
#include "adc0809.h"
#include <math.h>
#include <string.h>

void adc0809_init(Adc0809 *a) {
    memset(a, 0, sizeof(*a));
}

static void finish(Adc0809 *a, uint64_t now) {
    if (a->busy && now >= a->done_at) {
        a->result = a->pending;
        a->busy = false;
    }
}

void adc0809_start(Adc0809 *a, uint8_t channel, uint64_t now, double clock_hz, double bus_hz) {
    finish(a, now);
    a->channel = channel & 7u;
    /* The comparator samples the input as the conversion runs; the input
     * is taken at START, the 1/2 LSB offset making it round to nearest. */
    double v = a->in[a->channel];
    long code = lround(v);
    a->pending = (uint8_t)(code < 0 ? 0 : code > 255 ? 255 : code);
    a->busy = true;
    a->done_at = now + (uint64_t)ceil(ADC0809_CONVERT_CLOCKS * bus_hz / clock_hz);
}

uint8_t adc0809_read(Adc0809 *a, uint64_t now) {
    finish(a, now);
    return a->result;
}

bool adc0809_eoc(Adc0809 *a, uint64_t now) {
    finish(a, now);
    return !a->busy;
}
