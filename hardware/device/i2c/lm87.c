/* See lm87.h. Register names, values and rules are the TI SNAS034J
 * datasheet's ("Registers and RAM", "Using the LM87"). */
#include "i2c/lm87.h"

#include <stdio.h>
#include <string.h>

enum {
    R_IHW_HI = 0x13, R_EHW_HI = 0x14, R_TEST = 0x15, R_CHMODE = 0x16,
    R_IHW_HI2 = 0x17, R_EHW_HI2 = 0x18, R_DAC = 0x19,
    R_VALUE_FIRST = 0x20, R_VALUE_LAST = 0x29,      /* conversion results */
    R_COMPANY = 0x3e, R_STEPPING = 0x3f,
    R_CFG1 = 0x40, R_ISR1 = 0x41, R_ISR2 = 0x42, R_IMR1 = 0x43, R_IMR2 = 0x44,
    R_CI_CLEAR = 0x46, R_VIDFAN = 0x47, R_VID4 = 0x49, R_CFG2 = 0x4a,
    R_ISR1_MIRROR = 0x4c, R_ISR2_MIRROR = 0x4d, R_ALERT_ENABLE = 0x80,

    CFG1_MONITOR = 0x01, CFG1_INT_ENABLE = 0x02, CFG1_THERM_ENABLE = 0x04,
    CFG1_RESET = 0x10, CFG1_INITIALIZE = 0x80,
    CFG2_LOCK_INTERNAL = 0x02, CFG2_LOCK_EXTERNAL = 0x04,
    TEST_SHUTDOWN = 0x01,
    MAX_REPORTS = 64,
};

/* Company ID 02h designates the LM87; an ADM1024 in the same socket reads
 * 41h. The datasheet gives stepping 06h for the current part. */
enum { COMPANY_LM87 = 0x02, STEPPING = 0x06 };

static void report(Lm87 *lm87, const char *what, unsigned value) {
    if (!i2c_debug_enabled() || lm87->reports >= MAX_REPORTS)
        return;
    lm87->reports++;
    fprintf(stderr, "lm87: %s (%#04x)\n", what, value);
}

/* Registers with a documented address: 13h-1Bh, 20h-29h, 2Bh-3Ch, 3Eh, 3Fh,
 * 40h-47h, 49h, 4Ah, 4Ch, 4Dh and 80h. 45h is reserved but read/write for
 * backward compatibility. */
static bool documented(uint8_t r) {
    return (r >= 0x13 && r <= 0x1b) || (r >= R_VALUE_FIRST && r <= R_VALUE_LAST) ||
           (r >= 0x2b && r <= 0x3c) || r == R_COMPANY || r == R_STEPPING ||
           (r >= R_CFG1 && r <= R_VIDFAN) || r == R_VID4 || r == R_CFG2 ||
           r == R_ISR1_MIRROR || r == R_ISR2_MIRROR || r == R_ALERT_ENABLE;
}

/* Power-on reset, RESET# and the INITIALIZATION bit clear or initialise these
 * registers; 13h and 14h only while their write-once locks in configuration
 * register 2 are clear. The value RAM and the DAC are untouched. */
static void initialize(Lm87 *lm87) {
    uint8_t *r = lm87->reg;
    if (!(r[R_CFG2] & CFG2_LOCK_INTERNAL)) r[R_IHW_HI] = 0x46;  /* 70 C */
    if (!(r[R_CFG2] & CFG2_LOCK_EXTERNAL)) r[R_EHW_HI] = 0x55;  /* 85 C */
    r[R_TEST] = 0x00;
    r[R_CHMODE] = 0x00;
    r[R_IHW_HI2] = 0x46;
    r[R_EHW_HI2] = 0x55;
    r[R_CFG1] = 0x08;           /* INT# clear is asserted at power on */
    r[R_ISR1] = r[R_ISR2] = r[R_ISR1_MIRROR] = r[R_ISR2_MIRROR] = 0;
    r[R_IMR1] = r[R_IMR2] = 0;
    r[R_CI_CLEAR] = 0;
    r[R_VIDFAN] = 0x50;         /* the VID inputs read 0 */
    r[R_VID4] = 0x80;
    r[R_CFG2] = 0;
    r[R_ALERT_ENABLE] = 0x20;
}

void lm87_reset(Lm87 *lm87) {
    lm87->reg[R_CFG2] = 0;      /* power-on clears the write-once locks first */
    initialize(lm87);
    lm87->reg[R_DAC] = 0xff;    /* power-on sets the DAC output to full scale */
    lm87->reg[R_COMPANY] = COMPANY_LM87;
    lm87->reg[R_STEPPING] = STEPPING;
    lm87->ptr = 0;
}

static uint8_t reg_read(Lm87 *lm87, uint8_t r) {
    if (!documented(r)) {
        report(lm87, "read of an undocumented register", r);
    } else if (r >= R_VALUE_FIRST && r <= R_VALUE_LAST &&
               !lm87->value_reported) {
        lm87->value_reported = true;
        report(lm87, "conversion result read; no inputs are measured", r);
    } else if ((r == R_COMPANY || r == R_STEPPING) && !lm87->id_reported) {
        lm87->id_reported = true;
        report(lm87, "ID read; an ADM1024 would read 41h here", r);
    }
    return lm87->reg[r];
}

static void reg_write(Lm87 *lm87, uint8_t r, uint8_t v) {
    uint8_t *reg = lm87->reg;
    if (!documented(r)) {
        report(lm87, "write to an undocumented register", r);
        return;
    }
    switch (r) {
    case R_IHW_HI:
        if (!(reg[R_CFG2] & CFG2_LOCK_INTERNAL))
            reg[r] = v;
        return;
    case R_EHW_HI:
        if (!(reg[R_CFG2] & CFG2_LOCK_EXTERNAL))
            reg[r] = v;
        return;
    case R_TEST:
        if (v & TEST_SHUTDOWN)
            report(lm87, "shutdown mode set", v);
        reg[r] = v;
        return;
    case R_CFG1:
        if (v & CFG1_INITIALIZE) {      /* self-clearing */
            initialize(lm87);
            return;
        }
        if (v & CFG1_RESET)
            report(lm87, "RESET# output pulse; its wiring is unknown", v);
        reg[r] = v & (uint8_t)~CFG1_RESET;
        if ((v & CFG1_MONITOR) && !lm87->monitor_reported) {
            lm87->monitor_reported = true;
            report(lm87, "monitoring started; no conversions or limit "
                         "interrupts are produced", v);
        }
        if (v & (CFG1_INT_ENABLE | CFG1_THERM_ENABLE))
            report(lm87, "INT#/THERM# output enabled; pin wiring unknown", v);
        return;
    case R_CI_CLEAR:
        if (v & 0x80)
            report(lm87, "chassis intrusion clear pulse", v);
        reg[r] = v & 0x7f;
        return;
    case R_VIDFAN:
        reg[r] = (uint8_t)((v & 0xf0) | (reg[r] & 0x0f));   /* VID3-0 read-only */
        return;
    case R_VID4:
        reg[r] = (uint8_t)((v & 0xfe) | (reg[r] & 0x01));   /* VID4 read-only */
        return;
    case R_CFG2:
        /* Bits 1 and 2 are write-once until power-on or INITIALIZATION; bits
         * 4-5 are reserved. */
        reg[r] = (uint8_t)((v & 0xcf) | (reg[r] & 0x06));
        return;
    case R_ALERT_ENABLE:
        reg[r] = (uint8_t)((v & 0x40) | (reg[r] & ~0x40));  /* only bit 6 */
        if (v & 0x40)
            report(lm87, "SMBALERT# mode enabled; wiring unknown", v);
        return;
    case R_COMPANY: case R_STEPPING:
    case R_ISR1: case R_ISR2: case R_ISR1_MIRROR: case R_ISR2_MIRROR:
        return;                                             /* read-only */
    default:
        if (r >= R_VALUE_FIRST && r <= R_VALUE_LAST)
            return;                                         /* results */
        reg[r] = v;             /* limits, masks, DAC, 16h-18h and 45h */
        return;
    }
}

/* A write transfer is the pointer byte followed by at most one data byte; a
 * read returns the register the pointer addresses. The datasheet describes no
 * auto-increment, so the pointer stays put. */
static bool lm87_start(void *ctx, bool read) {
    (void)read;
    ((Lm87 *)ctx)->bytes_written = 0;
    return true;
}

static bool lm87_write(void *ctx, uint8_t byte) {
    Lm87 *lm87 = ctx;
    if (lm87->bytes_written == 0)
        lm87->ptr = byte;
    else if (lm87->bytes_written == 1)
        reg_write(lm87, lm87->ptr, byte);
    else
        report(lm87, "extra data byte; auto-increment is not documented", byte);
    lm87->bytes_written++;
    return true;
}

static uint8_t lm87_read(void *ctx, bool ack) {
    (void)ack;
    Lm87 *lm87 = ctx;
    return reg_read(lm87, lm87->ptr);
}

static void lm87_stop(void *ctx) {
    ((Lm87 *)ctx)->bytes_written = 0;
}

I2cDevice lm87_device(Lm87 *lm87, uint8_t addr7) {
    return (I2cDevice){ .addr = addr7, .ctx = lm87, .start = lm87_start,
                        .write = lm87_write, .read = lm87_read,
                        .stop = lm87_stop };
}
