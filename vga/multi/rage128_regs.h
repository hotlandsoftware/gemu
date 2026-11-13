#pragma once

#include <stdint.h>

/* Register table of the ATI RAGE 128 VR/GL (Register Reference Guide
 * RRG-R04100): memory-mapped registers by MMR offset, and the indirect PLL
 * registers (reached through CLOCK_CNTL_INDEX/DATA) by PLL index. */
typedef struct Rage128RegInfo {
    const char *name;
    uint16_t offset;         /* MMR offset, or PLL index */
    char access;             /* 'B' read/write, 'R' read-only, 'W' write-only */
    uint8_t bits;
    uint32_t reset;          /* from the field defaults */
    uint32_t reset_unknown;  /* bits whose default is not a number */
} Rage128RegInfo;

#define RAGE128_NREGS 288
#define RAGE128_NPLL 18
extern const Rage128RegInfo rage128_regs[RAGE128_NREGS];     /* sorted by offset */
extern const Rage128RegInfo rage128_pll_regs[RAGE128_NPLL];  /* sorted by index */
