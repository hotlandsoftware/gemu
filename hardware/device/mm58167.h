#pragma once
/*
 * National Semiconductor MM58167A microprocessor real-time clock.
 *
 * A 32.768 kHz crystal drives BCD counters for thousandths of a second
 * up to the month; 32 registers on A0-A4:
 *
 *   00  thousandths of seconds (upper digit; the lower reads 0)
 *   01  hundredths and tenths      02  seconds        03  minutes
 *   04  hours (24-hour)            05  day of week (1-7)
 *   06  day of month (1-31)        07  month (1-12)
 *   08-0F  RAM, also the alarm compare latches for counters 00-07
 *          (a latch with its top two bits set is "don't care")
 *   10  interrupt status (R)       11  interrupt control (R/W)
 *   12  counter reset (W: a 1 bit clears that counter, 00 = bit 0)
 *   13  RAM reset (W, same bits)   14  status bit (R: a counter changed
 *                                      during the read cycle)
 *   15  GO: clears the thousandths through seconds counters
 *   16  standby interrupt enable (bit 0)
 *   1F  test mode
 *
 * Interrupt sources (status/control bits): 0 alarm compare, 1 tenth of
 * a second, 2 second, 3 minute, 4 hour, 5 day, 6 week, 7 month.  Reading
 * the status register clears it.
 *
 * Counters are BCD digit chains: each digit carries only from its last
 * value (9, or 5 for the tens of minutes...), so a value written outside
 * BCD counts on through the hex digits before wrapping, as BCD counter
 * logic does.  February counts 28 days (the chip has no year counter).
 *
 * The status bit flags a counter changing while the CPU's read strobe is
 * active.  Here a read is instantaneous at the cycle the bus reaches, so
 * the counters never change under it and register 14 reads 0.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  cnt[8];        /* counters 00-07, BCD as read */
    uint8_t  ram[8];
    uint8_t  int_status, int_control;
    bool     standby_int;
    bool     int_out;       /* INTERRUPT OUTPUT pin */
    uint32_t sub;           /* crystal ticks x 1000 toward the next millisecond */
    uint64_t last_cycle;    /* host bus cycle the crystal was last advanced to */
} Mm58167;

void    mm58167_init(Mm58167 *r);
/* Set the counters from a calendar time (the battery kept it running). */
void    mm58167_set_time(Mm58167 *r, int month, int mday, int wday, int hour, int min, int sec);
/* Advance the crystal to `cycle` of a bus clocked at bus_hz. */
void    mm58167_sync(Mm58167 *r, uint64_t cycle, double bus_hz);
uint8_t mm58167_read(Mm58167 *r, uint8_t reg);
uint8_t mm58167_peek(const Mm58167 *r, uint8_t reg);   /* no side effects */
void    mm58167_write(Mm58167 *r, uint8_t reg, uint8_t val);
