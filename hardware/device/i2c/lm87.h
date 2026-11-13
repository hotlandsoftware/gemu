#pragma once

/* National (TI) LM87 serial interface system hardware monitor (datasheet
 * SNAS034J). The BS460GX baseboard's "Heceta" monitor is initialised by
 * firmware through registers 13h/14h (hardware high limits) and 37h-3Ah,
 * which is the LM87 register map. The ADM1024 is register-compatible.
 *
 * Modelled: the register file and its access rules (address pointer, power-on
 * and INITIALIZATION values, read-only and write-once registers,
 * self-clearing bits). Not modelled: the monitoring loop's measurements and
 * the INT#/THERM#/RESET#/CI pins, because the rails, diodes and fans wired to
 * each input are undocumented. Set I2C_DEBUG to report accesses that depend on
 * those. */

#include "i2c/i2c.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Lm87 {
    uint8_t reg[256];
    uint8_t ptr;                    /* address pointer register */
    unsigned bytes_written;         /* in the current transfer */
    bool id_reported, monitor_reported, value_reported;
    unsigned reports;
} Lm87;

/* Power-on reset: the registers the datasheet gives values for. The value RAM
 * is indeterminate and keeps what it held. */
void lm87_reset(Lm87 *lm87);
/* The bus attachment at `addr7`: 2Ch, 2Dh or 2Eh per the ADD pin (open, Vcc,
 * ground). */
I2cDevice lm87_device(Lm87 *lm87, uint8_t addr7);
