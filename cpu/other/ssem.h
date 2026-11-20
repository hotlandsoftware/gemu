#pragma once
/*
 * SSEM - the Manchester Baby's processor (1948).
 *
 * The store is 32 lines of 32 bits.  An instruction uses two fields of its
 * line: bits 0-4, the operand line S, and bits 13-15, the function number.
 * Bit 0 is the least significant (the Baby showed it on the left).
 *
 * The control holds CI, the control instruction (the line of the
 * instruction executed last), and while the machine runs PI, the present
 * instruction.  Each step adds 1 to CI first, then fetches PI from that
 * line and obeys it - so a program starts at line 1 with CI cleared.
 *
 *   0 JMP S  CI = S             (S: the content of line S)
 *   1 JRP S  CI = CI + S
 *   2 LDN S  A = -S
 *   3 STO S  S = A
 *   4 SUB S  A = A - S
 *   5 SUB S  same as 4
 *   6 CMP    if A < 0, CI = CI + 1  (skip the next instruction)
 *   7 STP    stop, and light the stop lamp
 *
 * All arithmetic is 32-bit two's complement.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSEM_LINES 32

#define SSEM_LINE(w) ((unsigned)(w) & 31u)          /* bits 0-4 */
#define SSEM_FUNC(w) (((unsigned)(w) >> 13) & 7u)   /* bits 13-15 */
#define SSEM_INSN(func, line) ((uint32_t)(((func) & 7u) << 13 | ((line) & 31u)))

typedef enum {
    SSEM_JMP, SSEM_JRP, SSEM_LDN, SSEM_STO, SSEM_SUB, SSEM_SUB5, SSEM_CMP, SSEM_STP
} SsemFunc;

typedef struct {
    uint32_t store[SSEM_LINES];
    uint32_t a;        /* accumulator */
    uint32_t ci;       /* control instruction */
    uint32_t pi;       /* present instruction (shown only while running) */
    bool     stopped;  /* obeyed STP: the stop lamp is lit */
    uint64_t steps;    /* instructions obeyed since the last clear */
} Ssem;

/* Obey one instruction as it stands (no fetch) - also how the panel's
 * manual instruction (F-stat/L-stat with KC) runs. */
void ssem_obey(Ssem *m, uint32_t insn);
/* One machine cycle: CI = CI + 1, PI = store[CI], obey PI.  False once the
 * machine has stopped (STP). */
bool ssem_step(Ssem *m);

const char *ssem_mnemonic(unsigned func);
/* "LDN 24", "CMP", "STP" */
void ssem_disasm(uint32_t insn, char *buf, size_t len);
