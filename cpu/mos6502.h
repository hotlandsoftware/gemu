#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Status register (P) bits: NV-BDIZC */
#define MOS6502_P_C  0x01u  /* Carry */
#define MOS6502_P_Z  0x02u  /* Zero */
#define MOS6502_P_I  0x04u  /* Interrupt disable */
#define MOS6502_P_D  0x08u  /* Decimal */
#define MOS6502_P_B  0x10u  /* Break (set on push; not stored in live P) */
#define MOS6502_P_U  0x20u  /* Unused — always reads 1 */
#define MOS6502_P_V  0x40u  /* Overflow */
#define MOS6502_P_N  0x80u  /* Negative */

typedef struct Mos6502 {
    uint8_t  A;     /* Accumulator */
    uint8_t  X;     /* X index register */
    uint8_t  Y;     /* Y index register */
    uint8_t  SP;    /* Stack pointer (hardware stack is 0x0100–0x01FF) */
    uint16_t PC;    /* Program counter */
    uint8_t  P;     /* Status flags (NV-BDIZC) */

    uint64_t cycle_count;
    uint64_t insn_count;

    uint8_t (*mem_read) (uint16_t addr, void *ud);
    void    (*mem_write)(uint16_t addr, uint8_t val, void *ud);
    void    *mem_ud;

    bool irq;             /* level-sensitive: true = IRQ line asserted */
    bool nmi;             /* edge-triggered: set to true to request NMI */
    bool decimal_disable; /* true = ignore D flag in ADC/SBC (Ricoh 2A03 behaviour) */
} Mos6502;

void mos6502_init (Mos6502 *cpu);
void mos6502_reset(Mos6502 *cpu);
void mos6502_step (Mos6502 *cpu);
